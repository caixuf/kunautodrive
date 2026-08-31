#pragma once

// ============================================================================
// EvolvableTask — 形态发生细胞引擎的通用任务接口 (gym 风格)
//
// 设计目标: 让细胞演化引擎不再绑定任何业务 (量化/智驾只是两个插件),
// 任何 "观测 -> 离散动作 -> 奖励" 的序贯决策问题都可以作为一个 Task 挂载。
// 引擎是底座, 任务是插件 —— 适应力来自演化, 通用性来自接口。
// ============================================================================

#include <vector>
#include <string>
#include <cstdint>

namespace kun {

class EvolvableTask {
public:
    struct StepResult {
        std::vector<float> obs;   // 下一观测 (维度 = obs_dim)
        double reward{0.0};       // 即时奖励
        bool done{false};         // 回合终止 (到达目标 / 超时 / 失败)
        bool success{false};      // 是否达成目标 (区别于超时终止)
    };

    virtual ~EvolvableTask() = default;

    virtual const char* name() const = 0;

    // 观测维度 — 当前细胞有机体固定 4 通道输入, 任务需压缩到 obs_dim() <= 4
    virtual size_t obs_dim() const = 0;

    // 动作空间大小 (离散动作: 0 .. act_dim()-1)
    virtual size_t act_dim() const = 0;

    // 回合重置。episode_seed 用于任务内部随机化 (多布局泛化等)
    virtual void reset(uint32_t episode_seed) = 0;

    // 单步推演: 引擎把有机体输出映射为离散动作后交给任务
    virtual StepResult step(int action) = 0;

    // 回合结束后的适应度 (默认 = 累计奖励)
    virtual double current_fitness() const = 0;
};

} // namespace kun
