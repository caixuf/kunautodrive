// maneuver_tracker.h — 通用机动跟踪器（C++ 移植）
//
// 规格：tools/maneuver_tracker.py（Python 原型，掉头 + 倒车入库双 PASS）。
// 目标（回应"遇新操作就改好久、没通用能力"）：
//   机动 = 参考轨迹数据（点列 + 带符号 v），执行 = 这一个跟踪器。
//   新操作（倒库/侧方/坡道）= 加一份轨迹数据，不改执行逻辑。
//
// 核心设计（vs 掉头 5 层特例 gate）：
//   1. 弧长推进 s 单调，不猜段 —— 段归属由 s 在轨迹上的位置唯一决定，
//      掉头 debug 的 D/R 交界几何歧义（prog_i 锁段、heading gate）根源
//      是"几何最近点在两条弧间跳"。用弧长参数天生无歧义。
//   2. 挡位 = 执行点 v 符号（v<0→R），换挡滞回 + 刹停由 s 处 v 决定。
//   3. 横向 = kappa 前馈 + 车体系 e_lat/dh 反馈（倒挡反馈反号）。
//   4. 速度 = s 处 |v|（执行点），6m 前视 min|v| 同挡段，下限 1.5。
//
// 纵向 PID 留在 control_node（tracker 只算 steer/target_speed/gear/
// gear_pending），复用已验证的 C++ PID（reverse mirror + anti-windup +
// SHIFT_STOP）。
//
// 纯算法：只依赖 <cmath>/<cstring>/<algorithm> + adas_msgs_gen.h
// （TrajectoryPoint）。无 transport/param/framework 依赖，可独立单测。
// header-only，编译进 control_node 插件和 test_maneuver_tracker 测试。
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

#include "adas_msgs_gen.h"          // TrajectoryPoint

namespace maneuver {

struct ManeuverTrackerParams {
    double segment_v_threshold = 0.1;   // v < -this => reverse（段边界测试）
    double heading_gate_rad    = 0.4;   // 跨 D/R 边界需车头接近下一段起点
    double diverge_guard_rad   = 1.2;   // |dh| vs exec heading > 此值 => 暂停 s
    double lookahead_m         = 2.0;   // 横向前视弧长（同挡段）
    double speed_scan_m        = 6.0;   // 目标速度 min|v| 扫描窗口
    double speed_floor_mps     = 0.5;   // 目标速度下限（2026-08-05 1.5→0.5：掉头
                                        // Phase 1 刹停 + 驻停段能真正把车停到近 0，
                                        // 旧 1.5 会覆盖 v=0 让车停不住）
    double gear_v_threshold    = 0.3;   // 向前扫第一个 |v|>此值 => gear 意图
    double gear_pending_speed  = 0.8;   // |speed|>此值 => 换挡前刹停
    double lat_gain_dh         = 1.6;   // heading 误差反馈增益
    double lat_gain_elat       = 0.70;  // 车体系横向误差增益
    double max_steer           = 0.60;  // rad 满舵
};

struct TrackPoint {
    double x = 0, y = 0, heading = 0, kappa = 0, v = 0, l = 0;
};

struct ManeuverResult {
    double steer        = 0.0;   // rad，已 clip
    double target_speed = 0.0;   // 带符号（负=倒挡）
    int    gear         = 1;     // +1 DRIVE / -1 REVERSE
    bool   gear_pending = false;
};

class ManeuverTracker {
public:
    static constexpr int kMaxPts = 64;   // 与 Trajectory::points[64] 一致

    ManeuverTracker() { std::memset(pts_, 0, sizeof(pts_)); }

    /// 机动轨迹新到达（maneuver_mode 0→1）：重置 s/gear，建 cum_s。
    void init(const TrajectoryPoint* pts, int n, double wheelbase) {
        s_ = 0.0;
        gear_ = 1;
        has_prev_ego_ = false;
        travel_dist_ = 0.0;
        setTrajectory(pts, n, wheelbase);
    }

