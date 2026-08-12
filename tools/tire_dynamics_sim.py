#!/usr/bin/env python3
"""
tire_dynamics_sim.py — 轮胎动力学模型对比仿真（py-sim-first 阶段 1）

目的：在移植到 C++ 之前，先用 Python 验证「线性轮胎二自由度」与
「Pacejka 魔术公式」在横向动力学上的行为差异，确认非线性模型更接近真实
车辆（滑移角不再硬饱和、随载荷/附着 μ 变化），为 C++ physics.cpp 的
step_bicycle_dynamic 升级提供依据。

纯 Python 标准库实现，无 numpy/scipy。

模型对照（全部对齐 C++ physics.cpp 的数值方法：前向欧拉 dt=0.05s）：
  1. kinematic   — 运动学自行车模型（默认，当前 C++ 主线）
  2. linear      — 线性轮胎二自由度（对齐 step_bicycle_dynamic，含滑移角
                   硬饱和 ±0.12rad、低速<5m/s 退化运动学、摩擦圆护栏）
  3. pacejka     — Pacejka 魔术公式 F_y=D·sin(C·atan(B·α−E·(B·α−atan(B·α))))，
                   用与线性模型相同的二自由度框架，只是侧向力由魔术公式给出，
                   峰值由附着系数 μ 决定、滑移角平滑饱和。

用法:
  python3 tools/tire_dynamics_sim.py                     # 恒定 steer 转弯对比
  python3 tools/tire_dynamics_sim.py --curve             # 闭环弯道跟随 A/B
  python3 tools/tire_dynamics_sim.py --tune              # Pacejka 参数扫描标定
  python3 tools/tire_dynamics_sim.py --run-all           # 全量执行
"""

import math
import argparse

# ── 常量（对齐 C++ physics.cpp / entity.h apply_vehicle_defaults Ego/Car）──
DT               = 0.05          # 20Hz
WHEELBASE        = 2.7
MASS             = 1500.0
TIRE_STIFF_F     = 80000.0       # 前轴等效侧偏刚度 N/rad
TIRE_STIFF_R     = 80000.0       # 后轴等效侧偏刚度 N/rad
YAW_INERTIA      = 2250.0
DRAG_COEFF       = 0.4
STEER_TAU        = 0.15          # 转向一阶滞后
STEER_RATE_MAX   = 0.6           # 最大转向速率 rad/s
LOW_SPEED_MS     = 5.0           # 低速退化运动学
CG_FRONT_FRAC    = 0.45          # 质心到前轴占比
SLIP_ANGLE_MAX   = 0.12          # 线性模型滑移角硬饱和 (rad)
G                = 9.81

# Pacejka 默认标定（乘用车典型值）
PACEJKA = {
    'B': 11.0,    # 刚度因子
    'C': 1.3,     # 形状因子
    'E': -0.5,    # 曲率因子
    'mu': 0.9,    # 峰值附着系数
    'Fz_scale': 1.0,
}

def clamp(v, lo, hi):
    if v < lo: return lo
    if v > hi: return hi
    return v

def norm_angle(h):
    while h >  math.pi: h -= 2.0 * math.pi
    while h < -math.pi: h += 2.0 * math.pi
    return h


