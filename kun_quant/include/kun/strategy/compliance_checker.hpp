#pragma once

#include "kun/core/types.hpp"
#include "kun/backtest/backtest_engine.hpp"
#include "kun/strategy/strategy_base.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cmath>

namespace kun {

struct ComplianceCheckResult {
    bool passed{true};
    bool zero_cost_violation{false};
    bool missing_stop_loss_violation{false};
    bool future_leakage_detected{false};
    bool price_out_of_bounds{false};
    double normal_sharpe{0.0};
    double shifted_sharpe{0.0};
    std::vector<std::string> violation_messages;

    std::string summary() const;
};

/**
 * @brief §2-G 量化策略防作弊硬约束与合规检测器 (StrategyComplianceChecker)
 * 
 * 关卡 2：引擎断言机试 (零成本拒绝、无止损拒绝、越界价格校验)
 * 关卡 3：数据平移测试 (未来函数探测器 Data-Shift Tester)
 */
class StrategyComplianceChecker {
public:
    /**
     * @brief 执行三道硬约束合规性与未来函数检测
     * @param factory 策略工厂函数
     * @param history_bars 完整历史 K 线序列
     * @param slippage 滑点
     * @param commission_ratio 手续费率
     * @param stop_loss_pct 止损比例 (必须 > 0)
     */
    static ComplianceCheckResult verify_strategy(
        std::function<std::unique_ptr<IStrategy>()> factory,
        const std::vector<BarData>& history_bars,
        double slippage = 1.0,
        double commission_ratio = 0.0001,
        double stop_loss_pct = 0.02
    );

    /**
     * @brief 数据平移未来函数探测器 (v2: 未来扰动 + 决策日志比对)
     * 强突变最后一根 K 线后重跑回测, 若 bar 0..N-2 上的策略决策日志发生任何变化,
     * 说明策略决策依赖了未来数据 (如偷窥 bars[i+k]), 直接判作弊。
     */
    static bool detect_future_leakage(
        std::function<std::unique_ptr<IStrategy>()> factory,
        const std::vector<BarData>& history_bars,
        double& out_normal_sharpe,
        double& out_shifted_sharpe
    );
};

} // namespace kun
