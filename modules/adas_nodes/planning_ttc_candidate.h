/**
 * planning_ttc_candidate.h — TTC follow lead 候选选择（纯算法，可单测）
 *
 * D2-06 阶段：从 `planning_node.cpp` 内联的 TTC follow 候选循环抽出，理由与
 * `fusion_lane_hint.h` / `perception_points.h` 相同：planning_node 是 C++20 协程
 * 节点，纯算法无法被 CI 覆盖，靠测试副本会漂移。抽成 header-only 后
 * `tests/test_adas_nodes_logic.c` 直接编同一份实现，无副本漂移。
 *
 * Spec 契约（REQ_L3_DIR2_HDMAP.md §4.2 FR-RT-05 + D2-06）：
 *   - 输入：obs 数组（每条带 fusion 给的 `lane_match_hint: bool`），
 *           ego 位姿/朝向/车道宽度
 *   - 输出：候选 obs 索引（>=0）；无候选 → -1
 *   - 选 candidate 语义：
 *       * require_hint=true  → 仅 `hint=true` 的 obs 入候选（fusion 已
 *         验证"obs 在 ego 同车道或相邻 ±1"，是 planning 跟车首选）
 *       * require_hint=false → 所有 obs 都入候选（向后兼容 / fallback）
 *       * 沿车头投影距离 ∈ (along_min, along_max] → 入候选
 *       * 横向投影 |lat − lat_ref| ≤ lat_max → 入候选（lat_ref = 应跟随车道
 *         中心在车体系的横向坐标；变道中取目标车道，避免误跟"离开车道"的邻车）
 *       * 沿车头相对速度 rel_v ≥ rel_v_min → 入候选（过滤对向来车）
 *       * 沿车头距离最小者优先（closest in s）
 *
 *   与原 `planning_node.cpp` 内联版本的语义差异：
 *     1. 新增 hint 过滤：require_hint=true 时只挑 fusion 验证过同/邻车道的
 *        obs，不让跨车道、对向车、远侧车的 obs 误入选
 *     2. lat_max 由 caller 传入（hint=true 时用 0.75×lane_w 与原版一致；
 *        hint=false fallback 时 caller 可传更严的 0.5×lane_w）
 *     3. 其余参数与原版 TTC follow 循环逐项等价
 *
 *   不做：
 *     - 不修改任何入参（纯函数）
 *     - 不做 mutex / 全局状态（无副作用）
 *     - 不下结论：返回 -1 不代表"无前车"，仅代表"无候选"（caller 仍可走 truth 兜底）
 */

#ifndef PLANNING_TTC_CANDIDATE_H
#define PLANNING_TTC_CANDIDATE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TTC follow 候选的 obs 视图（仅取规划消费所需字段，避免耦合 adas_msgs_gen）。*/
typedef struct {
    double x;                /* 世界 x (m) */
    double y;                /* 世界 y (m) */
    double vx;               /* 世界 vx (m/s) */
    double vy;               /* 世界 vy (m/s) */
    bool   lane_match_hint;  /* fusion 给的 obs_lane_match_hint (D2-07 引入) */
    /* 到**应跟随车道中心线**的横向距离 (m，左为正)。由 caller 用 Frenet 投影
     * 算好传入（`obs_d − follow_lane_center_d`），使本函数的横向判据与 ego
     * 瞬时位置解耦 —— 变道途中 ego 悬在两车道之间时，以 ego 为零点会把
     * "离开车道"的邻车误判为跟车目标（D2-07 / r157_cut_out 刹停死锁）。
     * caller 无法投影时应退化传车体系横向距离（`−rx·sin h + ry·cos h`）。*/
    double lat_m;
} TtcObsView;

