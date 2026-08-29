#pragma once

#include "kun/strategy/strategy_base.hpp"
#include <deque>
#include <numeric>
#include <cmath>

namespace kun {

/**
 * @brief 经典布林通道突破策略 (Bollinger Bands Breakout Strategy)
 */
class BollingerStrategy : public IStrategy {
public:
    BollingerStrategy(std::string name, std::string symbol, int window = 20, double dev_multiplier = 2.0, double trade_volume = 1.0)
        : IStrategy(std::move(name)),
          symbol_(std::move(symbol)),
          window_(window),
          dev_multiplier_(dev_multiplier),
          trade_volume_(trade_volume) {}

    void on_init() override;
    void on_bar(const BarData& bar) override;
    void on_trade(const TradeData& trade) override;

private:
    std::string symbol_;
    int window_{20};
    double dev_multiplier_{2.0};
    double trade_volume_{1.0};

    std::deque<double> close_history_;
};

} // namespace kun
