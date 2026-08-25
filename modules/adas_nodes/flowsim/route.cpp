/**
 * route.cpp — Route 构建与查询实现
 */

#include "route.h"

#include "road_network.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace flowsim {

namespace {
/** 归一化到 [0,π] 的航向差绝对值。 */
double heading_gap(double a, double b) {
    double d = a - b;
    while (d > M_PI)  d -= 2.0 * M_PI;
    while (d < -M_PI) d += 2.0 * M_PI;
    return std::fabs(d);
}
}  // namespace

bool Route::build(FlowRoadNetwork& roads, double tol) {
    segs_.clear();
    total_ = 0.0;

    const int n = roads.road_count();
    if (n <= 0) return false;

    struct RoadEnds {
        int    id{0};
        double len{0.0};
        double sx{0}, sy{0}, sh{0};   // start 端点 + 航向
        double ex{0}, ey{0}, eh{0};   // end 端点 + 航向
        bool   used{false};
    };
    std::vector<RoadEnds> rs;
    rs.reserve(n);

    for (int i = 0; i < n; ++i) {
        RoadInfo info;
        if (!roads.road_info(i, info) || info.length <= 0.0) continue;
        WorldPos a, b;
        if (!roads.frenet_to_world((int)info.id, 0, 0.0, 0.0, a)) continue;
        if (!roads.frenet_to_world((int)info.id, 0, info.length, 0.0, b)) continue;
        rs.push_back({(int)info.id, info.length, a.x, a.y, a.h, b.x, b.y, b.h, false});
    }
    if (rs.empty()) return false;

    // 选起点：start 端点不与任何其它 road 的 end 端点重合的 road（无前驱）。
    auto is_pred = [&](const RoadEnds& p, const RoadEnds& q) {
        return std::hypot(p.ex - q.sx, p.ey - q.sy) < tol;
    };
    int seed = -1;
    for (size_t i = 0; i < rs.size(); ++i) {
        bool has_pred = false;
        for (size_t j = 0; j < rs.size(); ++j) {
            if (i == j) continue;
            if (is_pred(rs[j], rs[i])) { has_pred = true; break; }
        }
        if (!has_pred) { seed = (int)i; break; }
    }
    if (seed < 0) seed = 0;  // 全环路：随便从 0 起

    // 沿后继链构建
    int    cur = seed;
    double acc = 0.0;
    while (cur >= 0 && !rs[cur].used) {
        rs[cur].used = true;
        segs_.push_back({rs[cur].id, rs[cur].len, acc});
        acc += rs[cur].len;

        int    best = -1;
        double best_score = 1e18;
        for (size_t j = 0; j < rs.size(); ++j) {
            if (rs[j].used) continue;
            double d = std::hypot(rs[cur].ex - rs[j].sx, rs[cur].ey - rs[j].sy);
            if (d > tol) continue;
            // 航向连续优先；长度更长的减分（tiebreak：主路 > 短存根/急弯匝道）
            double score = heading_gap(rs[cur].eh, rs[j].sh) - 0.001 * rs[j].len;
            if (score < best_score) { best_score = score; best = (int)j; }
        }
        cur = best;
    }

    total_ = acc;
    return !segs_.empty();
}

