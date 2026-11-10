/**
 * physics.cpp — 自行车模型积分实现（运动学 + 动力学两版）
 *
 * 数值方法：前向欧拉积分，dt=0.05s（20Hz）。
 * 纵向力模型两版共用（与 sim_world_node.c vehicle_tick() 一致，行为不退化）：
 *   drive_force  = throttle * 5000 N
 *   brake_force  = brake    * 8000 N
 *   drag_force   = drag_coeff * speed²
 *   accel        = (drive - brake - drag) / mass
 *
 * 横向：
 *   运动学 step_bicycle —— d(heading)/dt = (speed/wheelbase)·tan(steer)
 *   动力学 step_bicycle_dynamic —— 线性轮胎二自由度（侧向速度 v_y + 横摆角速度 r）：
 *     α_f = steer − atan2(v_y + a·r, v_x)      前轴滑移角
 *     α_r =       − atan2(v_y − b·r, v_x)      后轴滑移角
 *     F_yf = Cαf·α_f,  F_yr = Cαr·α_r          线性轮胎侧向力
 *     v_y' = (F_yf + F_yr)/m − v_x·r
 *     r'   = (a·F_yf − b·F_yr)/Iz
 *   低速（v_x < LOW_SPEED_MS）退化运动学：前向欧拉在此区间对横向刚度不稳定，
 *   且低速轮胎滑移可忽略，运动学既够用又更稳。边界详见 docs/CALIBRATION_GUIDE.md。
 */

#include "physics.h"

#include <cmath>

