#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "scenario.h"

namespace flowengine::planning {

/**
 * @brief Scenario Manager and Arbitrator with Hysteresis and Anti-Chatter mechanisms.
 *
 * Implements priority-based scenario selection, zero-delay emergency preemption,
 * and multi-frame debounce to prevent chattering between scenarios due to sensor noise.
 */
class ScenarioManager {
public:
    static constexpr size_t kMaxScenarios = 16;
    static constexpr uint32_t kDefaultDebounceFrames = 3;
    static constexpr int kDefaultEmergencyPriorityThreshold = 100;

    ScenarioManager() = default;

    /**
     * @brief Register a Scenario into the manager (zero heap allocation).
     * @param scenario Pointer to statically or member-allocated Scenario instance.
     * @return true if registered successfully.
     */
    bool RegisterScenario(Scenario* scenario) noexcept;

    /**
     * @brief Main per-frame update tick.
     * Evaluates applicable scenarios, performs arbitration with hysteresis/debounce,
     * and runs the active scenario.
     * @param context In-out frame context.
     * @param trajectory In-out output trajectory.
     * @return ScenarioStatus
     */
    ScenarioStatus Update(FrameContext& context, Trajectory& trajectory);

    /**
     * @brief Evaluate registered scenarios and select the highest priority applicable candidate.
     */
    Scenario* SelectScenario(const FrameContext& context) const noexcept;

    /**
     * @brief Reset manager state and all registered scenarios.
     */
    void Reset() noexcept;

    // Getters and configuration
    Scenario* CurrentScenario() const noexcept { return current_scenario_; }
    Scenario* PendingScenario() const noexcept { return pending_scenario_; }
    uint32_t PendingFrameCount() const noexcept { return pending_frame_count_; }
    uint32_t DebounceThreshold() const noexcept { return debounce_threshold_frames_; }
    void SetDebounceThreshold(uint32_t frames) noexcept { debounce_threshold_frames_ = frames; }

    int EmergencyPriorityThreshold() const noexcept { return emergency_priority_threshold_; }
    void SetEmergencyPriorityThreshold(int priority) noexcept { emergency_priority_threshold_ = priority; }

    size_t ScenarioCount() const noexcept { return scenario_count_; }
    Scenario* GetScenario(size_t index) const noexcept {
        return (index < scenario_count_) ? scenarios_[index] : nullptr;
    }

private:
    void SwitchToScenario(Scenario* target) noexcept;

    std::array<Scenario*, kMaxScenarios> scenarios_{};
    size_t scenario_count_{0};

    Scenario* current_scenario_{nullptr};
    Scenario* pending_scenario_{nullptr};
    uint32_t pending_frame_count_{0};

    uint32_t debounce_threshold_frames_{kDefaultDebounceFrames};
    int emergency_priority_threshold_{kDefaultEmergencyPriorityThreshold};
};

} // namespace flowengine::planning
