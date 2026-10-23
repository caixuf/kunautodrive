#include "scenario_manager.h"

namespace flowengine::planning {

bool ScenarioManager::RegisterScenario(Scenario* scenario) noexcept {
    if (!scenario || scenario_count_ >= kMaxScenarios) {
        return false;
    }
    // Prevent duplicate registration
    for (size_t i = 0; i < scenario_count_; ++i) {
        if (scenarios_[i] == scenario) {
            return false;
        }
    }
    scenarios_[scenario_count_++] = scenario;
    return true;
}

Scenario* ScenarioManager::SelectScenario(const FrameContext& context) const noexcept {
    Scenario* best = nullptr;
    int best_priority = -1;

    for (size_t i = 0; i < scenario_count_; ++i) {
        Scenario* s = scenarios_[i];
        if (s != nullptr && s->IsApplicable(context)) {
            int p = s->Priority();
            if (p > best_priority) {
                best_priority = p;
                best = s;
            }
        }
    }
    return best;
}

void ScenarioManager::SwitchToScenario(Scenario* target) noexcept {
    if (current_scenario_ != nullptr) {
        current_scenario_->Reset();
    }
    current_scenario_ = target;
    if (current_scenario_ != nullptr) {
        current_scenario_->Reset();
    }
    pending_scenario_ = nullptr;
    pending_frame_count_ = 0;
}

ScenarioStatus ScenarioManager::Update(FrameContext& context, Trajectory& trajectory) {
    Scenario* candidate = SelectScenario(context);

    if (current_scenario_ == nullptr) {
        // Initial scenario selection
        if (candidate != nullptr) {
            SwitchToScenario(candidate);
        }
    } else {
        if (candidate == current_scenario_) {
            // Active scenario remains the top candidate: cancel any pending transition
            pending_scenario_ = nullptr;
            pending_frame_count_ = 0;
        } else if (candidate != nullptr) {
            // A different scenario wants to take control.
            // Rule 1: Emergency preemption (immediate transition, 0-frame debounce).
            // Rule 2: If the current scenario is already finished, switch immediately.
            bool is_emergency = (candidate->Priority() >= emergency_priority_threshold_);
            bool current_finished = current_scenario_->IsFinished();

            if (is_emergency || current_finished) {
                SwitchToScenario(candidate);
            } else {
                // Rule 3: Normal transition requires multi-frame debounce (hysteresis).
                if (candidate == pending_scenario_) {
                    pending_frame_count_++;
                    if (pending_frame_count_ >= debounce_threshold_frames_) {
                        SwitchToScenario(pending_scenario_);
                    }
                } else {
                    pending_scenario_ = candidate;
                    pending_frame_count_ = 1;
                    if (debounce_threshold_frames_ <= 1) {
                        SwitchToScenario(pending_scenario_);
                    }
                }
            }
        } else {
            // candidate is null (no scenario applicable)
            if (current_scenario_->IsFinished()) {
                SwitchToScenario(nullptr);
            }
        }
    }

    if (current_scenario_ != nullptr) {
        return current_scenario_->Run(context, trajectory);
    }

    // Fallback: no scenario active
    trajectory.seq = context.seq;
    trajectory.stamp_us = context.timestamp_us;
    trajectory.point_count = 0;
    trajectory.valid = 0;
    trajectory.planner_state = 0;
    return ScenarioStatus::ERROR;
}

void ScenarioManager::Reset() noexcept {
    for (size_t i = 0; i < scenario_count_; ++i) {
        if (scenarios_[i] != nullptr) {
            scenarios_[i]->Reset();
        }
    }
    current_scenario_ = nullptr;
    pending_scenario_ = nullptr;
    pending_frame_count_ = 0;
}

} // namespace flowengine::planning
