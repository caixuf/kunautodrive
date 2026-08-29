#pragma once

#include "kun/strategy/strategy_base.hpp"
#include <deque>
#include <numeric>

namespace kun {

/**
 * @brief 经典双均线趋势跟踪策略 (Dual Moving Average CTA Strategy)
 */
class DualMaStrategy : public IStrategy {
public:
    DualMaStrategy(std::string name, std::string symbol, int fast_period = 5, int slow_period = 20, double trade_volume = 1.0)
        : IStrategy(std::move(name)),
          symbol_(std::move(symbol)),
          fast_period_(fast_period),
          slow_period_(slow_period),
          trade_volume_(trade_volume) {}

    void on_init() override;
    void on_bar(const BarData& bar) override;
    void on_order(const OrderData& order) override;
    void on_trade(const TradeData& trade) override;

private:
    double calculate_ma(int period) const;

    std::string symbol_;
    int fast_period_{5};
    int slow_period_{20};
    double trade_volume_{1.0};

    std::deque<double> close_history_;
    double last_fast_ma_{0.0};
    double last_slow_ma_{0.0};
};

} // namespace kun
