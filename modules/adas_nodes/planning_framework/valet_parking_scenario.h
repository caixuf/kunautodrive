#pragma once

#include <vector>
#include <string_view>
#include <cmath>
#include "scenario.h"
#include "stage.h"
#include "task.h"
#include "hybrid_a_star.h"

namespace flowengine::planning {

/**
 * @brief Stage 1: Approach Stage
 * Controls vehicle along road/lane while decelerating towards parking trigger zone.
 */
class ApproachStage : public Stage {
public:
    ApproachStage();
    std::string_view Name() const override { return "APPROACH"; }
    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;
    bool IsFinished(const FrameContext& context) const override;
    void Reset() override;

private:
    double approach_distance_threshold_{18.0}; // m
    double approach_speed_threshold_{3.5};    // m/s
};

/**
 * @brief Stage 2: Open Space Cruise Stage
 * Executes Hybrid A* obstacle-free path planning from current vehicle pose to parking slot,
 * downsamples to <=64 points with gear switch cusps preserved, and latches the trajectory.
 */
class OpenSpaceCruiseStage : public Stage {
public:
    OpenSpaceCruiseStage();
    std::string_view Name() const override { return "OPEN_SPACE_CRUISE"; }
    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;
    bool IsFinished(const FrameContext& context) const override;
    void Reset() override;

    bool IsLatched() const noexcept { return trajectory_latched_; }
    const Trajectory& LatchedTrajectory() const noexcept { return latched_trajectory_; }

    static int DownsampleTrajectory(const std::vector<PathPoint>& raw_path,
                                    Trajectory& trajectory,
                                    double wheelbase);

private:
    bool trajectory_latched_{false};
    Trajectory latched_trajectory_{};
    HybridAStar planner_{};

    bool PlanTrajectory(FrameContext& context);
};

/**
 * @brief Stage 3: Precision Hold Stage
 * Holds vehicle at zero velocity inside parking slot.
 */
class PrecisionHoldStage : public Stage {
public:
    PrecisionHoldStage();
    std::string_view Name() const override { return "PRECISION_HOLD"; }
    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;
    bool IsFinished(const FrameContext& context) const override;
    void Reset() override;
};

/**
 * @brief Scenario for Valet Parking (Milestone M3: Approach -> Open Space Hybrid A* -> Precision Hold)
 */
class ValetParkingScenario : public Scenario {
public:
    static constexpr int kPriority = 50; // Higher than LaneFollow (10), lower than EmergencyStop (100)

    ValetParkingScenario();

    ScenarioType Type() const override { return ScenarioType::VALET_PARKING; }
    std::string_view Name() const override { return "VALET_PARKING"; }
    int Priority() const override { return kPriority; }

    bool IsApplicable(const FrameContext& context) const override;
    ScenarioStatus Run(FrameContext& context, Trajectory& trajectory) override;
    void Reset() override;

    std::string_view CurrentStageName() const noexcept;
    bool IsActive() const noexcept { return is_active_; }

private:
    ApproachStage approach_stage_{};
    OpenSpaceCruiseStage open_space_cruise_stage_{};
    PrecisionHoldStage precision_hold_stage_{};
    bool is_active_{false};
};

} // namespace flowengine::planning