bool Route::build_from_chain(FlowRoadNetwork& roads, const int* road_ids, int count) {
    segs_.clear();
    total_ = 0.0;
    if (!road_ids || count <= 0) return false;

    // 预建 road_id → length 索引（esmini road_count 通常不大，线性扫描即可）。
    struct LenById { int id; double len; };
    std::vector<LenById> lens;
    const int n = roads.road_count();
    lens.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        RoadInfo info;
        if (roads.road_info(i, info) && info.length > 0.0)
            lens.push_back({(int)info.id, info.length});
    }
    if (lens.empty()) return false;

    double acc = 0.0;
    for (int i = 0; i < count; ++i) {
        int rid = road_ids[i];
        // 图外 road（不在 xodr 里）截断，保留已链部分。
        auto it = std::find_if(lens.begin(), lens.end(),
                               [rid](const LenById& l) { return l.id == rid; });
        if (it == lens.end()) break;

        /* ── 路口盒缺口桥接（2026-08-15 OSM）：chain 相邻段在路口盒处几何断开
         * （OSM way 在交叉口节点分割，盒内无道路几何，实测 gap 10-25m）。
         * 直接拼接会让 ref_path 瞬移、控制器在缺口处被"拽"（陆家嘴隧道东端
         * 掉头段 21.8m 缺口 ego 原地打转实测）。
         *
         * dh>10° 的转角用「直线—圆弧—直线」fillet 桥接（道路工程标准做法）：
         *   两条端点切线求交 I，t = R·tan(dh/2) 定切点 T0/T1，R=10m 目标半径
         *   （κ=0.1，转向/舒适性双达标）；端点切线方向偏斜时端点弦并不关于
         *   交角平分线对称，三次 Hermite 端点 κ 会飙到 0.4~1.7（陆家嘴实测，
         *   R=0.6m，物理极限 0.25）→ planning 可行性连续 invalid → control
         *   跌 ROAD_GUARD 冲出路沿卡死。真圆弧 κ 恒定 1/R，无此问题。
         *   t 超出盒内可用直行段（d0/d1）时修剪相邻真实段（prev.length 退 /
         *   next.s0 进），让弧扫过路口盒内部——与真实右转几何一致。
         * 交点在端点后方（S 形/反向几何）或近平行时回退 Hermite 直桥。 */
        if (!segs_.empty()) {
            RouteSeg& prev = segs_.back();
            WorldPos pe, ns;
            bool ok_prev = !prev.is_virtual
                ? roads.frenet_to_world(prev.road_id, 0, prev.s0 + prev.length, 0.0, pe)
                : false;
            bool ok_next = roads.frenet_to_world(it->id, 0, 0.0, 0.0, ns);
            if (prev.is_virtual && !prev.pts.empty()) {
                pe.x = prev.pts.back().x; pe.y = prev.pts.back().y;
                pe.h = prev.pts.back().h; pe.z = 0.0;
                ok_prev = true;
            }
            if (ok_prev && ok_next) {
                double gap = std::hypot(ns.x - pe.x, ns.y - pe.y);
                double dh  = heading_gap(pe.h, ns.h);
                /* 桥接触发：gap>2m 的几何断开，或 dh>10° 的航向跳变（含 gap≈0
                 * 的尖角——roads 在路口盒边界正好相接但方向差 87°，不桥接就是
                 * κ=∞ 折角，planning 可行性必失败）。 */
                if (gap > 2.0 || dh > 0.17) {
                    double u0x = std::cos(pe.h), u0y = std::sin(pe.h);
                    double u1x = std::cos(ns.h), u1y = std::sin(ns.h);
                    double cross01 = u0x * u1y - u0y * u1x;
                    double dpx = ns.x - pe.x, dpy = ns.y - pe.y;
                    /* 交点参数：I = pe + d0·u0 = ns − d1·u1（d0/d1 = 盒内可用直行） */
                    double d0 = -1.0, d1 = -1.0;
                    bool use_arc = (dh > 0.17 && std::fabs(cross01) > 1e-3);
                    if (use_arc) {
                        d0 = (dpx * u1y - dpy * u1x) / cross01;
                        double bb = (dpx * u0y - dpy * u0x) / cross01;
                        d1 = -bb;
                        /* 交点可落于端点后方 ≤5m（trim 补偿，含 d0=d1=0 的
                         * 端点重合尖角——必须两侧各修 t 才有 fillet 空间）；
                         * 再远即 S 形/反向几何 → 回退 Hermite。 */
                        if (d0 < -5.0 || d1 < -5.0) use_arc = false;
                    }
                    /* 构建诊断：每个桥接点打印原始几何（一次构建，量小） */
                    std::fprintf(stderr,
                        "[route] bridge prev_end(%.1f,%.1f h=%.0f°) next_start(%.1f,%.1f h=%.0f°) gap=%.1f dh=%.0f° d0=%.1f d1=%.1f arc=%d\n",
                        pe.x, pe.y, pe.h * 180.0 / M_PI, ns.x, ns.y, ns.h * 180.0 / M_PI,
                        gap, dh * 180.0 / M_PI, d0, d1, (int)use_arc);
                    RouteSeg vs;
                    vs.road_id = -1;
                    vs.is_virtual = true;
                    vs.s_start = acc;
                    double next_s0 = 0.0;
                    if (use_arc) {
                        /* fillet 半径目标 20m（κ=0.05）：必须落在巡航转向
                         * 能力带内——control 巡航 steer 钳幅 0.16rad → 可跟踪
                         * κ≤0.06 (R≥16.8m)；机动模式阈值 κ>0.12 (R<8.3m)。
                         * κ∈(0.06,0.12] 是死带：巡航跟不上、机动不触发。
                         * R=10m (κ=0.10) 正落死带（陆家嘴实测：弯中 lat_err
                         * 漂到 15m → ROAD_GUARD 卡死）。R=20 留 16% 余量，
                         * ST 弯速 v_lim=0.85·sqrt(5/0.05)=8.5m/s。 */
                        const double R_TARGET = 20.0;
                        double dh_signed = std::atan2(cross01, u0x * u1x + u0y * u1y);
                        double t_need = R_TARGET * std::tan(dh * 0.5);
                        double tmax_prev = prev.is_virtual ? 0.0 : prev.length * 0.4;
                        double tmax_next = it->len * 0.4;
                        if (tmax_prev > 25.0) tmax_prev = 25.0;
                        if (tmax_next > 25.0) tmax_next = 25.0;
                        double t_eff = t_need;
                        if (t_eff > d0 + tmax_prev) t_eff = d0 + tmax_prev;
                        if (t_eff > d1 + tmax_next) t_eff = d1 + tmax_next;
                        if (t_eff < 0.3) t_eff = 0.3;
                        double r_eff = t_eff / std::tan(dh * 0.5);
                        double trim_prev = (t_eff > d0) ? (t_eff - d0) : 0.0;
                        double trim_next = (t_eff > d1) ? (t_eff - d1) : 0.0;
                        /* 修剪 prev 段尾 / next 段首（真实段几何采样为桥接端点） */
                        WorldPos f0 = pe, f1 = ns;
                        if (trim_prev > 1e-6 && !prev.is_virtual) {
                            if (roads.frenet_to_world(prev.road_id, 0,
                                    prev.s0 + prev.length - trim_prev, 0.0, f0)) {
                                prev.length -= trim_prev;
                                acc         -= trim_prev;
                                vs.s_start   = acc;
                            } else { f0 = pe; }
                        }
                        if (trim_next > 1e-6) {
                            if (roads.frenet_to_world(it->id, 0, trim_next, 0.0, f1)) {
                                next_s0 = trim_next;
                            } else { f1 = ns; }
                        }
                        double ix = pe.x + d0 * u0x, iy = pe.y + d0 * u0y;
                        double t0x = ix - t_eff * u0x, t0y = iy - t_eff * u0y;
                        double t1x = ix + t_eff * u1x, t1y = iy + t_eff * u1y;
                        /* 折线组装：f0 →(直行)→ T0 →(圆弧)→ T1 →(直行)→ f1 */
                        auto push_pt = [&](double x, double y, double h) {
                            if (!vs.pts.empty())
                                vs.length += std::hypot(x - vs.pts.back().x,
                                                        y - vs.pts.back().y);
                            vs.pts.push_back({x, y, h, 0.0, 0.0});
                        };
                        push_pt(f0.x, f0.y, f0.h);
                        /* 前直行段（f0→T0）：间隔 ~2m */
                        double d_str0 = std::hypot(t0x - f0.x, t0y - f0.y);
                        int n_str0 = (int)(d_str0 / 2.0);
                        for (int k = 1; k <= n_str0; ++k) {
                            double f = (double)k / (n_str0 + 1);
                            push_pt(f0.x + (t0x - f0.x) * f, f0.y + (t0y - f0.y) * f,
                                    std::atan2(t0y - f0.y, t0x - f0.x));
                        }
                        /* 圆弧段：T0 起绕圆心转 |dh|，σ=转向符号 */
                        double sigma = (dh_signed >= 0.0) ? 1.0 : -1.0;
                        double nx = -sigma * u0y, ny = sigma * u0x;  /* 转向侧法向 */
                        double cx = t0x + r_eff * nx, cy = t0y + r_eff * ny;
                        double a0 = std::atan2(t0y - cy, t0x - cx);
                        int n_arc = (int)(dh * r_eff / 1.5) + 2;
                        if (n_arc < 4) n_arc = 4;
                        if (n_arc > 20) n_arc = 20;
                        for (int k = 0; k <= n_arc; ++k) {
                            double phi = dh * (double)k / n_arc;
                            double aa = a0 + sigma * phi;
                            push_pt(cx + r_eff * std::cos(aa), cy + r_eff * std::sin(aa),
                                    pe.h + sigma * phi);
                        }
                        /* 后直行段（T1→f1） */
                        double d_str1 = std::hypot(f1.x - t1x, f1.y - t1y);
                        int n_str1 = (int)(d_str1 / 2.0);
                        for (int k = 1; k <= n_str1; ++k) {
                            double f = (double)k / (n_str1 + 1);
                            push_pt(t1x + (f1.x - t1x) * f, t1y + (f1.y - t1y) * f,
                                    std::atan2(f1.y - t1y, f1.x - t1x));
                        }
                        push_pt(f1.x, f1.y, f1.h);
                    } else {
                        /* 近直线/退化几何：三次 Hermite 直桥，切向量 chord/3 */
                        double chord = std::hypot(ns.x - pe.x, ns.y - pe.y);
                        double L = chord / 3.0;
                        double m0x = u0x * L, m0y = u0y * L;
                        double m1x = u1x * L, m1y = u1y * L;
                        int npts = (int)(chord / 2.0) + 2;
                        if (npts < 6) npts = 6;
                        if (npts > 24) npts = 24;
                        double px = pe.x, py = pe.y;
                        for (int k = 0; k < npts; ++k) {
                            double t = (double)k / (npts - 1);
                            double t2 = t * t, t3 = t2 * t;
                            double h00 =  2.0 * t3 - 3.0 * t2 + 1.0;
                            double h10 =  t3 - 2.0 * t2 + t;
                            double h01 = -2.0 * t3 + 3.0 * t2;
                            double h11 =  t3 - t2;
                            double bx = h00 * pe.x + h10 * m0x + h01 * ns.x + h11 * m1x;
                            double by = h00 * pe.y + h10 * m0y + h01 * ns.y + h11 * m1y;
                            double d00 =  6.0 * t2 - 6.0 * t;
                            double d10 =  3.0 * t2 - 4.0 * t + 1.0;
                            double d01 = -6.0 * t2 + 6.0 * t;
                            double d11 =  3.0 * t2 - 2.0 * t;
                            double dx = d00 * pe.x + d10 * m0x + d01 * ns.x + d11 * m1x;
                            double dy = d00 * pe.y + d10 * m0y + d01 * ns.y + d11 * m1y;
                            double bh = std::atan2(dy, dx);
                            if (k > 0) vs.length += std::hypot(bx - px, by - py);
                            vs.pts.push_back({bx, by, bh, 0.0, 0.0});
                            px = bx; py = by;
                        }
                    }
                    acc += vs.length;
                    segs_.push_back(std::move(vs));
                    segs_.push_back({it->id, it->len - next_s0, acc});
                    segs_.back().s0 = next_s0;
                    acc += it->len - next_s0;
                    continue;
                }
            }
        }

        segs_.push_back({it->id, it->len, acc});
        acc += it->len;
    }

    total_ = acc;
    return !segs_.empty();
}

