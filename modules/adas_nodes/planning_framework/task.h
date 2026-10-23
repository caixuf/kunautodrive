#pragma once

#include <string_view>
#include "planning_context.h"
#include "adas_msgs_gen.h"

namespace flowengine::planning {

/**
 * @brief Status returned by a Task after execution.
 */
enum class TaskStatus {
    SUCCESS = 0,
    FAILED  = 1,
    SKIPPED = 2,
    RUNNING = 3
};

/**
 * @brief Pure virtual base class Task.
 *
 * Each Task encapsulates a single discrete planning computation (decider, speed planner,
 * path optimizer, collision checker, etc.). Tasks read and write to the FrameContext
 * (including its Blackboard) and the Trajectory without heap allocation.
 */
class Task {
public:
    virtual ~Task() = default;

    /**
     * @brief Human-readable name of the task.
     */
    virtual std::string_view Name() const = 0;

    /**
     * @brief Execute the task algorithm.
     * @param context In-out frame context (vehicle state, road geometry, blackboard, etc.)
     * @param trajectory In-out trajectory being planned.
     * @return TaskStatus
     */
    virtual TaskStatus Execute(FrameContext& context, Trajectory& trajectory) = 0;

    /**
     * @brief Reset task internal state between scenarios or stages.
     */
    virtual void Reset() {}
};

} // namespace flowengine::planning
