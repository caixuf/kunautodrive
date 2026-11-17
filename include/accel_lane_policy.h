#pragma once

namespace flowengine {

/** 高架入口加速车道。behavior 与单测共用，不要在节点里再写一份。 */
struct AccelLaneInput {
    int lane_count{0};
    bool oneway{false};
    bool on_return{false};
    int current_idx{0};
    double speed_mps{0.0};
    double cruise_mps{0.0};          /* 当前这条路生效的巡航，不是管线回退 15 */
    bool inner_lane_empty{false};
    double inner_lead_mps{0.0};
    bool carriageway_stop_ahead{false};
    bool cooldown_active{false};
};

struct AccelLaneDecision {
    bool active{false};
    bool holding{false};
    bool merge_inward{false};
    int target_lane{-1};
};

inline int accel_outer_lane(int lane_count, bool on_return) {
    if (lane_count <= 0) return 0;
    return on_return ? 0 : lane_count - 1;
}

/**
 * 双向至少 8 车道：
 * 低于生效巡航的 85% 时，停在或每次一条地靠向最外侧加速车道，不向内并。
 * 达到 85% 后，内侧空着或前车不低于巡航的 70% 才向内一步；慢车不并。
 */
inline AccelLaneDecision decide_accel_lane(const AccelLaneInput& in) {
    AccelLaneDecision d;
    if (in.oneway || in.lane_count < 8 || in.cruise_mps <= 0.5) return d;
    const int legal_first = in.lane_count / 2;
    const bool on_this_side = in.on_return ? (in.current_idx < legal_first)
                                           : (in.current_idx >= legal_first);
    if (!on_this_side) return d;
    if (in.current_idx < 0 || in.current_idx >= in.lane_count) return d;
    d.active = true;
    const int outer = accel_outer_lane(in.lane_count, in.on_return);
    if (in.speed_mps < in.cruise_mps * 0.85) {
        d.holding = true;
        int toward = outer;
        if (in.current_idx < outer) toward = in.current_idx + 1;
        else if (in.current_idx > outer) toward = in.current_idx - 1;
        d.target_lane = toward;
        return d;
    }
    if (in.carriageway_stop_ahead || in.cooldown_active) return d;
    const bool gap_ok = in.inner_lane_empty ||
                        (in.inner_lead_mps >= in.cruise_mps * 0.70);
    if (!gap_ok) return d;
    const int inward = in.on_return ? in.current_idx + 1 : in.current_idx - 1;
    if (in.on_return) {
        if (inward >= legal_first || inward >= in.lane_count) return d;
    } else if (inward < legal_first || inward < 0) {
        return d;
    }
    d.merge_inward = true;
    d.target_lane = inward;
    return d;
}

}  // namespace flowengine
