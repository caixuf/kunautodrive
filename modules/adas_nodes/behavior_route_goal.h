#pragma once

/**
 * behavior_route_goal.h — 导航路线目标车道 → 行为层相邻变道（纯逻辑）
 *
 * 背景：route_step lane_change 以前只在 planning 里改写 target_lane_offset，
 * behavior 仍停在 CRUISE/LEFT_CHANGE，出现「车向右靠、仪表盘显示左变道」
 * 的状态不一致。这里把「路线要求第 N 条车道」翻译成 behavior 的相邻一步
 * 变道意图：每次只移动到 current±1，完成后再由下一帧继续，避免一次跨 3
 * 条车道的横向阶跃。
 *
 * 语义：
 *   - lane 索引 0 = 最左（行进方向参考系），N-1 = 最右；
 *   - 双向路只接受本向半幅 [N/2, N-1]，单向路 [0, N-1]；
 *   - on_return（掉头返程）/泊车场景不接管；
 *   - 已经在目标车道、目标越界 → 不触发。
 */

#include <algorithm>

namespace behavior_route_goal {

struct Decision {
    int  lane{-1};       /**< 下一跳目标车道；-1 = 不触发 */
    bool trigger{false}; /**< 是否应发起一次相邻变道 */
};

inline int first_legal_lane(int lane_count, bool oneway) {
    if (lane_count <= 0) return 0;
    return oneway ? 0 : lane_count / 2;
}

inline Decision next_lane(int current_lane, int goal_lane, int lane_count,
                          bool oneway, bool on_return, bool parking) {
    Decision d;
    if (goal_lane < 0 || lane_count <= 0 || current_lane < 0) return d;
    if (on_return || parking) return d;

    const int first = first_legal_lane(lane_count, oneway);
    const int last = lane_count - 1;
    if (goal_lane < first || goal_lane > last) return d;
    if (current_lane < first || current_lane > last) return d;
    if (current_lane == goal_lane) return d;

    const int next = (current_lane < goal_lane) ? current_lane + 1
                                                : current_lane - 1;
    if (next < first || next > last) return d;
    d.lane = next;
    d.trigger = true;
    return d;
}

}  // namespace behavior_route_goal