namespace flowsim {

/* 动力学模型低于此纵向车速退化运动学（前向欧拉稳定域下界 + 低速滑移可忽略） */
static constexpr double LOW_SPEED_MS = 5.0;
/* 质心到前/后轴距离占轴距的比例（质心略偏前，等刚度下呈不足转向） */
static constexpr double CG_FRONT_FRAC = 0.45;
/* 滑移角饱和：线性轮胎仅在小滑移角有效，超出近似摩擦极限而饱和。
 * 既贴近真实轮胎（friction circle），又保证任意控制输入下前向欧拉有界不发散。 */
static constexpr double SLIP_ANGLE_MAX = 0.12;
static constexpr double GRAV_CONST = 9.81;

/* 悬架二阶弹簧阻尼模型常数（右手定则 ENU 车体系：x前 y左 z上）
 * 俯仰：加速抬头 pitch<0，制动点头 pitch>0；
 * 侧倾：左转离心力向右 roll>0，右转左倾 roll<0。 */
static constexpr double SUSP_K_PITCH   = 100000.0;  // 俯仰等效刚度 (N·m/rad)
static constexpr double SUSP_C_PITCH   = 35000.0;   // 俯仰阻尼 (N·m·s/rad，过阻尼 ζ≈1.1)
static constexpr double SUSP_I_PITCH   = 2500.0;    // 绕 Y 轴转动惯量 (kg·m²)
static constexpr double SUSP_K_ROLL    = 80000.0;   // 侧倾等效刚度 (N·m/rad，含防倾杆)
static constexpr double SUSP_C_ROLL    = 16000.0;   // 侧倾阻尼 (N·m·s/rad，过阻尼 ζ≈1.1)
static constexpr double SUSP_I_ROLL    = 700.0;     // 绕 X 轴转动惯量 (kg·m²)
static constexpr double SUSP_ANGLE_MAX = 0.15;      // 悬架物理限位行程 (rad，约 8.6°)

static void update_axle_loads(Entity& e, double* fz_f_clamped = nullptr, double* fz_r_clamped = nullptr) {
    // 动态轴荷转移与对称守恒 (F_zf + F_zr = mg)
    double a = CG_FRONT_FRAC * e.wheelbase;
    double b = e.wheelbase - a;
    double delta_fz = e.mass * e.a_x_body * e.cg_height / e.wheelbase;
    double fz_f_static = e.mass * GRAV_CONST * b / e.wheelbase;
    double fz_r_static = e.mass * GRAV_CONST * a / e.wheelbase;
    e.F_zf = fz_f_static - delta_fz;
    e.F_zr = fz_r_static + delta_fz;
    if (fz_f_clamped) *fz_f_clamped = std::max(e.F_zf, 0.05 * fz_f_static);
    if (fz_r_clamped) *fz_r_clamped = std::max(e.F_zr, 0.05 * fz_r_static);
}

static void update_suspension(Entity& e, double dt) {
    update_axle_loads(e);

    // 俯仰二阶动力学 (半隐式欧拉在 dt=0.05s 下稳定不发散)
    double M_p = -e.mass * e.a_x_body * e.cg_height;
    double p_acc = (M_p - SUSP_C_PITCH * e.pitch_rate - SUSP_K_PITCH * e.pitch) / SUSP_I_PITCH;
    e.pitch_rate += p_acc * dt;
    e.pitch += e.pitch_rate * dt;
    if (e.pitch >  SUSP_ANGLE_MAX) e.pitch =  SUSP_ANGLE_MAX;
    if (e.pitch < -SUSP_ANGLE_MAX) e.pitch = -SUSP_ANGLE_MAX;

    // 侧倾二阶动力学
    double M_r = e.mass * e.a_y_body * e.cg_height;
    double r_acc = (M_r - SUSP_C_ROLL * e.roll_rate - SUSP_K_ROLL * e.roll) / SUSP_I_ROLL;
    e.roll_rate += r_acc * dt;
    e.roll += e.roll_rate * dt;
    if (e.roll >  SUSP_ANGLE_MAX) e.roll =  SUSP_ANGLE_MAX;
    if (e.roll < -SUSP_ANGLE_MAX) e.roll = -SUSP_ANGLE_MAX;
}

static inline double clamp_slip(double a) {
    if (a >  SLIP_ANGLE_MAX) return  SLIP_ANGLE_MAX;
    if (a < -SLIP_ANGLE_MAX) return -SLIP_ANGLE_MAX;
    return a;
}

/* 纵向加速度：两模型共用（净力/质量）。
 * brake/drag 是耗散力，方向恒与运动方向相反——旧实现符号固定为负，
 * 倒车（v<0）时刹车/风阻反而把车往更负速度推（越刹越快倒车）。
 * v==0 时刹车不产生运动（静摩擦），只有 drive 起作用。 */
static double longitudinal_accel(const Entity& e, double throttle, double brake, double v) {
    double drive_force = throttle * 5000.0;
    double sgn = (v > 0.0) ? 1.0 : (v < 0.0 ? -1.0 : 0.0);
    double brake_force = brake * 8000.0 * sgn;
    double drag_force  = e.drag_coeff * v * std::fabs(v);
    return (drive_force - brake_force - drag_force) / e.mass;
}

/* 转向执行器：一阶滞后 (τ≈0.15s EPS) + 速率限幅，写回 e.steer。两模型共用 */
static void update_steer(Entity& e, double steer_cmd, double dt) {
    /* 正常行驶限幅 0.25rad（≈14°），掉头时（steer_override=true）放宽到 0.60rad（≈34°）。
     * 宽路掉头打一圈多（~0.50rad），窄路窄路打死方向盘（~0.55-0.60rad），
     * 详见 Python 扫描结果。 */
    const double steer_limit = e.steer_override ? 0.60 : 0.25;
    if (steer_cmd >  steer_limit) steer_cmd =  steer_limit;
    if (steer_cmd < -steer_limit) steer_cmd = -steer_limit;

    double alpha = dt / (e.steer_tau + dt);
    double steer_next = e.steer + alpha * (steer_cmd - e.steer);

    double max_rate = e.steer_rate_max * dt;
    double d = steer_next - e.steer;
    if (d >  max_rate) steer_next = e.steer + max_rate;
    if (d < -max_rate) steer_next = e.steer - max_rate;
    e.steer = steer_next;
}

static void normalize_heading(Entity& e) {
    while (e.heading >  M_PI) e.heading -= 2.0 * M_PI;
    while (e.heading < -M_PI) e.heading += 2.0 * M_PI;
}

/* Pacejka 96 魔术公式侧向力（乘用车标定见 tools/tire_dynamics_sim.py --tune）：
 *   F_y = D·sin(C·atan(B·α − E·(B·α − atan(B·α))))
 *   D   = μ·Fz（峰值受附着系数限制，μ 可降以模拟湿滑路面）
 *   Fz  = 静态轴荷（质心偏前，前/后轴按 CG_FRONT_FRAC 分配）
 * 与线性模型不同：α 不硬饱和，由魔术公式自身平滑进入饱和，更贴近真实摩擦圆。 */
static double pacejka_lateral_force(double alpha, double mu, double Fz,
                                    double B, double C, double E) {
    double Bx = B * alpha;
    double y  = std::sin(C * std::atan(Bx - E * (Bx - std::atan(Bx))));
    return (mu * Fz) * y;
}

/* 二自由度横向动力学积分（Pacejka 版）：更新 v_y_body / yaw_rate / heading，
 * 并把车体系速度投影回世界系 x/y。框架与 integrate_lateral_dynamics 一致，
 * 仅轮胎侧向力由魔术公式给出（不硬饱和滑移角）。 */
static void integrate_lateral_dynamics_pacejka(Entity& e, double dt) {
    double a  = CG_FRONT_FRAC * e.wheelbase;          // 质心→前轴
    double b  = e.wheelbase - a;                      // 质心→后轴
    double vx = e.v_x_body;

    // 滑移角（不饱和，交给魔术公式）
    double alpha_f = e.steer - std::atan2(e.v_y_body + a * e.yaw_rate, vx);
    double alpha_r =        - std::atan2(e.v_y_body - b * e.yaw_rate, vx);

    // 前/后轴动态垂直载荷（含动态纵向轴荷转移 ΔFz 与正值下界夹紧）
    double Fz_f = 0.0, Fz_r = 0.0;
    update_axle_loads(e, &Fz_f, &Fz_r);

    double mu_effective = e.pacejka_mu * (e.surface_mu_scale > 0.0 ? e.surface_mu_scale : 1.0);

    e.F_yf = pacejka_lateral_force(alpha_f, mu_effective, Fz_f,
                                   e.pacejka_b, e.pacejka_c, e.pacejka_e);
    e.F_yr = pacejka_lateral_force(alpha_r, mu_effective, Fz_r,
                                   e.pacejka_b, e.pacejka_c, e.pacejka_e);

    double vy_dot = (e.F_yf + e.F_yr) / e.mass - vx * e.yaw_rate;
    double r_dot  = (a * e.F_yf - b * e.F_yr) / e.yaw_inertia;

    e.v_y_body += vy_dot * dt;
    e.yaw_rate += r_dot  * dt;
    e.a_y_body = (e.F_yf + e.F_yr) / e.mass;

    /* 摩擦圆护栏（与线性模型一致）：|v_y| ≤ vx·tan(slip_max)·1.5,
     * |r| ≤ μ·g/vx。Pacejka 峰值本身受 μ·Fz 限制，护栏仅兜底非线性输入。 */
    const double max_vy = std::fabs(vx) * std::tan(SLIP_ANGLE_MAX) * 1.5;
    if (std::fabs(e.v_y_body) > max_vy) e.v_y_body = std::copysign(max_vy, e.v_y_body);
    const double max_r = (0.8 * GRAV_CONST) / std::max(vx, 1.0);
    if (std::fabs(e.yaw_rate) > max_r) e.yaw_rate = std::copysign(max_r, e.yaw_rate);

    e.heading  += e.yaw_rate * dt;
    normalize_heading(e);

    // 车体系 (v_x, v_y) → 世界系
    double ch = std::cos(e.heading), sh = std::sin(e.heading);
    e.vx = e.v_x_body * ch - e.v_y_body * sh;
    e.vy = e.v_x_body * sh + e.v_y_body * ch;
    e.x += e.vx * dt;
    e.y += e.vy * dt;
    e.speed = std::sqrt(e.v_x_body * e.v_x_body + e.v_y_body * e.v_y_body);
}

void step_bicycle(Entity& e, double dt, double throttle, double brake, double steer) {
    double grade_term = (e.road_grade != 0.0) ? (GRAV_CONST * std::sin(e.road_grade)) : 0.0;
    double acc = longitudinal_accel(e, throttle, brake, e.speed) - grade_term;
    e.a_x_body = acc;
    e.speed += acc * dt;
    /* 倒车只允许显式负油门触发（三把方向掉头相位）。
     * 普通制动若穿过 0，必须钳回静止，不能误变成持续倒车。 */
    if (e.speed < 0.0 && throttle >= 0.0) e.speed = 0.0;
    if (e.speed < -4.0) e.speed = -4.0;
    if (e.speed >  60.0) e.speed = 60.0;

    update_steer(e, steer, dt);

    // ── 运动学 yaw_rate（后轴参考点）──
    e.yaw_rate = (e.speed / e.wheelbase) * std::tan(e.steer);
    e.a_y_body = e.speed * e.yaw_rate;
    e.heading += e.yaw_rate * dt;
    normalize_heading(e);

    // ── 车辆中心为参考点（非后轴）──
    double half_wb = e.wheelbase * 0.5;
    double vx_rear = e.speed * std::cos(e.heading);
    double vy_rear = e.speed * std::sin(e.heading);
    e.x += vx_rear * dt - half_wb * std::sin(e.heading) * e.yaw_rate * dt;
    e.y += vy_rear * dt + half_wb * std::cos(e.heading) * e.yaw_rate * dt;

    // 世界系速度（车辆中心，供 perception/sensor 使用）
    e.vx = vx_rear - half_wb * std::sin(e.heading) * e.yaw_rate;
    e.vy = vy_rear + half_wb * std::cos(e.heading) * e.yaw_rate;

    update_suspension(e, dt);
}

void step_pedestrian(Entity& e, double dt) {
    e.x += e.vx * dt;
    e.y += e.vy * dt;
    e.speed = std::sqrt(e.vx * e.vx + e.vy * e.vy);
}

/* 线性轮胎二自由度横向动力学积分：更新 v_y_body / yaw_rate / heading，
 * 并把车体系速度投影回世界系 x/y。调用前 e.v_x_body 应为当前纵向车速。 */
static void integrate_lateral_dynamics(Entity& e, double dt) {
    double a  = CG_FRONT_FRAC * e.wheelbase;          // 质心→前轴
    double b  = e.wheelbase - a;                      // 质心→后轴
    double vx = e.v_x_body;

    double alpha_f = clamp_slip(e.steer - std::atan2(e.v_y_body + a * e.yaw_rate, vx));
    double alpha_r = clamp_slip(       - std::atan2(e.v_y_body - b * e.yaw_rate, vx));

    e.F_yf = e.tire_stiffness_f * alpha_f;            // 线性轮胎 F_y = Cα·α（饱和后）
    e.F_yr = e.tire_stiffness_r * alpha_r;

    double vy_dot = (e.F_yf + e.F_yr) / e.mass - vx * e.yaw_rate;
    double r_dot  = (a * e.F_yf - b * e.F_yr) / e.yaw_inertia;

    e.v_y_body += vy_dot * dt;
    e.yaw_rate += r_dot  * dt;
    e.a_y_body = (e.F_yf + e.F_yr) / e.mass;

    /* 稳定性护栏：线性轮胎 + 滑移角硬饱和下，高速持续转向时 v_y 经陀螺项
     * −vx·r 正反馈而发散（2026-07 PR #72 实测：≥20 m/s 持续转向必然发散，
     * v_y_body 可涨到 5×10⁴ m/s，推翻"有界不发散"承诺）。施加物理上界：
     *   - 滑移约束  |v_y| ≤ vx·tan(slip_max)·1.5（正常工况远低于此）
     *   - 摩擦圆近似 |r| ≤ μ·g/vx（μ≈0.8），横向加速度 ≤ 0.8g
     * 只在失控/极端输入时钳位，不影响正常操控。 */
    const double max_vy = std::fabs(vx) * std::tan(SLIP_ANGLE_MAX) * 1.5;
    if (std::fabs(e.v_y_body) > max_vy) e.v_y_body = std::copysign(max_vy, e.v_y_body);
    const double max_r = (0.8 * GRAV_CONST) / std::max(vx, 1.0);
    if (std::fabs(e.yaw_rate) > max_r) e.yaw_rate = std::copysign(max_r, e.yaw_rate);

    e.heading  += e.yaw_rate * dt;
    normalize_heading(e);

    // 车体系 (v_x, v_y) → 世界系
    double ch = std::cos(e.heading), sh = std::sin(e.heading);
    e.vx = e.v_x_body * ch - e.v_y_body * sh;
    e.vy = e.v_x_body * sh + e.v_y_body * ch;
    e.x += e.vx * dt;
    e.y += e.vy * dt;
    e.speed = std::sqrt(e.v_x_body * e.v_x_body + e.v_y_body * e.v_y_body);
}

void step_bicycle_dynamic(Entity& e, double dt, double throttle, double brake, double steer) {
    // 低速退化：运动学既够用又稳（前向欧拉对高横向刚度在低速发散）
    if (e.speed < LOW_SPEED_MS) {
        step_bicycle(e, dt, throttle, brake, steer);
        e.v_x_body = e.speed;                         // 供下一帧跨过阈值时无缝接续
        e.v_y_body = 0.0;
        return;
    }

    // 纵向车速积分：用 v_x_body 自身而非总速 e.speed
    if (e.v_x_body <= 0.0 && e.speed > 0.0) e.v_x_body = e.speed;
    double grade_term = (e.road_grade != 0.0) ? (GRAV_CONST * std::sin(e.road_grade)) : 0.0;
    double acc = longitudinal_accel(e, throttle, brake, e.v_x_body) - grade_term;
    e.a_x_body = acc;
    e.v_x_body += acc * dt;
    if (e.v_x_body < 0.0) e.v_x_body = 0.0;

    update_steer(e, steer, dt);
    integrate_lateral_dynamics(e, dt);               // 内部把 e.speed 更新为合速度大小
    update_suspension(e, dt);
}

void step_bicycle_dynamic_pacejka(Entity& e, double dt, double throttle, double brake, double steer) {
    // 低速退化：运动学既够用又稳（前向欧拉对高横向刚度在低速发散）
    if (e.speed < LOW_SPEED_MS) {
        step_bicycle(e, dt, throttle, brake, steer);
        e.v_x_body = e.speed;
        e.v_y_body = 0.0;
        return;
    }
    // 入口同步 + 纵向解耦积分（与 step_bicycle_dynamic 同构）
    if (e.v_x_body <= 0.0 && e.speed > 0.0) e.v_x_body = e.speed;
    double grade_term = (e.road_grade != 0.0) ? (GRAV_CONST * std::sin(e.road_grade)) : 0.0;
    double acc = longitudinal_accel(e, throttle, brake, e.v_x_body) - grade_term;
    e.a_x_body = acc;
    e.v_x_body += acc * dt;
    if (e.v_x_body < 0.0) e.v_x_body = 0.0;

    update_steer(e, steer, dt);
    integrate_lateral_dynamics_pacejka(e, dt);
    update_suspension(e, dt);
}

void apply_vehicle_defaults(Entity& e) {
    /* 执行器滞后参数：所有车辆共用默认值 */
    e.steer_tau = 0.15;
    e.steer_rate_max = 0.6;
    e.surface_mu_scale = 1.0;
    e.road_grade = 0.0;

    /* Pacejka 魔术公式参数（physics_model=pacejka 时才被读取；运动学/线性模式
     * 无视）。乘用车标定扫描见 tools/tire_dynamics_sim.py --tune：
     *   B=8.0 C=1.2 E=-0.2 μ=0.7（干燥沥青，峰值横向加速度 ~0.7g）。
     * μ 为峰值附着系数，可降以模拟湿滑/积雪路面。所有车型共用（真实车差异在
     * 载荷 Fz 与 yaw_inertia，B/C/E 对乘用车差异不大）。 */
    e.pacejka_b = 8.0;
    e.pacejka_c = 1.2;
    e.pacejka_e = -0.2;
    e.pacejka_mu = 0.7;

    /* 动力学模型参数（physics_model=dynamic 时才被读取；运动学模式无视）：
     *   tire_stiffness_{f,r} —— 每轴等效侧偏刚度 (N/rad)，线性轮胎 F_y = Cα·α
     *   yaw_inertia          —— 绕 Z 轴转动惯量 Iz (kg·m²)
     * 量级取自乘用车/SUV/卡车典型标定值（见 docs/CALIBRATION_GUIDE.md）。 */
    switch (e.type) {
        case EntityType::Truck:
            e.length = 8.0;  e.width = 2.4;
            e.wheelbase = 5.0;
            e.mass = 8000.0;
            e.drag_coeff = 0.6;
            e.max_brake = 3.0;
            e.cg_height = 1.10;
            e.tire_stiffness_f = 180000.0;
            e.tire_stiffness_r = 180000.0;
            e.yaw_inertia = 25000.0;
            break;
        case EntityType::SUV:
            e.length = 4.8;  e.width = 2.0;
            e.wheelbase = 2.85;
            e.mass = 1800.0;
            e.drag_coeff = 0.45;
            e.max_brake = 4.0;
            e.cg_height = 0.68;
            e.tire_stiffness_f = 90000.0;
            e.tire_stiffness_r = 90000.0;
            e.yaw_inertia = 3200.0;
            break;
        case EntityType::Car:
        case EntityType::Ego:
        default:
            e.length = 4.6;  e.width = 2.0;
            e.wheelbase = 2.7;
            e.mass = 1500.0;
            e.drag_coeff = 0.4;
            e.max_brake = 4.0;
            e.cg_height = 0.55;
            e.tire_stiffness_f = 80000.0;
            e.tire_stiffness_r = 80000.0;
            e.yaw_inertia = 2250.0;
            break;
    }
}

}  // namespace flowsim
