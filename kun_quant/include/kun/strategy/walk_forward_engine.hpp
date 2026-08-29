#pragma once

#include "kun/core/types.hpp"
#include "kun/backtest/backtest_engine.hpp"
#include "kun/strategy/dual_ma_strategy.hpp"
#include <string>
#include <vector>
#include <cmath>

namespace kun {

struct WalkForwardSliceResult {
    int slice_index{0};
    double is_sharpe{0.0};
    double is_pnl{0.0};
    double oos_sharpe{0.0};
    double oos_pnl{0.0};
    int best_fast_window{5};
    int best_slow_window{20};
};

struct WalkForwardReport {
    double walk_forward_efficiency{0.0}; // WFE = mean(OOS) / mean(IS)
    double overall_oos_pnl{0.0};
    bool is_robust{false};
    bool is_parameter_plateau{false};
    std::vector<WalkForwardSliceResult> slices;

    std::string summary() const;
};

/**
 * @brief 滚动样本外前向优化与参数平原验证引擎 (WalkForwardEngine)
 * 拒绝在全样本上过拟合单一神奇参数，采用滚动 IS/OOS 切片与参数平原检验
 */
class WalkForwardEngine {
public:
    static WalkForwardReport run_walk_forward(
        const std::vector<BarData>& all_bars,
        int num_slices = 5,
        double is_ratio = 0.70
    );

    /**
     * @brief 参数平原检验 (Plateau Robustness Check)
     * 检验最优参数邻域 (如 fast+-1, slow+-2) 的绩效方差，防止单点尖刺过拟合
     */
    static bool check_parameter_plateau(
        const std::vector<BarData>& bars,
        int optimal_fast,
        int optimal_slow
    );
};

} // namespace kun