class TireVehicle:
    """统一的轮胎动力学车辆状态机（linear / pacejka 共用框架）。

    字段与 Entity 对齐；model ∈ {'kinematic','linear','pacejka'}。
    """

    def __init__(self, model='kinematic', x0=0.0, y0=0.0, v0=5.0, heading0=0.0):
        self.model = model
        self.x, self.y, self.heading = x0, y0, heading0
        self.v_x_body = v0          # 车体系纵向速度
        self.v_y_body = 0.0         # 车体系横向速度
        self.speed = v0
        self.yaw_rate = 0.0
        self.steer = 0.0
        self.F_yf = 0.0
        self.F_yr = 0.0
        # 诊断
        self.lat_accel = 0.0        # 横向加速度 m/s²（= 侧向力/质量）
        self.alpha_f = 0.0
        self.alpha_r = 0.0

    # ── 纵向力（对齐 physics.cpp longitudinal_accel）──
    def _longitudinal_accel(self, throttle, brake, v):
        drive = throttle * 5000.0
        sgn = 1.0 if v > 0.0 else (-1.0 if v < 0.0 else 0.0)
        brake_f = brake * 8000.0 * sgn
        drag = DRAG_COEFF * v * abs(v)
        return (drive - brake_f - drag) / MASS

    # ── 转向执行器（对齐 update_steer：限幅 + 一阶滞后 + 速率限幅）──
    def _update_steer(self, steer_cmd, dt, steer_override=False):
        limit = 0.60 if steer_override else 0.25
        steer_cmd = clamp(steer_cmd, -limit, limit)
        alpha = dt / (STEER_TAU + dt)
        nxt = self.steer + alpha * (steer_cmd - self.steer)
        max_rate = STEER_RATE_MAX * dt
        d = nxt - self.steer
        if d >  max_rate: nxt = self.steer + max_rate
        if d < -max_rate: nxt = self.steer - max_rate
        self.steer = nxt

    # ── 轮胎侧向力 ──
    def _tire_force(self, alpha, is_front):
        if self.model == 'pacejka':
            # Pacejka 96 魔术公式（侧向）
            C = PACEJKA['C']
            B = PACEJKA['B']
            E = PACEJKA['E']
            # 垂直载荷（静载荷分配，质心偏前）
            a = CG_FRONT_FRAC * WHEELBASE
            b = WHEELBASE - a
            fz = (MASS * G * b / WHEELBASE) if is_front else (MASS * G * a / WHEELBASE)
            fz *= PACEJKA['Fz_scale']
            D = PACEJKA['mu'] * fz          # 峰值侧向力 = μ·Fz
            Bx = B * alpha
            y = D * math.sin(C * math.atan(Bx - E * (Bx - math.atan(Bx))))
            return y
        # 线性轮胎：F_y = Cα·α，滑移角硬饱和（对齐 step_bicycle_dynamic）
        a_sat = clamp(alpha, -SLIP_ANGLE_MAX, SLIP_ANGLE_MAX)
        stiff = TIRE_STIFF_F if is_front else TIRE_STIFF_R
        return stiff * a_sat

    def step(self, throttle, brake, steer_cmd, dt=DT, steer_override=False):
        if self.model == 'kinematic':
            self._step_kinematic(throttle, brake, steer_cmd, dt, steer_override)
        else:
            self._step_dynamic(throttle, brake, steer_cmd, dt, steer_override)

    # ── 运动学（对齐 step_bicycle）──
    def _step_kinematic(self, throttle, brake, steer_cmd, dt, steer_override):
        self.speed += self._longitudinal_accel(throttle, brake, self.speed) * dt
        if self.speed < 0.0 and throttle >= 0.0: self.speed = 0.0
        self.speed = clamp(self.speed, -4.0, 60.0)
        self._update_steer(steer_cmd, dt, steer_override)
        self.yaw_rate = (self.speed / WHEELBASE) * math.tan(self.steer)
        self.heading += self.yaw_rate * dt
        self.heading = norm_angle(self.heading)
        half_wb = WHEELBASE * 0.5
        vx_r = self.speed * math.cos(self.heading)
        vy_r = self.speed * math.sin(self.heading)
        self.x += vx_r * dt - half_wb * math.sin(self.heading) * self.yaw_rate * dt
        self.y += vy_r * dt + half_wb * math.cos(self.heading) * self.yaw_rate * dt
        self.v_x_body = self.speed
        self.v_y_body = 0.0

    # ── 二自由度动力学（linear/pacejka；横向解对齐 integrate_lateral_dynamics）──
    def _step_dynamic(self, throttle, brake, steer_cmd, dt, steer_override):
        # 低速退化运动学（对齐 step_bicycle_dynamic LOW_SPEED_MS）
        if self.speed < LOW_SPEED_MS:
            self._step_kinematic(throttle, brake, steer_cmd, dt, steer_override)
            return
        if self.v_x_body <= 0.0 and self.speed > 0.0:
            self.v_x_body = self.speed
        # 纵向：用 v_x_body 自身积分（避免侧向速度注入自泵）
        self.v_x_body += self._longitudinal_accel(throttle, brake, self.v_x_body) * dt
        if self.v_x_body < 0.0: self.v_x_body = 0.0
        self._update_steer(steer_cmd, dt, steer_override)

        a = CG_FRONT_FRAC * WHEELBASE
        b = WHEELBASE - a
        vx = self.v_x_body

        # 滑移角（linear 用 C++ 的硬饱和；pacejka 由魔术公式自然饱和）
        if self.model == 'linear':
            self.alpha_f = clamp(self.steer - math.atan2(self.v_y_body + a * self.yaw_rate, vx),
                                 -SLIP_ANGLE_MAX, SLIP_ANGLE_MAX)
            self.alpha_r = clamp(- math.atan2(self.v_y_body - b * self.yaw_rate, vx),
                                 -SLIP_ANGLE_MAX, SLIP_ANGLE_MAX)
        else:
            self.alpha_f = self.steer - math.atan2(self.v_y_body + a * self.yaw_rate, vx)
            self.alpha_r = - math.atan2(self.v_y_body - b * self.yaw_rate, vx)

        self.F_yf = self._tire_force(self.alpha_f, is_front=True)
        self.F_yr = self._tire_force(self.alpha_r, is_front=False)

        vy_dot = (self.F_yf + self.F_yr) / MASS - vx * self.yaw_rate
        r_dot  = (a * self.F_yf - b * self.F_yr) / YAW_INERTIA

        self.v_y_body += vy_dot * dt
        self.yaw_rate += r_dot * dt

        # 摩擦圆护栏（对齐 C++）：|v_y|≤vx·tan(slip_max)·1.5, |r|≤μ·g/vx
        max_vy = abs(vx) * math.tan(SLIP_ANGLE_MAX) * 1.5
        if abs(self.v_y_body) > max_vy:
            self.v_y_body = math.copysign(max_vy, self.v_y_body)
        max_r = (0.8 * G) / max(vx, 1.0)
        if abs(self.yaw_rate) > max_r:
            self.yaw_rate = math.copysign(max_r, self.yaw_rate)

        self.heading += self.yaw_rate * dt
        self.heading = norm_angle(self.heading)

        ch, sh = math.cos(self.heading), math.sin(self.heading)
        self.vx = self.v_x_body * ch - self.v_y_body * sh
        self.vy = self.v_x_body * sh + self.v_y_body * ch
        self.x += self.vx * dt
        self.y += self.vy * dt
        self.speed = math.sqrt(self.v_x_body ** 2 + self.v_y_body ** 2)
        self.lat_accel = math.sqrt(self.F_yf ** 2 + self.F_yr ** 2) / MASS


