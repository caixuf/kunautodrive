#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include "task.h"

namespace flowengine::planning {

/**
 * @brief Stage execution status.
 */
enum class StageStatus {
    RUNNING  = 0,
    FINISHED = 1,
    ERROR    = 2
};

/**
 * @brief Pure virtual base class Stage.
 *
 * A Stage represents a concrete phase within a Scenario. It manages an ordered
 * sequence of Tasks, executes them per cycle via Process(), and tracks whether
 * the stage condition has been satisfied via IsFinished().
 */
class Stage {
public:
    static constexpr size_t kMaxTasksPerStage = 16;

    virtual ~Stage() = default;

    /**
     * @brief Human-readable name of the stage.
     */
    virtual std::string_view Name() const = 0;

    /**
     * @brief Process stage execution for current frame.
     * Executes internal tasks and evaluates completion.
     */
    virtual StageStatus Process(FrameContext& context, Trajectory& trajectory) = 0;

    /**
     * @brief Check whether stage has completed its execution for current context.
     */
    virtual bool IsFinished(const FrameContext& context) const = 0;

    /**
     * @brief Cached stage completion query.
     */
    virtual bool IsFinished() const noexcept {
        return finished_;
    }

    /**
     * @brief Reset stage and its internal tasks.
     */
    virtual void Reset() {
        finished_ = false;
        for (size_t i = 0; i < task_count_; ++i) {
            if (tasks_[i] != nullptr) {
                tasks_[i]->Reset();
            }
        }
    }

    /**
     * @brief Register a Task into the stage pipeline (zero heap allocation).
     */
    bool AddTask(Task* task) noexcept {
        if (!task || task_count_ >= kMaxTasksPerStage) {
            return false;
        }
        tasks_[task_count_++] = task;
        return true;
    }

    size_t TaskCount() const noexcept { return task_count_; }

    Task* GetTask(size_t index) const noexcept {
        return (index < task_count_) ? tasks_[index] : nullptr;
    }

protected:
    std::array<Task*, kMaxTasksPerStage> tasks_{};
    size_t task_count_{0};
    bool finished_{false};
};

} // namespace flowengine::planning