int Route::index_of(int road_id) const {
    for (size_t i = 0; i < segs_.size(); ++i) {
        if (segs_[i].road_id == road_id) return (int)i;
    }
    return -1;
}

void Route::locate(double route_s, int& road_id, double& s_local, int& route_idx) const {
    if (segs_.empty()) { road_id = 0; s_local = 0; route_idx = -1; return; }
    if (route_s < 0.0)      route_s = 0.0;
    if (route_s > total_)   route_s = total_;

    for (size_t i = 0; i < segs_.size(); ++i) {
        double seg_end = segs_[i].s_start + segs_[i].length;
        if (route_s <= seg_end || i + 1 == segs_.size()) {
            route_idx = (int)i;
            road_id   = segs_[i].road_id;
            s_local   = route_s - segs_[i].s_start;
            if (s_local < 0.0)              s_local = 0.0;
            if (s_local > segs_[i].length)  s_local = segs_[i].length;
            /* s0（fillet 段首修剪）在此折进返回值：所有 locate→frenet_to_world
             * 的外部消费方（flowsim u_turn 判据/start override/npc_ai）拿到的
             * 直接是 esmini s，无需各自知悉修剪。虚拟段 s0=0 无影响。 */
            s_local += segs_[i].s0;
            return;
        }
    }
    // 理论不可达（上面循环末段兜底）
    route_idx = (int)segs_.size() - 1;
    road_id   = segs_.back().road_id;
    s_local   = segs_.back().length + segs_.back().s0;
}

