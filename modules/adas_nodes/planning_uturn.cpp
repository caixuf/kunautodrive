#include "planning_uturn.h"

#include "construction_zones.h"
#include "planning_coordinates.h"
#include "road_geometry.h"
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace planning_uturn {

bool body_hits_construction(const UturnEnv& env, double x, double y, double h,
                            double half_body_len, double half_body_wid,
                            double margin) {
    if (env.cz_count <= 0 || !env.cz_x || !env.cz_y || !env.cz_len || !env.cz_wid) {
        return false;
    }
    const double sh = std::sin(h), ch = std::cos(h);
    const double cxs[4] = {
        x + half_body_len * ch - half_body_wid * sh,
        x + half_body_len * ch + half_body_wid * sh,
        x - half_body_len * ch - half_body_wid * sh,
        x - half_body_len * ch + half_body_wid * sh,
    };
    const double cys[4] = {
        y + half_body_len * sh + half_body_wid * ch,
        y + half_body_len * sh - half_body_wid * ch,
        y - half_body_len * sh + half_body_wid * ch,
        y - half_body_len * sh - half_body_wid * ch,
    };
    for (int z = 0; z < env.cz_count; ++z) {
        const double half_l = 0.5 * env.cz_len[z] + margin;
        const double half_w = 0.5 * env.cz_wid[z] + margin;
        for (int k = 0; k < 4; ++k) {
            if (std::fabs(cxs[k] - env.cz_x[z]) <= half_l &&
                std::fabs(cys[k] - env.cz_y[z]) <= half_w) {
                return true;
            }
        }
    }
    return false;
}

double construction_forward_space(const UturnEnv& env, double ego_x, double ego_y,
                                  double clearance) {
    if (env.cz_count <= 0 || !env.cz_x || !env.cz_y || !env.cz_len || !env.cz_wid) {
        return 1e9;
    }
    double best = 1e9;
    for (int z = 0; z < env.cz_count; ++z) {
        const double front = construction_zone_front_x(env.cz_x[z], env.cz_len[z]);
        if (front <= ego_x) continue;
        /* 横向：施工带覆盖 ego 所在 y（半宽 + 车半宽 1m）才算挡路 */
        const double half_w = 0.5 * env.cz_wid[z] + 1.0;
        if (std::fabs(ego_y - env.cz_y[z]) > half_w) continue;
        double d = front - ego_x - clearance;
        if (d < 0.0) d = 0.0;
        if (d < best) best = d;
    }
    return best;
}

int generate_trajectory(const UturnEnv& env, TrajectoryPoint* points, int max_points,
                        double ego_x, double ego_y, double ego_heading,
                        double ego_speed, double wheelbase,
                        double forward_space_m) {
    if (!points || max_points <= 0) return 0;

    const double dt = 0.05;               /* 20Hz 时间步长 */
    const double uturn_steer = 0.60;      /* 前进弧转向角：满舵（用户规范"方向盘接近打死第一把"）。
                                           * 旧值 0.45（wheelbase=2.8 模型下 Python 扫参最优）转弯慢，
                                           * 64 点轨迹截断在 Phase 3 前；满舵 yaw_rate 提高 38%，
                                           * 弧段缩短，轨迹可覆盖全部相位。 */
    /* 2026-08-05 慢转（用户规范"先停下来然后慢慢多把转"）：3.5→2.5 / -3.0→-2.0。
     * Python 仿真验证：corner_limit=6.0 + v_fwd=2.5 + v_rev=2.0 → corner_max=6.00
     * （距路沿 1.0m 余量）、2 把完成、车道对准良好。速度不影响运动学几何，
     * 只增加可控性、降低撞路沿风险。 */
    const double uturn_speed = 2.5;       /* 掉头前弧低速 (m/s) */
    const double reverse_speed = -2.0;    /* 倒车速度 (m/s) */
    const double half_wb = wheelbase * 0.5;  /* 半轴距：车辆中心偏移后轴距离 */
    /* 2026-08-05 12→10：掉头前向空间不足 12m 时 Phase 0 倒车腾挪（"掉头直接倒车"
     * 观感太骚，实测 fwd=11.5m 只差 1.5m）。掉头首弧 2×R≈7.9m（R=wb/tan(0.6)=3.95），
     * 10m 足够开始多把方向；真正无空间（返程路端 x≈0）仍需倒车，那是真实需求。 */
    const double min_uturn_space = 10.0;  /* 最小掉头前向空间 (m)：2×转弯半径 + 余量 */
    const double cz_margin = 0.5;         /* 施工区 AABB 外扩安全余量 (m) */

    /* 安全上限（防止无限循环） */
    const double phase0_max_dur = 5.0;   /* 腾挪倒车最多 5s */

    int n = 0;
    double x = ego_x, y = ego_y, h = ego_heading;
    double v = ego_speed;
    double steer = 0.0;
    uint32_t t_us = 0;

    auto road_l = [&](double px, double py) -> float {
        return (float)(py - road_center_y(px, env.curve_start_x, env.curve_length_m, env.curve_offset_m));
    };

    /* 归一化 heading 到 [-π, π] */
    auto norm_h = [](double hd) -> double {
        while (hd > M_PI)  hd -= 2.0 * M_PI;
        while (hd < -M_PI) hd += 2.0 * M_PI;
        return hd;
    };

    double h_start = norm_h(h);

    /* 掉头目标车道方向（2026-08 返程掉头修复）：
     * 前进端掉头（进入时 heading≈0，前进车道）→ 掉到对向车道，Phase 4 对齐 ±π
     * 返程掉头（进入时 heading≈±π，对向车道）→ 掉回前进车道，Phase 4 对齐 0 */
    const double uturn_target_h =
        (std::fabs(h_start) > M_PI * 0.5) ? 0.0 : M_PI;

    /* ═══ Phase 0 (可选): 倒车腾挪 — 前向空间不足时先倒车 ═══ */
    if (forward_space_m > 0.0 && forward_space_m < min_uturn_space) {
        double need_reverse = min_uturn_space - forward_space_m + 1.0;  /* +1m 安全余量 */
        double reversed = 0.0;
        steer = 0.0;
        v = reverse_speed;
        double phase0_t = 0.0;
        LOG_INFO("planning", "[UTURN] forward_space=%.1fm < %.1fm → reverse to make room (need %.1fm)",
                 forward_space_m, min_uturn_space, need_reverse);
        while (n < max_points) {
            x += v * std::cos(h) * dt;
            y += v * std::sin(h) * dt;
            reversed += std::fabs(v) * dt;

            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = (float)v;  /* 负值 = 倒车档 */
            points[n].kappa = 0.0f;
            points[n].a = 0.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            phase0_t += dt;
            t_us += (uint32_t)(dt * 1e6);

            bool space_ok = (reversed >= need_reverse);
            bool timeout  = (phase0_t >= phase0_max_dur);
            if (space_ok || timeout) break;
        }
        /* 倒车结束后刹停，再进入 Phase 1 */
        if (n < max_points && v < -0.1) {
            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = 0.0f;
            points[n].kappa = 0.0f;
            points[n].a = -5.0f;
            points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
            v = 0.0;
        }
        LOG_INFO("planning", "[UTURN] Phase 0 done: reversed %.1fm, now at x=%.1f y=%.2f",
                 reversed, x, y);
    }

    /* ═══ Phase 1: 直线重刹 → 先停稳再进弯 ═══ */
    {
        const double brake_decel = -8.0;  /* 急刹减速度 (m/s²) */
        const double stop_hold_s = 0.5;   /* 停稳驻留时长 (s) */
        while (v > 0.05 && n < max_points) {
            v += brake_decel * dt;
            if (v < 0.0) v = 0.0;
            x += v * std::cos(h) * dt;
            y += v * std::sin(h) * dt;

            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;  points[n].v = (float)v;
            points[n].kappa = 0.0f;
            points[n].a = (float)brake_decel;
            points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
        }
        v = 0.0;
        {
            double held = 0.0;
            while (n < max_points && held < stop_hold_s) {
                points[n].t_rel_us = t_us;
                points[n].x = (float)x;  points[n].y = (float)y;
                points[n].heading = (float)h;  points[n].v = 0.0f;
                points[n].kappa = 0.0f;
                points[n].a = 0.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
                points[n].l = road_l(x, y);
                n++;
                held += dt;
                t_us += (uint32_t)(dt * 1e6);
            }
        }
        if (n == 0) {
            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;  points[n].v = 0.0f;
            points[n].kappa = 0.0f;  points[n].a = 0.0f;
            points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
        }
    }

    /* ═══ Phase 2-4: 多把方向掉头（N-point，角点约束退出）═══ */
    const double road_half_width =
        ((env.lane_count >= 1) ? (double)env.lane_count : 2.0) * 0.5 * env.lane_width;
    const double guardrail_margin = 1.0;
    const double uturn_corner_limit = road_half_width - guardrail_margin;
    const double uturn_reverse_corridor = 0.5;
    const int    uturn_max_strokes = 5;
    const double uturn_stroke_timeout_s = 4.0;
    const double uturn_reverse_timeout_s = 3.0;
    const double uturn_reverse_h_guard = 0.30;
    const double half_body_len = 2.3;
    const double half_body_wid = 1.0;

    const int lc = (env.lane_count >= 2) ? env.lane_count : 2;
    const double side_offset = planning_coord::lane_group_side_offset(
        lc, env.lane_width, env.road_oneway != 0);
    double target_lane_center_y;
    if (env.road_oneway) {
        target_lane_center_y = planning_coord::lane_center_d(0, lc, env.lane_width, side_offset);
    } else {
        target_lane_center_y =
            (uturn_target_h < 0.5)
                ? planning_coord::lane_center_d(lc / 2, lc, env.lane_width, side_offset)
                : planning_coord::lane_center_d(lc / 2 - 1, lc, env.lane_width, side_offset);
    }
    const double stroke_dir = (uturn_target_h < 0.5) ? -1.0 : 1.0;

    bool uturn_done = false;
    for (int stroke = 0; stroke < uturn_max_strokes && !uturn_done && n < max_points; stroke++) {
        /* ── 前进弧 ── */
        v = uturn_speed;
        double stroke_t = 0.0;
        while (n < max_points) {
            double ramp = std::min(1.0, stroke_t / 1.2);
            steer = uturn_steer * ramp;
            double yaw_rate = (v / wheelbase) * std::tan(steer);
            h += yaw_rate * dt;
            x += v * std::cos(h) * dt - half_wb * std::sin(h) * yaw_rate * dt;
            y += v * std::sin(h) * dt + half_wb * std::cos(h) * yaw_rate * dt;

            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;  points[n].v = (float)v;
            points[n].kappa = (float)(std::tan(steer) / wheelbase);
            points[n].a = 0.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            stroke_t += dt;
            t_us += (uint32_t)(dt * 1e6);

            auto rect_max_abs_y = [&](double hh) {
                double c[4] = {
                    y + half_body_len * std::sin(hh) + half_body_wid * std::cos(hh),
                    y + half_body_len * std::sin(hh) - half_body_wid * std::cos(hh),
                    y - half_body_len * std::sin(hh) + half_body_wid * std::cos(hh),
                    y - half_body_len * std::sin(hh) - half_body_wid * std::cos(hh),
                };
                double m = 0.0;
                for (int k = 0; k < 4; k++) {
                    double ay = std::fabs(c[k]);
                    if (ay > m) m = ay;
                }
                return m;
            };
            double corner_y = rect_max_abs_y(h);
            bool corner_ok = (corner_y <= uturn_corner_limit);
            bool cz_ok = !body_hits_construction(
                env, x, y, h, half_body_len, half_body_wid, cz_margin);
            bool aligned = (std::fabs(norm_h(h - uturn_target_h)) < 0.10)
                        && (std::fabs(y - target_lane_center_y) < 1.75);
            if (!corner_ok || !cz_ok || aligned || stroke_t >= uturn_stroke_timeout_s) {
                uturn_done = aligned && cz_ok;
                break;
            }
        }
        if (uturn_done) break;

        /* 前进→倒车交界: 刹停点 v=0 */
        if (n < max_points && std::fabs(v) > 0.1) {
            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = 0.0f;
            points[n].kappa = 0.0f;
            points[n].a = -5.0f;
            points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
            v = 0.0;
        }

        /* ── 倒车：反打满舵，heading 继续向目标转，中心回撤走廊 ── */
        steer = -uturn_steer;
        v = reverse_speed;
        double rev_t = 0.0;
        while (n < max_points) {
            double yaw_rate = (v / wheelbase) * std::tan(steer);
            h += yaw_rate * dt;
            x += v * std::cos(h) * dt - half_wb * std::sin(h) * yaw_rate * dt;
            y += v * std::sin(h) * dt + half_wb * std::cos(h) * yaw_rate * dt;

            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = (float)v;
            points[n].kappa = (float)(std::tan(steer) / wheelbase);
            points[n].a = 0.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            rev_t += dt;
            t_us += (uint32_t)(dt * 1e6);

            bool corridor = (stroke_dir > 0) ? (y <= uturn_reverse_corridor)
                                             : (y >= -uturn_reverse_corridor);
            double hn = norm_h(h);
            bool h_guard = (stroke_dir > 0) ? (hn >= uturn_target_h - uturn_reverse_h_guard)
                                            : (hn <= -M_PI + uturn_reverse_h_guard);
            bool cz_hit = body_hits_construction(
                env, x, y, h, half_body_len, half_body_wid, cz_margin);
            if (corridor || h_guard || cz_hit || rev_t >= uturn_reverse_timeout_s) break;
        }

        /* 倒车→前进交界: 刹停点 v=0（换挡 R→D） */
        if (n < max_points && std::fabs(v) > 0.1) {
            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = 0.0f;
            points[n].kappa = 0.0f;
            points[n].a = -5.0f;
            points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
            v = 0.0;
        }
    }

    /* ═══ Phase 5: 巡航 → 剩余点以目标速度直行填充 ═══ */
    {
        steer = 0.0;
        v = env.cfg_target_speed;
        while (n < max_points) {
            double nx = x + v * std::cos(h) * dt;
            double ny = y + v * std::sin(h) * dt;
            if (body_hits_construction(env, nx, ny, h, half_body_len, half_body_wid, cz_margin)) {
                points[n].t_rel_us = t_us;
                points[n].x = (float)x;  points[n].y = (float)y;
                points[n].heading = (float)h;
                points[n].v = 0.0f;
                points[n].kappa = 0.0f;
                points[n].a = -5.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
                points[n].l = road_l(x, y);
                n++;
                break;
            }
            x = nx; y = ny;

            points[n].t_rel_us = t_us;
            points[n].x = (float)x;  points[n].y = (float)y;
            points[n].heading = (float)h;
            points[n].v = (float)v;
            points[n].kappa = 0.0f;
            points[n].a = 0.0f;  points[n].jerk = 0.0f;  points[n].s = 0.0f;
            points[n].l = road_l(x, y);
            n++;
            t_us += (uint32_t)(dt * 1e6);
        }
    }

    /* 截断兜底：64 点 (max_points) 在高速触发时可能截断在倒车段（v<0）。 */
    if (n > 0 && points[n - 1].v < -0.1) {
        points[n - 1].v = (float)uturn_speed;
        LOG_WARN("planning", "[UTURN] trajectory truncated in reverse segment — force end point v=%.1f (n=%d/%d)",
                 uturn_speed, n, max_points);
    }

    return n;
}

}  // namespace planning_uturn
