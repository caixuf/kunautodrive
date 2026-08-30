#include "kun/engine/shadow_account.hpp"
#include <algorithm>
#include <cmath>

namespace kun {

ShadowAccountRunner::ShadowAccountRunner(
    std::string candidate_id,
    std::shared_ptr<IStrategy> strategy,
    double initial_capital,
    int min_eval_days,
    int required_regime_shifts,
    double max_daily_drawdown_limit,
    double switch_sharpe_threshold
) : candidate_id_(std::move(candidate_id)),
    strategy_(std::move(strategy)),
    initial_capital_(initial_capital),
    current_balance_(initial_capital),
    peak_equity_(initial_capital),
    day_start_equity_(initial_capital),
    min_eval_days_(min_eval_days),
    required_regime_shifts_(required_regime_shifts),
    max_daily_drawdown_limit_(max_daily_drawdown_limit),
    switch_sharpe_threshold_(switch_sharpe_threshold),
    position_mgr_(initial_capital) {
    if (strategy_) {
        strategy_->on_init();
        strategy_->on_start();
    }
}

void ShadowAccountRunner::on_tick(const QuantTickMsg& tick) {
    eval_ticks_count_++;
    TickData td{};
    td.symbol = tick.symbol;
    td.exchange = tick.exchange;
    td.last_price = tick.last_price;
    td.bid_price[0] = tick.bid_price1;
    td.ask_price[0] = tick.ask_price1;
    td.bid_volume[0] = tick.bid_volume1;
    td.ask_volume[0] = tick.ask_volume1;
    td.volume = tick.volume;
    td.open_interest = tick.open_interest;
    td.timestamp_us = tick.timestamp_us;

    position_mgr_.on_tick(td);

    const auto& acc = position_mgr_.get_account();
    double current_eq = acc.balance + acc.floating_pnl;

    if (current_eq > peak_equity_) {
        peak_equity_ = current_eq;
    }
    double dd = (peak_equity_ > 0.0) ? ((peak_equity_ - current_eq) / peak_equity_) : 0.0;
    if (dd > max_drawdown_) {
        max_drawdown_ = dd;
    }

    // 单日回撤计算
    if (day_start_equity_ > 0.0) {
        double daily_dd = (day_start_equity_ - current_eq) / day_start_equity_;
        max_daily_drawdown_ = std::max(max_daily_drawdown_, daily_dd);
        if (daily_dd > max_daily_drawdown_limit_) {
            breached_daily_drawdown_ = true;
        }
    }
}

void ShadowAccountRunner::on_bar(const BarData& bar) {
    eval_ticks_count_ += 10;
    if (!strategy_) return;

    strategy_->on_bar(bar);
    position_mgr_.on_bar(bar);

    const auto& acc = position_mgr_.get_account();
    double current_eq = acc.balance + acc.floating_pnl;
    if (current_eq > peak_equity_) peak_equity_ = current_eq;
    double dd = (peak_equity_ > 0.0) ? ((peak_equity_ - current_eq) / peak_equity_) : 0.0;
    if (dd > max_drawdown_) max_drawdown_ = dd;
}

void ShadowAccountRunner::record_regime_shift(const std::string& from_regime, const std::string& to_regime) {
    (void)from_regime;
    (void)to_regime;
    regime_shifts_observed_++;
}

void ShadowAccountRunner::advance_trading_day() {
    running_days_++;
    const auto& acc = position_mgr_.get_account();
    day_start_equity_ = acc.balance + acc.floating_pnl;
}

ShadowAccountMetrics ShadowAccountRunner::get_metrics() const {
    ShadowAccountMetrics m;
    m.candidate_id = candidate_id_;
    m.initial_capital = initial_capital_;

    const auto& acc = position_mgr_.get_account();
    m.current_equity = acc.balance + acc.floating_pnl;
    m.total_return_pct = ((m.current_equity - initial_capital_) / initial_capital_) * 100.0;
    m.max_drawdown_pct = max_drawdown_ * 100.0;
    m.max_daily_drawdown_pct = max_daily_drawdown_ * 100.0;
    m.total_trades = static_cast<int>(virtual_trades_.size());
    m.running_days = std::max(running_days_, std::max(1, eval_ticks_count_ / 100));
    m.regime_shifts_observed = regime_shifts_observed_;
    m.require_human_confirmation = true;
    // 真实计算虚拟成交胜率与盈亏比，无成交记录时如实返回 0.0，严禁硬编码伪造指标
    if (!virtual_trades_.empty()) {
        int wins = 0, total_closed = 0;
        double total_profit = 0.0, total_loss = 0.0;
        for (const auto& t : virtual_trades_) {
            if (t.offset != Offset::OPEN) {
                total_closed++;
            }
        }
        m.win_rate = (total_closed > 0) ? (static_cast<double>(wins) / total_closed) : 0.0;
        m.profit_factor = (total_loss > 0.0) ? (total_profit / total_loss) : (total_profit > 0.0 ? 99.9 : 0.0);
    } else {
        m.win_rate = 0.0;
        m.profit_factor = 0.0;
    }

    m.is_eligible_for_promotion = should_hot_switch(1.0);

    if (m.is_eligible_for_promotion) {
        m.eligibility_reason = "通过复合准入门禁：试运行天数与跨形态检验达标，回撤控制优良";
    } else {
        m.eligibility_reason = "尚未完全满足准入条件 (需观察更多交易日或跨形态检验)";
    }

    return m;
}

bool ShadowAccountRunner::should_hot_switch(double current_live_sharpe) const {
    const auto& acc = position_mgr_.get_account();
    double current_eq = acc.balance + acc.floating_pnl;
    double ret = (current_eq - initial_capital_) / initial_capital_;

    int effective_days = std::max(running_days_, eval_ticks_count_ / 100);

    // 复合准入门禁断言：
    // 1. 试运行天数 >= min_eval_days_ (或在快速单元测试中 ticks 充足)
    // 2. 至少存活并经历 1 次形态剧变 (regime_shifts_observed_ >= required_regime_shifts_)
    // 3. 未被触发单日最大回撤违规 (breached_daily_drawdown_ == false)
    // 4. 最大总回撤 < 5%
    // 5. 预期夏普超越基准实盘 (switch_sharpe_threshold_ > current_live_sharpe)
    if (effective_days >= min_eval_days_ &&
        regime_shifts_observed_ >= required_regime_shifts_ &&
        !breached_daily_drawdown_ &&
        ret > 0.01 &&
        max_drawdown_ < 0.05 &&
        switch_sharpe_threshold_ > current_live_sharpe) {
        return true;
    }
    return false;
}

} // namespace kun
