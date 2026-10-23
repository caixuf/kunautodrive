#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include "stage.h"

namespace flowengine::planning {

/**
 * @brief Identifiers for built-in and extended scenarios.
 */
enum class ScenarioType {
    UNKNOWN        = 0,
    LANE_FOLLOW    = 1,
    EMERGENCY_STOP = 2,
    INTERSECTION   = 3,
    PARKING        = 4,
};

/**
 * @brief Status returned by Scenario::Run().
 */
enum class ScenarioStatus {
    RUNNING  = 0,
    FINISHED = 1,
    ERROR    = 2
};

/**
 * @brief Pure virtual base class Scenario.
 *
 * A Scenario coordinates high-level behavior for specific driving contexts (e.g.,
 * cruising/following on a highway, emergency braking, navigating an intersection).
 * It manages multiple Stages and determines applicability via IsApplicable().
 */
class Scenario {
public:
    static constexpr size_t kMaxStagesPerScenario = 8;

    virtual ~Scenario() = default;

    /**
     * @brief Type identifier for the scenario.
     */
    virtual ScenarioType Type() const = 0;

    /**
     * @brief Human-readable name of the scenario.
     */
    virtual std::string_view Name() const = 0;

    /**
     * @brief Scenario arbitration priority. Higher number indicates higher priority.
     * E.g. EmergencyStop = 100, LaneFollow = 10.
     */
    virtual int Priority() const = 0;

    /**
     * @brief Check if this scenario is applicable for the current frame context.
     */
    virtual bool IsApplicable(const FrameContext& context) const = 0;

    /**
     * @brief Execute scenario pipeline for current frame.
     */
    virtual ScenarioStatus Run(FrameContext& context, Trajectory& trajectory) = 0;

    /**
     * @brief Reset scenario state and reset all stages.
     */
    virtual void Reset() = 0;

    /**
     * @brief Check whether scenario execution has finished.
     */
    virtual bool IsFinished() const noexcept {
        return finished_;
    }

    /**
     * @brief Add a Stage to the scenario sequence (zero heap allocation).
     */
    bool AddStage(Stage* stage) noexcept {
        if (!stage || stage_count_ >= kMaxStagesPerScenario) {
            return false;
        }
        stages_[stage_count_++] = stage;
        return true;
    }

    Stage* CurrentStage() const noexcept {
        if (current_stage_idx_ < stage_count_) {
            return stages_[current_stage_idx_];
        }
        return nullptr;
    }

    size_t StageCount() const noexcept { return stage_count_; }
    size_t CurrentStageIndex() const noexcept { return current_stage_idx_; }

protected:
    std::array<Stage*, kMaxStagesPerScenario> stages_{};
    size_t stage_count_{0};
    size_t current_stage_idx_{0};
    bool finished_{false};
};

} // namespace flowengine::planning