double Route::to_route_s(int route_idx, double s_local) const {
    if (route_idx < 0 || route_idx >= (int)segs_.size()) return 0.0;
    /* 入参按 esmini s 语义（与 locate 返回值一致）→ 折掉 s0 得段内局部 s。
     * s0=0 的段（NPC 自动链/测试）行为不变。 */
    return segs_[route_idx].s_start + (s_local - segs_[route_idx].s0);
}

bool Route::sample_pose(FlowRoadNetwork& roads, double route_s,
                        double& x, double& y, double& h) const {
    int rid = 0, ridx = -1;
    double sl = 0.0;
    locate(route_s, rid, sl, ridx);
    if (ridx < 0 || ridx >= (int)segs_.size()) return false;
    const RouteSeg& sg = segs_[ridx];
    if (sg.is_virtual) {
        /* 合成折线插值：s_local 沿折线弧长推进，航向取所在微段方向 */
        if (sg.pts.size() < 2) return false;
        double remain = sl;
        for (size_t i = 0; i + 1 < sg.pts.size(); ++i) {
            double sx = sg.pts[i + 1].x - sg.pts[i].x;
            double sy = sg.pts[i + 1].y - sg.pts[i].y;
            double seg_len = std::hypot(sx, sy);
            if (remain <= seg_len || i + 2 == sg.pts.size()) {
                double t = (seg_len > 1e-9) ? (remain / seg_len) : 0.0;
                if (t > 1.0) t = 1.0;
                x = sg.pts[i].x + sx * t;
                y = sg.pts[i].y + sy * t;
                h = std::atan2(sy, sx);
                return true;
            }
            remain -= seg_len;
        }
        x = sg.pts.back().x;
        y = sg.pts.back().y;
        h = sg.pts.back().h;
        return true;
    }
    WorldPos wp;
    /* sl 已在 locate() 内折入 s0（fillet 段首修剪），直接作 esmini s 用。 */
    if (!roads.frenet_to_world(rid, 0, sl, 0.0, wp)) return false;
    x = wp.x;
    y = wp.y;
    h = wp.h;
    return true;
}