# ══════════════════════════════════════════════════════════════
#  场景 1：恒定 steer 定速转弯 —— 对比三种模型的 yaw_rate / 半径 / 横向加速度
# ══════════════════════════════════════════════════════════════

def run_steady_turn(model, steer, speed, duration=8.0):
    """定速 v0 + 恒定 steer 匀速转弯，测量稳态 yaw_rate / 转弯半径 / 横向加速度。"""
    v = TireVehicle(model=model, x0=0.0, y0=0.0, v0=speed, heading0=0.0)
    throttle = 0.15  # 维持速度的平衡油门（近似匀速）
    n = int(duration / DT)
    for _ in range(n):
        v.step(throttle, 0.0, steer, steer_override=True)
    # 稳态值取后半段平均
    half = int(n * 0.5)
    # 用 yaw_rate 稳态算半径：R = v_forward / r
    r_avg = v.yaw_rate
    v_avg = v.speed
    radius = v_avg / r_avg if abs(r_avg) > 1e-6 else float('inf')
    return {
        'model': model, 'steer': steer, 'speed': speed,
        'yaw_rate': r_avg,
        'radius': radius,
        'lat_accel': v.lat_accel,
        'v_y_body': v.v_y_body,
    }


# ══════════════════════════════════════════════════════════════
#  场景 2：闭环弯道跟随 A/B —— 同一 Stanley 控制器，看三种模型过弯精度
# ══════════════════════════════════════════════════════════════

class CurvedRoad:
    def __init__(self, kappa=0.005, length=500.0):
        self.kappa = kappa
        self.length = length

    def center_pos_at(self, s):
        k = self.kappa
        if abs(k) < 1e-9: return (s, 0.0)
        return (math.sin(s * k) / k, (1.0 - math.cos(s * k)) / k)

    def center_heading_at(self, s):
        return s * self.kappa


