#include "kun/strategy/dual_ma_strategy.hpp"
#include <iomanip>
#include <sstream>

namespace kun {

void DualMaStrategy::on_init() {
    KUN_LOG_INFO(strategy_name_, "DualMaStrategy initialized for symbol: " + symbol_ + 
                 " [Fast: " + std::to_string(engine_.fast_period()) + ", Slow: " + std::to_string(engine_.slow_period()) + "]");
}

void DualMaStrategy::on_bar(const BarData& bar) {
    if (bar.symbol != symbol_) return;

    auto long_pos = get_position(symbol_, Direction::LONG);
    auto short_pos = get_position(symbol_, Direction::SHORT);

    int pos_state = 0;
    if (long_pos.volume > 0) pos_state = 1;
    else if (short_pos.volume > 0) pos_state = -1;

    // 复用纯同构信号算子计算金叉/死叉指令
    auto signals = engine_.update_price(bar.close_price, pos_state);

    for (const auto& sig : signals) {
        std::stringstream ss;
        ss << sig.reason << " at price " << bar.close_price << " [Fast MA: "
           << std::fixed << std::setprecision(2) << sig.fast_ma << ", Slow MA: " << sig.slow_ma << "]";
        KUN_LOG_INFO(strategy_name_, ss.str());

        switch (sig.type) {
            case SignalType::BUY_CLOSE:
                if (short_pos.volume > 0) cover(symbol_, bar.close_price, short_pos.volume);
                break;
            case SignalType::BUY_OPEN:
                buy(symbol_, bar.close_price, trade_volume_);
                break;
            case SignalType::SELL_CLOSE:
                if (long_pos.volume > 0) sell(symbol_, bar.close_price, long_pos.volume);
                break;
            case SignalType::SELL_OPEN:
                short_sell(symbol_, bar.close_price, trade_volume_);
                break;
            default:
                break;
        }
    }
}

void DualMaStrategy::on_order(const OrderData& order) {
    // 处理撤单、拒单逻辑
}

void DualMaStrategy::on_trade(const TradeData& trade) {
    std::stringstream ss;
    ss << "Trade executed: " << to_string(trade.direction) << " " << to_string(trade.offset)
       << " " << trade.symbol << " Vol: " << trade.volume << " @ Price: " << trade.price
       << " Comm: " << trade.commission;
    KUN_LOG_INFO(strategy_name_, ss.str());
}

} // namespace kun