    /// 每机动帧：刷新点 + cum_s（planning 可能 >40m cache miss 重生成）。
    /// 若起终点发生实质性跳变（如停滞重规划）则复位 s/gear，避免沿用旧轨迹末段 s 导致新规划立即 done。
    void setTrajectory(const TrajectoryPoint* pts, int n, double wheelbase) {
        const bool was_done = done();
        const int new_n = std::min(n, (int)kMaxPts);
        const bool start_shifted = (n_ > 0 && new_n > 0 &&
            std::hypot((double)pts[0].x - (double)pts_[0].x,
                       (double)pts[0].y - (double)pts_[0].y) > (was_done ? 0.10 : 0.35));
        const bool end_shifted = (n_ > 0 && new_n > 0 &&
            std::hypot((double)pts[new_n - 1].x - (double)pts_[n_ - 1].x,
                       (double)pts[new_n - 1].y - (double)pts_[n_ - 1].y) > 0.35);
        n_ = new_n;
        if (n_ > 0) std::memcpy(pts_, pts, sizeof(TrajectoryPoint) * (size_t)n_);
        wheelbase_ = wheelbase;
        rebuildCumS();
        if (start_shifted || (was_done && end_shifted)) {
            s_ = 0.0;
            gear_ = 1;
            has_prev_ego_ = false;
            travel_dist_ = 0.0;
        }
        int i = 0;
        exec(&i);           // 刷新缓存参考几何（on_trajectory init 后可读）
        lookahead(i);
    }

    void setParams(const ManeuverTrackerParams& p) { p_ = p; }

    bool hasTrajectory() const { return n_ > 0; }
    double arcLengthS() const { return s_; }
    bool done() const { return n_ > 0 && s_ >= cum_s_[n_ - 1]; }

    /// 参考几何（control 的 target_path_*/lane_d 簿记用）。
    const TrackPoint& execPoint() const { return last_exec_; }
    const TrackPoint& lookaheadPoint() const { return last_lookahead_; }

    /// 镜像 Python ManeuverTracker.tick()。只返回跟随器输出；纵向 PID 在 control。
    ManeuverResult tick(double ego_x, double ego_y, double ego_heading,
                        double current_speed, double dt);

private:
    TrajectoryPoint pts_[kMaxPts];
    double cum_s_[kMaxPts];
    int    n_ = 0;
    double wheelbase_ = 2.7;
    double s_ = 0.0;
    int    gear_ = 1;
    ManeuverTrackerParams p_;
    TrackPoint last_exec_, last_lookahead_;
    double prev_ego_x_ = 0.0, prev_ego_y_ = 0.0;
    bool   has_prev_ego_ = false;
    double travel_dist_ = 0.0;

    void rebuildCumS() {
        cum_s_[0] = 0.0;
        for (int i = 1; i < n_; i++) {
            cum_s_[i] = cum_s_[i - 1] +
                std::hypot((double)pts_[i].x - (double)pts_[i - 1].x,
                           (double)pts_[i].y - (double)pts_[i - 1].y);
        }
    }

    static double normAngle(double a) {
        while (a >  M_PI) a -= 2.0 * M_PI;
        while (a < -M_PI) a += 2.0 * M_PI;
        return a;
    }

    static TrackPoint toTrack(const TrajectoryPoint& p) {
        TrackPoint t;
        t.x = p.x; t.y = p.y; t.heading = p.heading;
        t.kappa = p.kappa; t.v = p.v; t.l = p.l;
        return t;
    }