def stanley(lat_err, heading_err, yaw_rate, speed, kappa, prev, params, ff_understeer=0.0):
    """简版 Stanley（与 control_sim 同构，含前馈）。

    ff_understeer — 动力学模型的 understeer 前馈补偿系数。运动学模型无侧偏、
    无侧向滑移，前馈只需 WHEELBASE·kappa；动力学模型存在侧偏角，转弯半径比
    运动学大，需额外补偿：δ_ff = kappa·(WHEELBASE + K_us·v²)。K_us 即
    understeer 梯度 (rad·s²/m)，扫描它可把动力学模型的过弯横向偏差拉回。
    """
    lat_kp, lat_kd, yaw_damp = params
    speed_eff = max(speed, 3.0)
    cte = math.atan2(lat_kp * lat_err, speed_eff)
    heading_term = lat_kd * heading_err
    yaw_damp_term = yaw_damp * yaw_rate
    ff = kappa * (WHEELBASE + ff_understeer * speed * speed)
    steer = cte - heading_term - yaw_damp_term + ff
    steer = clamp(steer, -0.25, 0.25)
    steer = 0.5 * steer + 0.5 * prev
    return steer


def run_curve_follow(model, kappa=0.005, speed=12.0, duration=15.0,
                     ff_understeer=0.0):
    """闭环弯道跟随：测稳态横向偏差与 heading 误差。

    对动力学模型（linear/pacejka）传入 ff_understeer 做侧偏补偿，观察能否把
    横向偏差压回运动学水平。返回 avg_lat_err / avg_heading_err / max_lat_accel。
    """
    road = CurvedRoad(kappa=kappa)
    v = TireVehicle(model=model, x0=0.0, y0=0.0, v0=speed, heading0=0.0)
    params = (0.5, 2.0, 0.28)   # lat_kp, lat_kd_heading, yaw_damping
    prev = 0.0
    R = 1.0 / kappa
    circle_cy = R
    n = int(duration / DT)
    tail_lat = []
    tail_heading = []
    for _ in range(n):
        # 用圆心距离法算横向偏差（正=弯道外侧）
        dc = math.sqrt(v.x * v.x + (v.y - circle_cy) * (v.y - circle_cy))
        lat = dc - R
        # 最近点切线 heading（右弯时圆心在 +y）
        if dc > 1e-9:
            nx = v.x / dc
            ny = (v.y - circle_cy) / dc
            nearest_h = math.atan2(nx, -ny)
        else:
            nearest_h = road.center_heading_at(v.x)
        heading_err = norm_angle(v.heading - nearest_h)
        # 右弯目标切线，车在外侧(lat>0)需向右转(负 steer 方向)，取 -lat
        steer = stanley(-lat, heading_err, v.yaw_rate, v.speed, kappa, prev,
                        params, ff_understeer)
        prev = steer
        # 纵向：维持速度
        v.step(0.15, 0.0, steer, steer_override=False)
        if _ >= n // 2:
            tail_lat.append(abs(lat))
            tail_heading.append(abs(heading_err))
    avg_lat = sum(tail_lat) / len(tail_lat) if tail_lat else 999
    avg_head = sum(tail_heading) / len(tail_heading) if tail_heading else 999
    return {'model': model, 'kappa': kappa, 'speed': speed,
            'avg_lat_err': avg_lat, 'avg_heading_err': avg_head,
            'max_lat_accel': max(v.lat_accel, 0.0)}


# ══════════════════════════════════════════════════════════════
#  Pacejka 参数扫描标定
# ══════════════════════════════════════════════════════════════

