#pragma once

#include "scenario.h"
#include "stage.h"
#include "task.h"

namespace flowengine::planning {

/**
 * @brief Decider task for Emergency Stop and L3 Minimum Risk Maneuver (MRM).
 * Determines target deceleration, stopping distance, and hazard signals.
 */
class EmergencyStopDeciderTask : public Task {
public:
    EmergencyStopDeciderTask() = default;

    std::string_view Name() const override { return "EmergencyStopDeciderTask"; }

    TaskStatus Execute(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override {}

    double hard_brake_decel{-5.0};    // m/s^2 (severe emergency)
    double mrm_brake_decel{-2.5};     // m/s^2 (controlled L3 MRM in-lane stop)
    double default_brake_decel{-3.5}; // m/s^2 (standard emergency stop)
};

/**
 * @brief Trajectory generation task for Emergency Stop.
 * Generates an in-lane stopping profile bringing speed down to 0 at specified decel rate.
 */
class EmergencyStopTrajectoryTask : public Task {
public:
    EmergencyStopTrajectoryTask() = default;

    std::string_view Name() const override { return "EmergencyStopTrajectoryTask"; }

    TaskStatus Execute(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override {}
};

/**
 * @brief Stage managing Emergency Stop tasks and checking vehicle standstill.
 */
class EmergencyStopStage : public Stage {
public:
    EmergencyStopStage();

    std::string_view Name() const override { return "EmergencyStopStage"; }

    StageStatus Process(FrameContext& context, Trajectory& trajectory) override;

    bool IsFinished(const FrameContext& context) const override;

    EmergencyStopDeciderTask& DeciderTask() noexcept { return decider_task_; }
    EmergencyStopTrajectoryTask& TrajectoryTask() noexcept { return trajectory_task_; }

private:
    EmergencyStopDeciderTask decider_task_{};
    EmergencyStopTrajectoryTask trajectory_task_{};
};

/**
 * @brief Scenario for Emergency Stop and L3 MRM (high-priority safety preemption).
 */
class EmergencyStopScenario : public Scenario {
public:
    static constexpr int kPriority = 100; // Preempts normal driving scenarios immediately

    EmergencyStopScenario();

    ScenarioType Type() const override { return ScenarioType::EMERGENCY_STOP; }
    std::string_view Name() const override { return "EmergencyStopScenario"; }
    int Priority() const override { return kPriority; }

    bool IsApplicable(const FrameContext& context) const override;

    ScenarioStatus Run(FrameContext& context, Trajectory& trajectory) override;

    void Reset() override;

    EmergencyStopStage& GetEmergencyStopStage() noexcept { return emergency_stop_stage_; }

private:
    EmergencyStopStage emergency_stop_stage_{};
};

} // namespace flowengine::planning