double Route::project(FlowRoadNetwork& roads, double x, double y,
                      double hint_route_s, double window) const {
    if (segs_.empty()) return -1.0;

    double lo = 0.0, hi = total_;
    if (hint_route_s >= 0.0) {
        lo = hint_route_s - window;
        hi = hint_route_s + window;
        if (lo < 0.0)    lo = 0.0;
        if (hi > total_) hi = total_;
    }

    /* 粗采样：~2m 间距扫窗口内 route 中心线，找最近点。 */
    const double COARSE_STEP = 2.0;
    double best_d2 = 1e18, best_rs = -1.0;
    for (double rs = lo; rs <= hi + 1e-6; rs += COARSE_STEP) {
        double px = 0.0, py = 0.0, ph = 0.0;
        if (!sample_pose(roads, rs, px, py, ph)) continue;
        double dx = px - x, dy = py - y;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best_rs = rs; }
        if (rs >= hi) break;  /* 浮点循环边界 */
    }
    if (best_rs < 0.0) return -1.0;

    /* 精化：最优点 ±2m 内 0.25m 步长复扫（粗网格在弯道的弦高误差 ~cm 级，
     * 对 route_s 跟踪已够；精化只为消除 2m 量化台阶导致的 ref_path 抖动）。 */
    double fine_lo = best_rs - COARSE_STEP, fine_hi = best_rs + COARSE_STEP;
    if (fine_lo < lo) fine_lo = lo;
    if (fine_hi > hi) fine_hi = hi;
    for (double rs = fine_lo; rs <= fine_hi + 1e-6; rs += 0.25) {
        double px = 0.0, py = 0.0, ph = 0.0;
        if (!sample_pose(roads, rs, px, py, ph)) continue;
        double dx = px - x, dy = py - y;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best_rs = rs; }
        if (rs >= fine_hi) break;
    }
    return best_rs;
}

