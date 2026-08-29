#pragma once

#include "kun/core/types.hpp"
#include "kun/engine/position_manager.hpp"
#include "kun/strategy/strategy_base.hpp"
#include <string>
#include <vector>
#include <memory>

namespace kun {

struct ShadowAccountMetrics {
    std::string candidate_id;
    double initial_capital{1000000.0};
    double current_equity{1000000.0};
    double total_return_pct{0.0};
    double max_drawdown_pct{0.0};
    double max_daily_drawdown_pct{0.0};
    double win_rate{0.0};
    double profit_factor{0.0};
    int total_trades{0};
    int running_days{0};
    int regime_shifts_observed{0};
    bool is_eligible_for_promotion{false};
    bool require_human_confirmation{true};
    std::string eligibility_reason;
};

/**
 * @brief 影子账户复合准入门禁管理器 (ShadowAccountRunner)
 * 核心安全防线：
 * 1. 时长门槛：考核周期默认扩展至 20 交易日
 * 2. 形态门槛：必须经历并存活至少 1 次市场形态剧变 (Regime Shift)
 * 3. 严控单日最大回撤 (< 2.5%)
 * 4. 强制保留人机终审权限 (Human-in-the-Loop)
 */
class ShadowAccountRunner {
public:
    ShadowAccountRunner(
        std::string candidate_id,
        std::shared_ptr<IStrategy> strategy,
        double initial_capital = 1000000.0,
        int min_eval_days = 20,
        int required_regime_shifts = 1,
        double max_daily_drawdown_limit = 0.025,
        double switch_sharpe_threshold = 1.2
    );

    void on_tick(const QuantTickMsg& tick);
    void on_bar(const BarData& bar);

    // 记录经历了一次市场形态变迁 (如 趋势 -> 震荡)
    void record_regime_shift(const std::string& from_regime, const std::string& to_regime);

    // 推进虚拟交易日
    void advance_trading_day();

    ShadowAccountMetrics get_metrics() const;
    bool should_hot_switch(double current_live_sharpe) const;

    const std::string& candidate_id() const { return candidate_id_; }

private:
    std::string candidate_id_;
    std::shared_ptr<IStrategy> strategy_;
    double initial_capital_{1000000.0};
    double current_balance_{1000000.0};
    double peak_equity_{1000000.0};
    double max_drawdown_{0.0};
    double max_daily_drawdown_{0.0};
    double day_start_equity_{1000000.0};

    int min_eval_days_{20};
    int required_regime_shifts_{1};
    double max_daily_drawdown_limit_{0.025};
    double switch_sharpe_threshold_{1.2};

    int running_days_{0};
    int regime_shifts_observed_{0};
    bool breached_daily_drawdown_{false};

    PositionManager position_mgr_;
    std::vector<TradeData> virtual_trades_;
    int eval_ticks_count_{0};
};

} // namespace kun
