#include "kun/strategy/dual_ma_strategy.hpp"
#include <iomanip>
#include <sstream>

namespace kun {

void DualMaStrategy::on_init() {
    KUN_LOG_INFO(strategy_name_, "DualMaStrategy initialized for symbol: " + symbol_ + 
                 " [Fast: " + std::to_string(fast_period_) + ", Slow: " + std::to_string(slow_period_) + "]");
}

double DualMaStrategy::calculate_ma(int period) const {
    if (static_cast<int>(close_history_.size()) < period) return 0.0;
    double sum = std::accumulate(close_history_.end() - period, close_history_.end(), 0.0);
    return sum / period;
}

void DualMaStrategy::on_bar(const BarData& bar) {
    if (bar.symbol != symbol_) return;

    close_history_.push_back(bar.close_price);
    if (static_cast<int>(close_history_.size()) > slow_period_ + 10) {
        close_history_.pop_front();
    }

    if (static_cast<int>(close_history_.size()) < slow_period_) {
        return;
    }

    double fast_ma = calculate_ma(fast_period_);
    double slow_ma = calculate_ma(slow_period_);

    auto long_pos = get_position(symbol_, Direction::LONG);
    auto short_pos = get_position(symbol_, Direction::SHORT);

    // 金叉判断 (Fast 上穿 Slow)
    if (last_fast_ma_ <= last_slow_ma_ && fast_ma > slow_ma) {
        std::stringstream ss;
        ss << "Golden Cross detected! Fast MA: " << std::fixed << std::setprecision(2) << fast_ma 
           << " > Slow MA: " << slow_ma << " at price " << bar.close_price;
        KUN_LOG_INFO(strategy_name_, ss.str());

        // 如果持有空仓，先平空
        if (short_pos.volume > 0) {
            cover(symbol_, bar.close_price, short_pos.volume);
        }
        // 开多仓
        if (long_pos.volume == 0) {
            buy(symbol_, bar.close_price, trade_volume_);
        }
    }
    // 死叉判断 (Fast 下穿 Slow)
    else if (last_fast_ma_ >= last_slow_ma_ && fast_ma < slow_ma) {
        std::stringstream ss;
        ss << "Death Cross detected! Fast MA: " << std::fixed << std::setprecision(2) << fast_ma 
           << " < Slow MA: " << slow_ma << " at price " << bar.close_price;
        KUN_LOG_INFO(strategy_name_, ss.str());

        // 如果持有多仓，先平多
        if (long_pos.volume > 0) {
            sell(symbol_, bar.close_price, long_pos.volume);
        }
        // 开空仓
        if (short_pos.volume == 0) {
            short_sell(symbol_, bar.close_price, trade_volume_);
        }
    }

    last_fast_ma_ = fast_ma;
    last_slow_ma_ = slow_ma;
}

void DualMaStrategy::on_order(const OrderData& order) {
    // 可以在此处理撤单、拒单重新发单逻辑
}

void DualMaStrategy::on_trade(const TradeData& trade) {
    std::stringstream ss;
    ss << "Trade executed: " << to_string(trade.direction) << " " << to_string(trade.offset)
       << " " << trade.symbol << " Vol: " << trade.volume << " @ Price: " << trade.price
       << " Comm: " << trade.commission;
    KUN_LOG_INFO(strategy_name_, ss.str());
}

} // namespace kun
