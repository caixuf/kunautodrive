#include "kun/strategy/bollinger_strategy.hpp"
#include <iomanip>
#include <sstream>

namespace kun {

void BollingerStrategy::on_init() {
    KUN_LOG_INFO(strategy_name_, "BollingerStrategy initialized for symbol: " + symbol_ + 
                 " [Window: " + std::to_string(window_) + ", Dev: " + std::to_string(dev_multiplier_) + "]");
}

void BollingerStrategy::on_bar(const BarData& bar) {
    if (bar.symbol != symbol_) return;

    close_history_.push_back(bar.close_price);
    if (static_cast<int>(close_history_.size()) > window_ + 10) {
        close_history_.pop_front();
    }

    if (static_cast<int>(close_history_.size()) < window_) {
        return;
    }

    // 计算均值
    double sum = std::accumulate(close_history_.end() - window_, close_history_.end(), 0.0);
    double mean = sum / window_;

    // 计算标准差
    double sq_sum = 0.0;
    for (auto it = close_history_.end() - window_; it != close_history_.end(); ++it) {
        sq_sum += (*it - mean) * (*it - mean);
    }
    double std_dev = std::sqrt(sq_sum / window_);

    double upper_band = mean + dev_multiplier_ * std_dev;
    double lower_band = mean - dev_multiplier_ * std_dev;

    auto long_pos = get_position(symbol_, Direction::LONG);
    auto short_pos = get_position(symbol_, Direction::SHORT);

    // 向上突破上轨 -> 做多
    if (bar.close_price > upper_band) {
        if (short_pos.volume > 0) {
            cover(symbol_, bar.close_price, short_pos.volume);
        }
        if (long_pos.volume == 0) {
            buy(symbol_, bar.close_price, trade_volume_);
        }
    }
    // 向下突破下轨 -> 做空
    else if (bar.close_price < lower_band) {
        if (long_pos.volume > 0) {
            sell(symbol_, bar.close_price, long_pos.volume);
        }
        if (short_pos.volume == 0) {
            short_sell(symbol_, bar.close_price, trade_volume_);
        }
    }
}

void BollingerStrategy::on_trade(const TradeData& trade) {
    std::stringstream ss;
    ss << "Bollinger Trade: " << to_string(trade.direction) << " " << to_string(trade.offset)
       << " " << trade.symbol << " Vol: " << trade.volume << " @ " << trade.price;
    KUN_LOG_INFO(strategy_name_, ss.str());
}

} // namespace kun