def tune_pacejka():
    """扫描 Pacejka B/C/E/μ，找「稳态横向加速度曲线平滑、峰值≈μ·g」的标定。"""
    print("\n" + "=" * 62)
    print("  Pacejka 参数扫描标定")
    print("=" * 62)
    # 目标：中等速度 12m/s 下，随 steer 增大横向加速度单调到饱和，峰值≈μ·g
    results = []
    for B in [8.0, 11.0, 14.0]:
        for C in [1.2, 1.3, 1.4]:
            for E in [-0.8, -0.5, -0.2]:
                for mu in [0.7, 0.9, 1.1]:
                    PACEJKA['B'], PACEJKA['C'], PACEJKA['E'], PACEJKA['mu'] = B, C, E, mu
                    # 扫 steer 看横向加速度是否平滑单调到饱和且不超 μ·g
                    accels = []
                    ok = True
                    for sd in [0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6]:
                        r = run_steady_turn('pacejka', sd, 12.0, duration=3.0)
                        accels.append(r['lat_accel'])
                    # 单调性：横向加速度应随 steer 单调不减（除饱和段）
                    for i in range(1, len(accels)):
                        if accels[i] < accels[i-1] - 0.05:
                            ok = False
                            break
                    peak = accels[-1] if accels else 0.0
                    mu_g = mu * G
                    if not ok:
                        continue
                    # 评分：峰值尽量接近 μ·g 且不超过，曲线平滑（相邻差不过大）
                    peak_err = abs(peak - mu_g) / mu_g
                    smooth = max(abs(accels[i]-accels[i-1]) for i in range(1, len(accels))) if len(accels) > 1 else 0
                    score = peak_err * 10 + smooth * 2
                    results.append((score, B, C, E, mu, peak, mu_g, accels))
    if results:
        results.sort(key=lambda r: r[0])
        score, B, C, E, mu, peak, mu_g, accels = results[0]
        print(f"\n  尝试: {len(results)} 个有效组合")
        print(f"  ✅ 最优标定: B={B}, C={C}, E={E}, μ={mu}")
        print(f"     峰值横向加速度={peak:.2f} m/s² (目标 μ·g={mu_g:.2f})")
        print(f"     steer 扫描: " + ", ".join(f"{a:.2f}" for a in accels))
    else:
        print("  ❌ 无有效标定")


# ══════════════════════════════════════════════════════════════
#  主入口
# ══════════════════════════════════════════════════════════════

def run_steady_compare():
    print("\n" + "=" * 62)
    print("  恒定 steer 定速转弯 —— 三种模型横向响应对比")
    print("=" * 62)
    for speed in [8.0, 12.0]:
        for steer in [0.1, 0.2, 0.4]:
            rows = []
            for model in ['kinematic', 'linear', 'pacejka']:
                r = run_steady_turn(model, steer, speed)
                rows.append(r)
            print(f"\n  v0={speed:.0f}m/s steer={steer:.2f}rad:")
            for r in rows:
                print(f"    {r['model']:10s} yaw_rate={r['yaw_rate']:6.3f} "
                      f"R={r['radius']:7.1f}m lat={r['lat_accel']:5.2f}m/s² "
                      f"v_y={r['v_y_body']:6.3f}m/s")


def run_curve_compare():
    print("\n" + "=" * 62)
    print("  闭环弯道跟随 A/B (kappa=0.005, R=200m)")
    print("  动力学模型含侧偏(understeer)，扫描 K_us 前馈补偿")
    print("=" * 62)
    for speed in [10.0, 12.0, 15.0]:
        print(f"\n  @ {speed:.0f}m/s:")
        for model in ['kinematic', 'linear', 'pacejka']:
            r = run_curve_follow(model, speed=speed)
            print(f"    {model:10s} K_us=0    : avg_lat_err={r['avg_lat_err']:.3f}m "
                  f"head_err={r['avg_heading_err']:.4f}rad "
                  f"max_lat={r['max_lat_accel']:.2f}m/s²")
            if model != 'kinematic':
                best = None
                for kus in [0.001, 0.002, 0.003, 0.004, 0.005, 0.006, 0.008, 0.010]:
                    rr = run_curve_follow(model, speed=speed, ff_understeer=kus)
                    if best is None or rr['avg_lat_err'] < best[1]:
                        best = (kus, rr['avg_lat_err'])
                print(f"    {model:10s} 补偿后 : avg_lat_err={best[1]:.3f}m "
                      f"(K_us={best[0]:.4f} rad·s²/m)")


def main():
    parser = argparse.ArgumentParser(description="FlowEngine 轮胎动力学仿真对比")
    parser.add_argument("--curve", action="store_true", help="闭环弯道跟随 A/B")
    parser.add_argument("--tune", action="store_true", help="Pacejka 参数扫描标定")
    parser.add_argument("--run-all", action="store_true", help="全量执行")
    args = parser.parse_args()

    if args.tune:
        tune_pacejka()
        return
    if args.curve:
        run_curve_compare()
        return
    if args.run_all:
        run_steady_compare()
        run_curve_compare()
        tune_pacejka()
        return
    # 默认：恒定 steer 对比
    run_steady_compare()


if __name__ == "__main__":
    main()