/**
 * 从 obs 数组里挑出"最像 ego 同/邻车道前车"的 1 个候选。
 *
 * @param obs            输入 obs 数组（NULL + count>0 返回 -1）
 * @param count          有效 obs 数量（必须 <= 数组长度；<=0 返回 -1）
 * @param ego_x          ego 世界 x (m)
 * @param ego_y          ego 世界 y (m)
 * @param ego_heading    ego 世界朝向（弧度，0 朝 +x，π 朝 -x）
 * @param lane_w         车道宽度（m，<=0 返回 -1，无候选）
 * @param along_min_m    沿车头最小距离（m，默认 0.0 即车头正前方；>0 留 hysteresis）
 * @param along_max_m    沿车头最大距离（m，默认 80.0；>0 必须）
 * @param rel_v_min_mps  沿车头最小相对速度（m/s；-2.0 表示允许接近中的对向车入选）
 * @param lat_max_m      横向门限（m），超过此值的 obs 不入选
 *                       - hint=true 路径：建议 0.5*lane_w + 0.6（与 behavior_planner
 *                         的 lead_lat_tol 同式同值，两模块对同一目标结论不翻转）
 *                       - hint=false 兜底：建议 0.5*lane_w（更严，因未验证同车道）
 * @param require_hint   true 必须 obs.lane_match_hint=true；false 不查 hint
 * @return               候选 obs 索引（>=0）；无候选 -1
 *
 * 横向门限用 `obs[i].lat_m`（obs 到**应跟随车道中心线**的横向距离，caller 用
 * Frenet 算好）。D2-07 修复：旧版以 **ego 瞬时位置**为零点
 * （lat = −rx·sin h + ry·cos h，rx/ry 相对 ego）。变道途中 ego_y 悬在两车道
 * 之间，零点随之漂移，"离开车道"的邻道车会被误判为跟车目标：实测 r157_cut_out
 * 中真值障碍（y=−1.75，源车道）lat=3.17 恰在门外，而 DBSCAN 只聚到近面、
 * 感知质心偏向可见侧 1.0m 后 lat=2.18 落进门内 → gap≈3m → command_speed 刹到
 * 0 且永不恢复（永久卡死）。改为对车道中心线求距后判据与 ego 位置解耦：
 * 邻道车（真值/感知两种表示）恒被拒，目标道前车（含 ±1m 感知质心偏差）恒被收。
 * Python 先行验证见 tools/ttc_lat_gate_sim.py（--run-all PASS）；参考系与
 * behavior_planner_node.cpp 的 `lat = |obs_projection.d − current_lane_d|` 一致。
 */
static inline int ttc_select_follow_lead(
        const TtcObsView* obs, int count,
        double ego_x, double ego_y, double ego_heading,
        double lane_w,
        double along_min_m, double along_max_m,
        double rel_v_min_mps, double lat_max_m,
        bool   require_hint) {
    if (!obs || count <= 0 || lane_w <= 0.0) return -1;
    if (along_max_m <= along_min_m) return -1;
    if (lat_max_m <= 0.0) return -1;

    const double fwd_x = cos(ego_heading);
    const double fwd_y = sin(ego_heading);
    int best_idx = -1;
    double best_along = 1e18;

    for (int i = 0; i < count; i++) {
        const TtcObsView* o = &obs[i];
        if (require_hint && !o->lane_match_hint) continue;

        const double rx = o->x - ego_x;
        const double ry = o->y - ego_y;
        const double along = rx * fwd_x + ry * fwd_y;
        if (along <= along_min_m || along > along_max_m) continue;

        /* 沿车头相对速度（obs 接近 ego 的速率；正=远离，负=接近）*/
        const double rel_v = o->vx * fwd_x + o->vy * fwd_y;
        if (rel_v < rel_v_min_mps) continue;

        /* 横向门限：obs 到**应跟随车道中心线**的横向距离（caller 用 Frenet 算好
         * 放进 o->lat_m）。判据与 ego 瞬时位置解耦 —— 旧版以 ego 为零点，
         * 变道途中零点漂移会把"离开车道"的邻车误判为跟车目标
         * （r157_cut_out 刹停死锁，见函数头 D2-07 说明）。 */
        if (fabs(o->lat_m) > lat_max_m) continue;

        if (along < best_along) {
            best_along = along;
            best_idx = i;
        }
    }
    return best_idx;
}

#ifdef __cplusplus
}
#endif

#endif /* PLANNING_TTC_CANDIDATE_H */