int Route::sample_ahead(FlowRoadNetwork& roads, double route_s_start,
                        double lookahead, double step_m,
                        std::vector<RefPathPoint>& out,
                        bool reverse) const {
    out.clear();
    if (segs_.empty() || lookahead <= 0.0 || step_m <= 0.0) return 0;

    // 起点 route_s 夹到 [0, total]
    if (route_s_start < 0.0)      route_s_start = 0.0;
    if (route_s_start > total_)   route_s_start = total_;

    auto norm_angle = [](double a) {
        while (a >  M_PI) a -= 2.0 * M_PI;
        while (a < -M_PI) a += 2.0 * M_PI;
        return a;
    };

    // 第一遍：按行进方向采样原始点 (route_s, x, y, h)。
    //   forward: route_s 从 start 递增到 start+lookahead，heading = 道路切线
    //   reverse: route_s 从 start 递减到 start-lookahead，heading = 道路切线 + π
    // 两种情况 out 均按「行进方向前方」有序（out[0]=起点）。
    struct Raw { double rs; double x; double y; double h; };
    std::vector<Raw> raw;
    raw.reserve(static_cast<size_t>(lookahead / step_m) + 2);

    const double dir = reverse ? -1.0 : +1.0;
    double rs_end = route_s_start + dir * lookahead;
    if (rs_end > total_) rs_end = total_;
    if (rs_end < 0.0)    rs_end = 0.0;

    for (double rs = route_s_start;
         reverse ? (rs >= rs_end - 1e-6) : (rs <= rs_end + 1e-6);
         rs += dir * step_m) {
        double sx = 0.0, sy = 0.0, sh = 0.0;
        if (!sample_pose(roads, rs, sx, sy, sh)) continue;
        double h = reverse ? norm_angle(sh + M_PI) : sh;
        raw.push_back({rs, sx, sy, h});
        if (rs == rs_end) break;  // 避免浮点循环多采一个
    }
    if (raw.size() < 2) {
        // 至少把所有原始点填进去（即使无法估计 kappa）
        for (const auto& r : raw) {
            out.push_back({r.x, r.y, r.h, 0.0, r.rs});
        }
        return static_cast<int>(out.size());
    }

    // 第二遍：中心差分估计曲率 kappa（在行进坐标系下，raw 已按行进方向有序）
    //   相邻两点的转角 dtheta = normalize(h[i+1] - h[i-1])
    //   弦长 chord = hypot(dx, dy)
    //   kappa = dtheta / chord
    // 端点用前向/后向差分
    for (size_t i = 0; i < raw.size(); ++i) {
        double dtheta = 0.0, chord = 0.0;
        if (i == 0) {
            dtheta = norm_angle(raw[1].h - raw[0].h);
            chord  = std::hypot(raw[1].x - raw[0].x, raw[1].y - raw[0].y);
        } else if (i + 1 == raw.size()) {
            dtheta = norm_angle(raw[i].h - raw[i - 1].h);
            chord  = std::hypot(raw[i].x - raw[i - 1].x, raw[i].y - raw[i - 1].y);
        } else {
            dtheta = norm_angle(raw[i + 1].h - raw[i - 1].h);
            chord  = std::hypot(raw[i + 1].x - raw[i - 1].x, raw[i + 1].y - raw[i - 1].y);
        }
        double kappa = (chord > 1e-6) ? (dtheta / chord) : 0.0;
        out.push_back({raw[i].x, raw[i].y, raw[i].h, kappa, raw[i].rs});
    }

    return static_cast<int>(out.size());
}

}  // namespace flowsim
