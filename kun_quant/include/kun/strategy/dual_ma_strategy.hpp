#pragma once

#include "kun/strategy/strategy_base.hpp"
#include <deque>
#include <numeric>
#include <vector>
#include <string>

namespace kun {

enum class SignalType {
    NONE = 0,
    BUY_OPEN,    // 买开多
    SELL_CLOSE,  // 卖平多
    SELL_OPEN,   // 卖开空
    BUY_CLOSE    // 买平空
};

struct StrategySignal {
    SignalType type{SignalType::NONE};
    double price{0.0};
    double fast_ma{0.0};
    double slow_ma{0.0};
    std::string reason;
};

/**
 * @brief 策略核心计算算子 (Strategy Core Homomorphism)
 * 纯状态机与数学算子：回测离线驱动与实盘 FlowCoro 协程驱动 100% 同构复用
 */
class DualMaSignalEngine {
public:
    DualMaSignalEngine(int fast_period = 5, int slow_period = 20)
        : fast_period_(fast_period), slow_period_(slow_period) {}

    void reset() {
        close_history_.clear();
        last_fast_ma_ = 0.0;
        last_slow_ma_ = 0.0;
        is_initialized_ = false;
    }

    std::vector<StrategySignal> update_price(double price, int current_pos_state) {
        // current_pos_state: 1 = 多头, -1 = 空头, 0 = 无持仓
        std::vector<StrategySignal> signals;
        close_history_.push_back(price);
        if (static_cast<int>(close_history_.size()) > slow_period_ + 20) {
            close_history_.pop_front();
        }

        if (static_cast<int>(close_history_.size()) < slow_period_) {
            return signals; // 暖机中
        }

        double fast_ma = calculate_ma(fast_period_);
        double slow_ma = calculate_ma(slow_period_);

        if (!is_initialized_) {
            last_fast_ma_ = fast_ma;
            last_slow_ma_ = slow_ma;
            is_initialized_ = true;
            return signals;
        }

        // 金叉: Fast 上穿 Slow
        if (last_fast_ma_ <= last_slow_ma_ && fast_ma > slow_ma) {
            if (current_pos_state < 0) {
                signals.push_back({SignalType::BUY_CLOSE, price, fast_ma, slow_ma, "Golden Cross - Cover Short"});
            }
            if (current_pos_state <= 0) {
                signals.push_back({SignalType::BUY_OPEN, price, fast_ma, slow_ma, "Golden Cross - Open Long"});
            }
        }
        // 死叉: Fast 下穿 Slow
        else if (last_fast_ma_ >= last_slow_ma_ && fast_ma < slow_ma) {
            if (current_pos_state > 0) {
                signals.push_back({SignalType::SELL_CLOSE, price, fast_ma, slow_ma, "Death Cross - Sell Long"});
            }
            if (current_pos_state >= 0) {
                signals.push_back({SignalType::SELL_OPEN, price, fast_ma, slow_ma, "Death Cross - Open Short"});
            }
        }

        last_fast_ma_ = fast_ma;
        last_slow_ma_ = slow_ma;
        return signals;
    }

    double calculate_ma(int period) const {
        if (static_cast<int>(close_history_.size()) < period) return 0.0;
        double sum = std::accumulate(close_history_.end() - period, close_history_.end(), 0.0);
        return sum / period;
    }

    int fast_period() const { return fast_period_; }
    int slow_period() const { return slow_period_; }

private:
    int fast_period_{5};
    int slow_period_{20};
    std::deque<double> close_history_;
    double last_fast_ma_{0.0};
    double last_slow_ma_{0.0};
    bool is_initialized_{false};
};

/**
 * @brief 经典双均线趋势跟踪策略 (Dual Moving Average CTA Strategy)
 */
class DualMaStrategy : public IStrategy {
public:
    DualMaStrategy(std::string name, std::string symbol, int fast_period = 5, int slow_period = 20, double trade_volume = 1.0)
        : IStrategy(std::move(name)),
          symbol_(std::move(symbol)),
          trade_volume_(trade_volume),
          engine_(fast_period, slow_period) {}

    void on_init() override;
    void on_bar(const BarData& bar) override;
    void on_order(const OrderData& order) override;
    void on_trade(const TradeData& trade) override;

private:
    std::string symbol_;
    double trade_volume_{1.0};
    DualMaSignalEngine engine_;
};

} // namespace kun
