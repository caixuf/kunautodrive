#pragma once

#include "scenario.h"
#include "stage.h"
#include "task.h"

namespace flowengine::planning {

/**
 * @brief Decider task for Lane Follow: determines cruise vs. lead vehicle follow.
 * Uses Constant Time Gap (CTG) headway policy and writes decisions to Blackboard.
 */
class LaneFollowDeciderTask : public Task {
public:
    LaneFollowDeciderTask() = default;

    std::string_view Name() const override { return "LaneFollowDeciderTask"; }

    TaskStatus Execute(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override {}

    // CTG & Cruise parameters
    double acc_standoff{5.0};             // Standstill safety distance (m)
    double acc_time_headway{1.5};         // Time headway (s)
    double acc_k_gap{0.4};                // Gap error gain (1/s)
    double acc_gap_err_clamp{8.0};        // Max speed delta from gap error (m/s)
    double default_cruise_speed{15.0};    // Default cruising speed (m/s)
    double max_follow_distance{80.0};     // Max search range for lead vehicle (m)
};

/**
 * @brief Trajectory generation task for Lane Follow.
 * Reads target speed and follow decisions from Blackboard and generates
 * a smooth 64-point trajectory.
 */
class LaneFollowTrajectoryTask : public Task {
public:
    LaneFollowTrajectoryTask() = default;

    std::string_view Name() const override { return "LaneFollowTrajectoryTask"; }

    TaskStatus Execute(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override {}
};

/**
 * @brief Stage for Lane Follow execution, managing decider and trajectory tasks.
 */
class LaneFollowStage : public Stage {
public:
    LaneFollowStage();

    std::string_view Name() const override { return "LaneFollowStage"; }

    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;

    bool IsFinished(const FrameContext& context) const override;

    LaneFollowDeciderTask& DeciderTask() noexcept { return decider_task_; }
    LaneFollowTrajectoryTask& TrajectoryTask() noexcept { return trajectory_task_; }

private:
    LaneFollowDeciderTask decider_task_{};
    LaneFollowTrajectoryTask trajectory_task_{};
};

/**
 * @brief Scenario for Lane Follow (cruising and car following).
 */
class LaneFollowScenario : public Scenario {
public:
    static constexpr int kPriority = 10;

    LaneFollowScenario();

    ScenarioType Type() const override { return ScenarioType::LANE_FOLLOW; }
    std::string_view Name() const override { return "LaneFollowScenario"; }
    int Priority() const override { return kPriority; }

    bool IsApplicable(const FrameContext& context) const override;

    ScenarioStatus Run(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override;

    LaneFollowStage& GetLaneFollowStage() noexcept { return lane_follow_stage_; }

private:
    LaneFollowStage lane_follow_stage_{};
};

} // namespace flowengine::planning