    /// 弧长推进：s 按车实际行驶距离单调推进，并在运动态做局部同挡段几何投影纠偏。
    void advanceS(double ego_x, double ego_y, double ego_heading, double current_speed, double dt) {
        int exec_i = 0;
        TrackPoint ex = exec(&exec_i);
        double dh = normAngle(ex.heading - ego_heading);
        if (std::fabs(dh) > p_.diverge_guard_rad) {
            prev_ego_x_ = ego_x; prev_ego_y_ = ego_y; has_prev_ego_ = true;
            return;   // 车脱离执行方向，暂停推进
        }
        double new_s = s_ + std::fabs(current_speed) * dt;
        // 段边界 guard：跨 v 符号变化点，若车头还没转到接近下一段起点，停在边界
        double boundary_cap = cum_s_[n_ - 1];
        for (int i = 0; i < n_ - 1; i++) {
            bool a = (double)pts_[i + 1].v < -p_.segment_v_threshold;
            bool b = (double)pts_[i].v     < -p_.segment_v_threshold;
            if (a != b && s_ < cum_s_[i]) {
                double next_h = pts_[i + 1].heading;
                if (std::fabs(normAngle(next_h - ego_heading)) > p_.heading_gate_rad) {
                    boundary_cap = std::min(boundary_cap, cum_s_[i]);
                    if (cum_s_[i] < new_s) new_s = cum_s_[i];
                }
            }
        }
        double step_ego = has_prev_ego_ ? std::hypot(ego_x - prev_ego_x_, ego_y - prev_ego_y_) : 0.0;
        travel_dist_ += step_ego;
        bool moved = has_prev_ego_ && (step_ego > 0.005);
        if (moved && n_ >= 2) {
            bool cur_rev = (double)pts_[exec_i].v < -p_.segment_v_threshold;
            double best_d2 = 1.0;
            double best_s = new_s;
            bool found_proj = false;
            for (int i = 0; i < n_ - 1; i++) {
                bool seg_rev = (double)pts_[i].v < -p_.segment_v_threshold;
                if (seg_rev != cur_rev) continue;
                if (cum_s_[i + 1] < new_s - 1.2 || cum_s_[i] > new_s + 1.2) continue;
                double sx = (double)pts_[i + 1].x - (double)pts_[i].x;
                double sy = (double)pts_[i + 1].y - (double)pts_[i].y;
                double seg_l2 = sx * sx + sy * sy;
                if (seg_l2 < 1e-9) continue;
                double t = ((ego_x - (double)pts_[i].x) * sx +
                            (ego_y - (double)pts_[i].y) * sy) / seg_l2;
                t = std::max(0.0, std::min(1.0, t));
                double px = (double)pts_[i].x + t * sx;
                double py = (double)pts_[i].y + t * sy;
                double d2 = (ego_x - px) * (ego_x - px) + (ego_y - py) * (ego_y - py);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_s = cum_s_[i] + t * (cum_s_[i + 1] - cum_s_[i]);
                    found_proj = true;
                }
            }
            if (found_proj && std::fabs(best_s - new_s) < 0.75) {
                double delta = best_s - new_s;
                // 有界局部纠偏：前进与后退纠偏幅度均受控，防止 s_ 在终点前被大步拉回而倒挂在车后
                double corr = (delta > 0.0)
                    ? (best_d2 < 0.09 ? std::min(0.025, 0.25 * delta) : 0.0)
                    : (best_d2 < 0.25 ? std::max(-0.025, 0.25 * delta) : 0.0);
                new_s = std::min(boundary_cap, std::max(0.0, new_s + corr));
            }
        }
        prev_ego_x_ = ego_x; prev_ego_y_ = ego_y; has_prev_ego_ = true;
        s_ = std::min(new_s, cum_s_[n_ - 1]);
    }

    /// 执行点：当前弧长处的轨迹点，线性插值（Python _exec）。
    TrackPoint exec(int* out_i) {
        int i = 0;
        while (i + 1 < n_ && cum_s_[i + 1] < s_) i++;
        TrackPoint pt;
        if (i + 1 >= n_) {
            pt = toTrack(pts_[n_ - 1]);
            if (out_i) *out_i = n_ - 1;
            last_exec_ = pt;
            return pt;
        }
        double span = cum_s_[i + 1] - cum_s_[i];
        double frac = (span > 1e-9) ? (s_ - cum_s_[i]) / span : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;   // 防 s==末点 1-ulp 外推
        const TrajectoryPoint& a = pts_[i];
        const TrajectoryPoint& b = pts_[i + 1];
        pt.x = a.x + (b.x - a.x) * frac;
        pt.y = a.y + (b.y - a.y) * frac;
        pt.heading = normAngle(a.heading + normAngle(b.heading - a.heading) * frac);
        pt.kappa = a.kappa + (b.kappa - a.kappa) * frac;
        pt.v = a.v + (b.v - a.v) * frac;
        pt.l = a.l + (b.l - a.l) * frac;   // 仅 control lane_d 簿记用
        if (out_i) *out_i = i;
        last_exec_ = pt;
        return pt;
    }

    /// 前视点：在同挡段内沿精确弧长 s_ + la_limit 线性插值，消除离散点间距带来的前视阶跃与冻结。
    TrackPoint lookahead(int base_i) {
        bool rev = (gear_ == -1);
        const double la_limit = std::min(p_.lookahead_m, rev ? 0.65 : 0.8);
        if (n_ <= 0) return TrackPoint{};
        if (n_ == 1 || base_i >= n_ - 1) {
            last_lookahead_ = toTrack(pts_[n_ - 1]);
            return last_lookahead_;
        }
        double max_same_gear_s = std::max(s_, cum_s_[base_i]);
        int end_j = base_i;
        for (int j = base_i; j + 1 < n_; j++) {
            double next_v = (double)pts_[j + 1].v;
            if (std::fabs(next_v) > p_.segment_v_threshold && (next_v < -p_.segment_v_threshold) != rev) break;
            end_j = j + 1;
            max_same_gear_s = cum_s_[end_j];
            if (max_same_gear_s >= s_ + la_limit) break;
        }
        double target_s = std::min(s_ + la_limit, max_same_gear_s);
        int k = base_i;
        while (k + 1 < end_j && cum_s_[k + 1] < target_s) k++;
        TrackPoint target;
        if (k + 1 >= n_ || k >= end_j) {
            target = toTrack(pts_[end_j]);
        } else {
            double span = cum_s_[k + 1] - cum_s_[k];
            double frac = (span > 1e-9) ? std::max(0.0, std::min(1.0, (target_s - cum_s_[k]) / span)) : 0.0;
            const TrajectoryPoint& a = pts_[k];
            const TrajectoryPoint& b = pts_[k + 1];
            target.x = a.x + (b.x - a.x) * frac;
            target.y = a.y + (b.y - a.y) * frac;
            target.heading = normAngle(a.heading + normAngle(b.heading - a.heading) * frac);
            target.kappa = a.kappa + (b.kappa - a.kappa) * frac;
            target.v = a.v + (b.v - a.v) * frac;
            target.l = a.l + (b.l - a.l) * frac;
        }
        last_lookahead_ = target;
        return target;
    }

