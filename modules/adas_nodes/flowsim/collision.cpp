/**
 * collision.cpp — 碰撞检测实现
 *
 * 两阶段：
 *   1. AABB broad-phase：用 |Δx| < (len1+len2)/2 && |Δy| < (wid1+wid2)/2 快速排除
 *   2. OBB SAT narrow-phase：对 AABB 通过的对，做 4 轴分离投影判定
 *
 * SAT 2D 矩形：
 *   - 每个盒子的局部 x 轴方向 (cos h, sin h) 和 y 轴方向 (-sin h, cos h)
 *   - 投影两盒子 4 角点到轴上，检查区间是否重叠
 *   - 任一轴分离 → 不相交
 */

#include "collision.h"
#include "road_network.h"
#include "lane_frenet.h"
#include "npc_yield.h"

#include <cmath>
#include <algorithm>

namespace flowsim {

/* ── OBB 4 角点（中心 + 尺寸 + 航向）── */
static void obb_corners(double cx, double cy, double len, double wid, double h,
                        double out[4][2]) {
    double hl = len * 0.5, hw = wid * 0.5;
    double ch = std::cos(h), sh = std::sin(h);
    // 局部坐标 4 角点：(hl,hw)(-hl,hw)(-hl,-hw)(hl,-hw)
    double local[4][2] = {{hl, hw}, {-hl, hw}, {-hl, -hw}, {hl, -hw}};
    for (int i = 0; i < 4; ++i) {
        out[i][0] = cx + local[i][0] * ch - local[i][1] * sh;
        out[i][1] = cy + local[i][0] * sh + local[i][1] * ch;
    }
}

/* ── 投影点到轴，返回 [min,max] ── */
static void project(double corners[4][2], double ax, double ay,
                    double& out_min, double& out_max) {
    out_min = out_max = corners[0][0] * ax + corners[0][1] * ay;
    for (int i = 1; i < 4; ++i) {
        double p = corners[i][0] * ax + corners[i][1] * ay;
        if (p < out_min) out_min = p;
        if (p > out_max) out_max = p;
    }
}

bool obb_intersect(double cx1, double cy1, double len1, double wid1, double h1,
                   double cx2, double cy2, double len2, double wid2, double h2) {
    double c1[4][2], c2[4][2];
    obb_corners(cx1, cy1, len1, wid1, h1, c1);
    obb_corners(cx2, cy2, len2, wid2, h2, c2);

    // 4 个分离轴：盒1 的 x/y 方向 + 盒2 的 x/y 方向
    double axes[4][2] = {
        {std::cos(h1),  std::sin(h1)},  // 盒1 局部 x
        {-std::sin(h1), std::cos(h1)},  // 盒1 局部 y
        {std::cos(h2),  std::sin(h2)},  // 盒2 局部 x
        {-std::sin(h2), std::cos(h2)},  // 盒2 局部 y
    };

    for (int i = 0; i < 4; ++i) {
        double min1, max1, min2, max2;
        project(c1, axes[i][0], axes[i][1], min1, max1);
        project(c2, axes[i][0], axes[i][1], min2, max2);
        // 任一轴上区间不重叠 → 分离 → 不相交
        if (max1 < min2 || max2 < min1) return false;
    }
    return true;  // 4 轴都重叠 → 相交
}

/* Minimum translation vector for two intersecting OBBs. Returns false when
 * they are not intersecting; otherwise `out_ax/out_ay` is a unit axis pointing
 * from box 1 to box 2 and `out_depth` is the smallest overlap on that axis. */
static bool obb_mtv(double cx1, double cy1, double len1, double wid1, double h1,
                    double cx2, double cy2, double len2, double wid2, double h2,
                    double& out_ax, double& out_ay, double& out_depth) {
    double c1[4][2], c2[4][2];
    obb_corners(cx1, cy1, len1, wid1, h1, c1);
    obb_corners(cx2, cy2, len2, wid2, h2, c2);
    double axes[4][2] = {
        {std::cos(h1),  std::sin(h1)},
        {-std::sin(h1), std::cos(h1)},
        {std::cos(h2),  std::sin(h2)},
        {-std::sin(h2), std::cos(h2)},
    };
    double best_depth = 1e9, best_ax = 1.0, best_ay = 0.0;
    for (int i = 0; i < 4; ++i) {
        double min1, max1, min2, max2;
        project(c1, axes[i][0], axes[i][1], min1, max1);
        project(c2, axes[i][0], axes[i][1], min2, max2);
        if (max1 < min2 || max2 < min1) return false;
        const double depth = std::min(max1, max2) - std::max(min1, min2);
        if (depth < best_depth) {
            best_depth = depth;
            best_ax = axes[i][0];
            best_ay = axes[i][1];
        }
    }
    /* Orient the axis from box 1 to box 2 so both boxes are pushed apart
     * in the correct direction. */
    if ((cx2 - cx1) * best_ax + (cy2 - cy1) * best_ay < 0.0) {
        best_ax = -best_ax;
        best_ay = -best_ay;
    }
    out_ax = best_ax;
    out_ay = best_ay;
    out_depth = best_depth;
    return true;
}

int detect_collisions(const EntityPool& pool, std::vector<CollisionPair>& pairs) {
    pairs.clear();
    int n = pool.size();
    for (int i = 0; i < n; ++i) {
        const Entity& a = pool[i];
        if (!a.active || !a.is_vehicle()) continue;
        for (int j = i + 1; j < n; ++j) {
            const Entity& b = pool[j];
            if (!b.active || !b.is_vehicle()) continue;

            // 任一方处于碰撞冷却期 → 跳过该对的碰撞检测。
            // 否则 detect_collisions 每 tick 仍报告重叠的车对，apply_collision_response
            // 又把 crash_cooldown 重置为 0.5s → 冷却永远归零不了 → 两车永久卡死重叠。
            if (a.crash_cooldown > 0.0 || b.crash_cooldown > 0.0) continue;

            // Broad-phase: 旋转安全 3D AABB 快速排除（含 Z 高程隔离，防止高架与地面车误判）。
            // 车辆大幅偏转（掉头/垂直泊车 swing-out）时，世界系 Y 投影半长可达 length/2，
            // 若沿用未旋转的 width*0.5 会错误过滤掉真实的 OBB 相交。
            double dz = std::fabs(a.z - b.z);
            if (dz > 2.2) continue;
            const double ca = std::fabs(std::cos(a.heading));
            const double sa = std::fabs(std::sin(a.heading));
            const double cb = std::fabs(std::cos(b.heading));
            const double sb = std::fabs(std::sin(b.heading));
            double dx = std::fabs(a.x - b.x);
            double dy = std::fabs(a.y - b.y);
            double aabb_x = 0.5 * (a.length * ca + a.width * sa + b.length * cb + b.width * sb);
            double aabb_y = 0.5 * (a.length * sa + a.width * ca + b.length * sb + b.width * cb);
            if (dx > aabb_x || dy > aabb_y) continue;

            // Narrow-phase: OBB SAT
            if (obb_intersect(a.x, a.y, a.length, a.width, a.heading,
                              b.x, b.y, b.length, b.width, b.heading)) {
                pairs.push_back({i, j});
            }
        }
    }
    return (int)pairs.size();
}

void apply_collision_response(EntityPool& pool, const std::vector<CollisionPair>& pairs) {
    for (const auto& p : pairs) {
        Entity& a = pool[p.a];
        Entity& b = pool[p.b];
        // 双方速度归零（模拟非弹性碰撞后停下）
        a.speed = 0; a.vx = 0; a.vy = 0;
        b.speed = 0; b.vx = 0; b.vy = 0;
        // 刹车踩下，防止下一 tick 又加速
        a.brake = 1.0; a.throttle = 0.0;
        b.brake = 1.0; b.throttle = 0.0;

        // E3: 物理分离改为沿 route_s 纵向推开，不做世界坐标任意方向弹飞。
        // 原实现沿中心连线推，两车如果有角度差（一个 heading 偏了），推的
        // 方向偏离道路方向 → 车被推到路外 → crash_cooldown 期间不刷新位置
        // → 冷却结束后在路外 world_to_frenet 失败 → 飞出路面朝向全乱。
        // 改为只沿 route 纵向拉开 route_s，横向 offset 和 heading 不动，
        // 下一帧 frenet_to_world 自然把车放回车道正确位置。
        if (a.route_dir != 0 && b.route_dir != 0) {
            double sep = 0.25 * (a.length + b.length) + 0.6;  // 双侧拉开 >= 车长+1.2m，彻底脱开 OBB
            double ds_a = (a.route_s >= b.route_s) ? sep : -sep;
            double ds_b = -ds_a;
            a.route_s += ds_a;
            b.route_s += ds_b;
            // world heading 与 route_dir * (+s) 一致，乘以 route_dir 使 (x,y) 与 route_s 同向分离
            double step_a = ds_a * (double)a.route_dir;
            double step_b = ds_b * (double)b.route_dir;
            a.x += step_a * std::cos(a.heading);
            a.y += step_a * std::sin(a.heading);
            b.x += step_b * std::cos(b.heading);
            b.y += step_b * std::sin(b.heading);
        } else {
            // 非 route 模式：世界系沿碰撞法线分离。
            // ego.route_dir 全代码库未被初始化（始终 0）；esmini/Route::build()
            // 失败时所有 NPC route_dir 也为 0。若不做分离，detect_collisions 每
            // tick 仍把重叠的车判定为碰撞，配合 cooldown 重置 → 永久卡死重叠。
            //
            // OBB SAT 不返回穿透深度，用 AABB 重叠量近似。采用 MTV（最小平移
            // 向量）思路：在 x/y 两轴上选重叠量较小的那根作为分离轴，各推一半
            // 穿透深度 + 小余量。这样保证一次分离就足以让 broad-phase AABB 不
            // 再通过（沿该轴完全脱开）。若沿中心连线方向推（task 草案写法），
            // 当中心连线方向与 max-overlap 轴对齐时，按 min-overlap 量推送无法
            // 真正脱开 → 下一 tick 仍报碰撞 → cooldown 仍被重置 → 还是卡死。
            /* Use the OBB minimum-translation vector, not unrotated AABB
             * extents: when two rotated cars intersect at a corner, both
             * unrotated overlaps can clamp to zero and the pair only moves
             * 0.01m, re-colliding forever. */
            double mtv_ax = 1.0, mtv_ay = 0.0, mtv_depth = 0.0;
            if (obb_mtv(a.x, a.y, a.length, a.width, a.heading,
                        b.x, b.y, b.length, b.width, b.heading,
                        mtv_ax, mtv_ay, mtv_depth)) {
                const double push = mtv_depth * 0.5 + 0.02;
                a.x -= mtv_ax * push;
                a.y -= mtv_ay * push;
                b.x += mtv_ax * push;
                b.y += mtv_ay * push;
            }
        }

        // 碰撞冷却缩短到 0.5s：原 2.0s 太长，撞一次堵 2 秒整条路就塞死。
        // 0.5s 够避免同一帧/下一两帧重复碰撞检测，又能快速恢复行驶。
        // 冷却期间 step_npc_vehicle 仍执行 route 位置刷新（E3 修复），
        // 车不会停在错误位置。
        if (a.is_vehicle()) a.crash_cooldown = 0.5;
        if (b.is_vehicle()) b.crash_cooldown = 0.5;
    }
}

void apply_guardrail(Entity& e, FlowRoadNetwork& roads, double dt) {
    if (!e.active || !e.is_vehicle()) return;

    // 优先在车辆自身当前 road_id 上投影判定，防止交叉路口被 world_to_frenet 误吸到垂直道路。
    const bool has_road_track = is_road_tracked_vehicle(
        e.road_id, e.route_dir, e.road_pos.ok());
    FrenetPos f{};
    double ref_t = 0.0;
    bool have_road = false;
    bool from_own_road = false;
    if (has_road_track) {
        WorldPos own_wp{};
        if (roads.frenet_to_world(e.road_id, 0, std::max(0.0, e.s), 0.0, own_wp)) {
            const OwnRoadProjection proj = project_onto_road_tangent(
                e.x, e.y, e.z, own_wp.x, own_wp.y, own_wp.z, own_wp.h);
            if (proj.valid) {
                f.road_id = e.road_id;
                f.lane_id = 0;
                f.s = std::max(0.0, e.s);
                ref_t = proj.lat;
                have_road = true;
                from_own_road = true;
            }
        }
    }
    if (!have_road) {
        if (!roads.world_to_frenet(e.x, e.y, f, e.z)) return;
        // 已跟踪 road_id 的车辆在路口若 world_to_frenet 命中另一条相交 road，不做跨路护栏钳制
        if (has_road_track && f.road_id != e.road_id) return;
        ref_t = offset_from_lane_internal(roads, f.road_id, f.lane_id, f.s, f.offset);
    }

    double lw = roads.lane_width(f.road_id, 0, f.s);
    if (lw < 1.0 || lw > 10.0) lw = 3.5;
    std::vector<int> lane_ids = roads.drivable_lane_ids(f.road_id, f.s);
    if ((int)lane_ids.size() < 2) return;       // 单车道/未知，不做护栏约束

    double min_t = 0.0, max_t = 0.0;
    compute_guardrail_bounds(lane_ids, lw, 0.6, min_t, max_t);
    const GuardrailClampAction act = compute_guardrail_clamp_action(ref_t, min_t, max_t);
    if (!act.triggered) return;

    WorldPos wp;
    if (!roads.frenet_to_world(f.road_id, 0, f.s, act.clamped_t, wp)) return;
    e.x = wp.x;
    e.y = wp.y;
    if (from_own_road || f.road_id == e.road_id || e.road_id < 0) {
        e.offset = act.clamped_t;
    }

    // 有界减速（已由 road_pos/route 跟踪的车不设 cooldown，避免 cooldown 结束与 road_pos 回弹）
    const double step_dt = (dt > 1e-4) ? dt : (1.0 / 60.0);
    e.speed = std::max(0.0, e.speed - 8.5 * step_dt);
    e.vx = e.speed * std::cos(e.heading);
    e.vy = e.speed * std::sin(e.heading);
    if (!has_road_track && act.intrusion_m >= 0.8 && e.crash_cooldown < 0.3) {
        e.crash_cooldown = 0.3;
    }
}

void apply_gravity(Entity& e, FlowRoadNetwork& roads, double dt) {
    if (!e.active || !e.is_vehicle()) return;
    const double G = 9.81;
    e.vz -= G * dt;   // 重力加速度（垂直向下）

    // 查询所在位置的道路高度作为支撑面
    FrenetPos f;
    bool on_road = roads.world_to_frenet(e.x, e.y, f, e.z);
    double ground_z = 0.0;
    if (on_road) {
        WorldPos wp;
        if (roads.frenet_to_world(f.road_id, 0, f.s, f.offset, wp)) {
            ground_z = wp.z;
        } else {
            on_road = false;
        }
    }

    if (!on_road) {
        // 不在道路上 → 无支撑，自由下落
        e.z += e.vz * dt;
        if (e.z < -200.0) e.z = -200.0;   // 防无限下坠
        return;
    }

    if (e.z <= ground_z) {
        // 贴地：道路支撑，垂直速度归零
        e.z = ground_z;
        e.vz = 0.0;
    } else {
        // 高于路面 → 重力下落；落到路面则支撑
        e.z += e.vz * dt;
        if (e.z <= ground_z) {
            e.z = ground_z;
            e.vz = 0.0;
        }
    }
}

}  // namespace flowsim
