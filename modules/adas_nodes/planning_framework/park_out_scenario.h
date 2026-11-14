#pragma once

#include "scenario.h"
#include "stage.h"
#include "valet_parking_scenario.h"
#include "open_space/hybrid_a_star.h"

namespace flowengine::planning {

/**
 * @brief Stage 1: Park-Out Maneuver Stage (Hybrid A* exit from parking bay to aisle)
 */
class ParkOutManeuverStage : public Stage {
public:
    ParkOutManeuverStage() = default;

    std::string_view Name() const override { return "PARK_OUT_MANEUVER"; }
    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;
    bool IsFinished(const FrameContext& context) const override;
    void Reset() override;

private:
    bool trajectory_latched_{false};
    Trajectory latched_trajectory_{};
    HybridAStar planner_{};

    bool PlanTrajectory(FrameContext& context);
};

/**
 * @brief Stage 2: Park-Out Cruise Handoff Stage (accelerate smoothly onto aisle centerline)
 */
class ParkOutCruiseStage : public Stage {
public:
    ParkOutCruiseStage() = default;

    std::string_view Name() const override { return "PARK_OUT_CRUISE"; }
    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;
    bool IsFinished(const FrameContext& context) const override;
    void Reset() override;

private:
    int settled_frames_{0};
};

/**
 * @brief Scenario for Open-Space Park-Out (Apollo ParkAndGo-aligned)
 *
 * Plans collision-free Hybrid A* exit trajectory from a parked garage slot to
 * the adjacent driving aisle, then hands back to LaneFollow / Frenet cruise.
 */
class ParkOutScenario : public Scenario {
public:
    static constexpr int kPriority = 55; // Above ValetParking (50) and LaneFollow (10), below EmergencyStop (100)

    ParkOutScenario();

    ScenarioType Type() const override { return ScenarioType::PARK_OUT; }
    std::string_view Name() const override { return "PARK_OUT"; }
    int Priority() const override { return kPriority; }

    bool IsApplicable(const FrameContext& context) const override;
    ScenarioStatus Run(FrameContext& context, Trajectory& trajectory) override;
    void Reset() override;

    std::string_view CurrentStageName() const noexcept;
    bool IsActive() const noexcept { return is_active_ && !finished_; }
    void ClearCompleted() noexcept { completed_once_ = false; }
    void RearmIfAtStartSlot(const FrameContext& context) noexcept;

private:
    ParkOutManeuverStage maneuver_stage_{};
    ParkOutCruiseStage cruise_stage_{};
    bool is_active_{false};
    bool completed_once_{false};
};

} // namespace flowengine::planning