    /// 挡位：默认跟随执行点当前段；若前视 speed_scan_m 内出现相反挡段，
    /// 则提前请求刹停换挡。这样短机动（倒库/侧方）不会因为 s 逼近太慢而错过换挡。
    bool updateGear(int base_i, double current_speed) {
        const double base_v = (double)pts_[base_i].v;
        const bool has_base_motion = (std::fabs(base_v) > p_.gear_v_threshold);
        int want = gear_;
        if (has_base_motion) {
            want = (base_v < 0) ? -1 : 1;
        } else {
            for (int i = base_i; i < n_; i++) {
                const double v = (double)pts_[i].v;
                if (std::fabs(v) > p_.gear_v_threshold) {
                    want = (v < 0) ? -1 : 1;
                    break;
                }
            }
        }
        if (want != gear_ && std::fabs(current_speed) > p_.gear_pending_speed)
            return true;   // gear_pending：带速想换挡，本帧刹停
        gear_ = want;
        return false;
    }
};

// ── 方法体（header-only inline）──

inline ManeuverResult ManeuverTracker::tick(double ego_x, double ego_y,
                                            double ego_heading,
                                            double current_speed, double dt) {
    advanceS(ego_x, ego_y, ego_heading, current_speed, dt);
    int exec_i = 0;
    TrackPoint exec_pt = exec(&exec_i);
    bool gear_pending = updateGear(exec_i, current_speed);

    // 修：换挡已落定但执行点仍卡在相反挡段 → 倒车画圆死锁。
    // 掉头前视窗口过早扫到倒车段，gear_ 已置 -1，但 exec 点还在前进弧内
    // （pts_[exec_i].v > 0）→ 前馈 ff 取前进弧 kappa（+0.6），倒挡下 steer 方向
    // 反 → yaw_rate 反向、heading 一路减 → advanceS 的 diverge_guard 永久冻结 s。
    // 这里把 s_ 直接跳到下一倒车段边界并重取执行点，让前馈/反馈方向与挡位一致，
    // 车头才能正确转升向 π，dh 收敛后 advanceS 恢复推进，后续点自然衔接。
    if (gear_ == -1 && (double)pts_[exec_i].v >= -p_.segment_v_threshold) {
        for (int i = exec_i; i < n_ - 1; i++) {
            if ((double)pts_[i + 1].v < -p_.segment_v_threshold) {
                s_ = cum_s_[i + 1];
                break;
            }
        }
        exec_pt = exec(&exec_i);   // 重取执行点 → ff 取倒车段 kappa，方向正确
    }

    // 目标速度方向跟随「已落定的 gear」，而非插值 exec v：
    // D/R 边界处 exec 在 v=-3 与 v=+3.5 间插值，v 符号在边界模糊（可能跨 0），
    // 若用 exec_rev 会与 gear 不一致 → 倒挡期拿到正向 target → REV_BRAKE 卡死
    // （2026-08-04 实测掉头倒车后 spd 卡 0 直到 40s timeout）。
    bool exec_rev = (gear_ == -1);
    // 目标速度幅值 = |执行点 v|，前视 speed_scan_m 内同挡段最小 |v|，下限 floor
    double mag = std::fabs(exec_pt.v);
    double arc = 0.0;
    for (int i = exec_i; i < n_ - 1; i++) {
        arc += std::hypot((double)pts_[i + 1].x - (double)pts_[i].x,
                          (double)pts_[i + 1].y - (double)pts_[i].y);
        bool next_rev = (double)pts_[i + 1].v < -p_.segment_v_threshold;
        const double scan_limit = exec_rev ? std::min(p_.speed_scan_m, 2.0) : p_.speed_scan_m;
        if (arc > scan_limit || next_rev != exec_rev) break;
        // 与 updateGear 一致：跳过 v≈0 的换挡刹停/驻停点，否则 min|v| 被
        // 压成 floor，车在短前进段里 0.5m/s 爬行，200 tick 到不了 D/R 边界
        // 进不了倒挡（parking/parallel CI 失败，2026-08-05 实测）。
        if (std::fabs((double)pts_[i + 1].v) > p_.gear_v_threshold)
            mag = std::min(mag, std::fabs((double)pts_[i + 1].v));
    }
    bool final_stop_leg = (n_ >= 2 && std::fabs((double)pts_[n_ - 1].v) < 0.05);
    if (final_stop_leg) {
        for (int i = exec_i; i < n_ - 1; i++) {
            bool nr = (double)pts_[i + 1].v < -p_.segment_v_threshold;
            if (nr != exec_rev && std::fabs((double)pts_[i + 1].v) > p_.gear_v_threshold) {
                final_stop_leg = false;
                break;
            }
        }
    }
    const double rem_end = std::max(0.0, cum_s_[n_ - 1] - s_);
    if (final_stop_leg && rem_end < 1.2 && cum_s_[n_ - 1] > 1.5) {
        double stop_v = std::max(0.12, 0.14 + 0.72 * std::sqrt(rem_end));
        mag = std::min(std::max(mag, std::min(p_.speed_floor_mps, stop_v)), stop_v);
    } else {
        mag = std::max(mag, p_.speed_floor_mps);
    }
    double target_speed = exec_rev ? -mag : mag;

    // 轨迹终点 = 机动完成 → 目标 0，车刹停（不继续反向冲）
    // 闭环真实行驶车辆：按车体沿末点朝向的物理残余纵深 rem_along 判停，避免弧长微差导致提前 0.1~0.2m 刹停
    bool end_aligned = true;
    if (final_stop_leg && n_ >= 2 && travel_dist_ > 0.5) {
        const TrajectoryPoint& gp = pts_[n_ - 1];
        const double rev_sgn = ((double)pts_[n_ - 2].v < -p_.segment_v_threshold) ? -1.0 : 1.0;
        const double rem_along = rev_sgn * (std::cos((double)gp.heading) * ((double)gp.x - ego_x) +
                                            std::sin((double)gp.heading) * ((double)gp.y - ego_y));
        if (rem_end <= 0.03 && rem_along > 0.03) {
            end_aligned = false;
            s_ = std::max(0.0, cum_s_[n_ - 1] - std::min(0.40, std::max(0.04, rem_along)));
        }
    }
    if (final_stop_leg && end_aligned && rem_end <= 0.03) {
        s_ = cum_s_[n_ - 1];
    }
    if (s_ >= cum_s_[n_ - 1]) { target_speed = 0.0; gear_ = 1; }
    if (gear_pending) target_speed = 0.0;

    // 横向：exec_pt 处精确曲线前馈 ff + 瞬时参考位姿偏差 dh/e_lat 反馈（避免弯道前视点自带 Δθ=κ·L 叠加 ff 造成内切饱和）
    // 倒挡航向反馈需反号（v<0 时 yaw_rate 与 steer 反号）；而 e_lat 为到参考点的左法向偏差，
    // 向左偏靶（e_lat>0）需正 steer 使车尾向左摆入（正方向同前挡）。
    (void)lookahead(exec_i);
    TrackPoint ref = end_aligned ? exec_pt : toTrack(pts_[n_ - 1]);
    bool rev = (gear_ == -1);
    double dh = normAngle(ref.heading - ego_heading);
    double e_lat = -std::sin(ref.heading) * (ref.x - ego_x)
                   + std::cos(ref.heading) * (ref.y - ego_y);
    double ff = std::atan(wheelbase_ * ref.kappa);
    double fb = rev
        ? (-p_.lat_gain_dh * dh + p_.lat_gain_elat * e_lat)
        : (p_.lat_gain_dh * dh + p_.lat_gain_elat * e_lat);
    double steer = std::max(-p_.max_steer, std::min(p_.max_steer, ff + fb));
    if (done()) {
        steer = 0.0;
    } else if (final_stop_leg && rem_end < 0.28 &&
               std::fabs(ref.kappa) < 0.01 && std::fabs(dh) < 0.03) {
        double cap = (rem_end < 0.12 && std::fabs(dh) < 0.025) ? 0.04 : 0.18;
        steer = std::max(-cap, std::min(cap, ff + 0.45 * fb));
    }

    ManeuverResult r{ steer, target_speed, gear_, gear_pending };
    return r;
}

} // namespace maneuver
