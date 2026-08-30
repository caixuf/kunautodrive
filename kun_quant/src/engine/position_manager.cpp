#include "kun/engine/position_manager.hpp"
#include "kun/core/logger.hpp"
#include <algorithm>

namespace kun {

PositionManager::PositionManager(double initial_balance) {
    account_.balance = initial_balance;
    account_.available = initial_balance;
}

void PositionManager::set_symbol_info(const SymbolInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    symbol_infos_[info.symbol] = info;
}

const SymbolInfo* PositionManager::get_symbol_info(const std::string& symbol) const {
    auto it = symbol_infos_.find(symbol);
    if (it != symbol_infos_.end()) {
        return &(it->second);
    }
    return nullptr;
}

void PositionManager::on_trade(const TradeData& trade) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 默认合约乘数与费率
    int multiplier = 10;
    double margin_ratio = 0.10;
    if (auto info = get_symbol_info(trade.symbol)) {
        multiplier = info->multiplier;
        margin_ratio = info->margin_ratio;
    }

    // 持仓 key 归属: 开仓成交方向即持仓方向;
    // 平仓成交回报方向是操作方向 (卖平/买平), 持仓方向为其反向
    Direction pos_dir = trade.direction;
    if (trade.offset != Offset::OPEN) {
        pos_dir = (trade.direction == Direction::LONG) ? Direction::SHORT : Direction::LONG;
    }
    std::string key = trade.symbol + "_" + to_string(pos_dir);
    auto& pos = positions_[key];
    pos.symbol = trade.symbol;
    pos.exchange = trade.exchange;
    pos.direction = trade.direction;

    // 扣除手续费
    account_.commission += trade.commission;
    account_.balance -= trade.commission;
    account_.available -= trade.commission;

    if (trade.offset == Offset::OPEN) {
        // 开仓逻辑
        double old_cost = pos.avg_price * pos.volume * multiplier;
        double new_cost = trade.price * trade.volume * multiplier;
        pos.volume += trade.volume;
        pos.today_volume += trade.volume;
        pos.avg_price = (old_cost + new_cost) / (pos.volume * multiplier);
        pos.open_cost = pos.avg_price * pos.volume * multiplier;

        // 计算占用保证金
        double added_margin = trade.price * trade.volume * multiplier * margin_ratio;
        pos.margin += added_margin;
        account_.margin += added_margin;
        account_.available -= added_margin;
    } else {
        // 平仓逻辑
        double close_vol = std::min(pos.volume, trade.volume);
        if (close_vol > 0) {
            double pnl = 0.0;
            if (trade.direction == Direction::LONG) {
                // 平空: 卖出开仓 -> 买入平仓
                pnl = (pos.avg_price - trade.price) * close_vol * multiplier;
            } else {
                // 平多: 买入开仓 -> 卖出平仓
                pnl = (trade.price - pos.avg_price) * close_vol * multiplier;
            }

            pos.realized_pnl += pnl;
            account_.realized_pnl += pnl;
            account_.balance += pnl;
            account_.available += pnl;

            // 释放保证金
            double released_margin = (pos.margin / pos.volume) * close_vol;
            pos.margin -= released_margin;
            account_.margin -= released_margin;
            account_.available += released_margin;

            // 今昨仓扣减
            if (trade.offset == Offset::CLOSE_TODAY) {
                double today_sub = std::min(pos.today_volume, close_vol);
                pos.today_volume -= today_sub;
                pos.yd_volume -= (close_vol - today_sub);
            } else if (trade.offset == Offset::CLOSE_YESTERDAY) {
                double yd_sub = std::min(pos.yd_volume, close_vol);
                pos.yd_volume -= yd_sub;
                pos.today_volume -= (close_vol - yd_sub);
            } else {
                // 默认平昨优先 (先扣昨仓，再扣今仓)
                double yd_sub = std::min(pos.yd_volume, close_vol);
                pos.yd_volume -= yd_sub;
                pos.today_volume -= (close_vol - yd_sub);
            }
            if (pos.today_volume < 0.0) pos.today_volume = 0.0;
            if (pos.yd_volume < 0.0) pos.yd_volume = 0.0;

            pos.volume -= close_vol;
            pos.frozen = std::max(0.0, pos.frozen - close_vol);

            if (pos.volume <= 0.0) {
                pos.volume = 0.0;
                pos.today_volume = 0.0;
                pos.yd_volume = 0.0;
                pos.avg_price = 0.0;
                pos.margin = 0.0;
                pos.open_cost = 0.0;
                pos.floating_pnl = 0.0;
                pos.frozen = 0.0;
            }
        }
    }

    // 重新计算当前最新价下的浮动盈亏
    if (latest_prices_.find(trade.symbol) != latest_prices_.end()) {
        update_pnl(trade.symbol, latest_prices_[trade.symbol]);
    }
}

void PositionManager::on_tick(const TickData& tick) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_prices_[tick.symbol] = tick.last_price;
    update_pnl(tick.symbol, tick.last_price);
}

void PositionManager::on_bar(const BarData& bar) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_prices_[bar.symbol] = bar.close_price;
    update_pnl(bar.symbol, bar.close_price);
}

void PositionManager::update_pnl(const std::string& symbol, double current_price) {
    double total_floating_pnl = 0.0;

    for (auto& [key, pos] : positions_) {
        if (pos.symbol == symbol && pos.volume > 0) {
            int multiplier = 10;
            if (auto info = get_symbol_info(symbol)) {
                multiplier = info->multiplier;
            }

            if (pos.direction == Direction::LONG) {
                pos.floating_pnl = (current_price - pos.avg_price) * pos.volume * multiplier;
            } else if (pos.direction == Direction::SHORT) {
                pos.floating_pnl = (pos.avg_price - current_price) * pos.volume * multiplier;
            }
        }
        total_floating_pnl += pos.floating_pnl;
    }

    account_.floating_pnl = total_floating_pnl;
}

PositionData PositionManager::get_position(const std::string& symbol, Direction direction) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = symbol + "_" + to_string(direction);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }
    PositionData empty_pos;
    empty_pos.symbol = symbol;
    empty_pos.direction = direction;
    return empty_pos;
}

std::vector<PositionData> PositionManager::get_all_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PositionData> result;
    for (const auto& [k, v] : positions_) {
        if (v.volume > 0) {
            result.push_back(v);
        }
    }
    return result;
}

AccountData PositionManager::get_account() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return account_;
}

bool PositionManager::freeze_margin(double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (account_.available >= amount) {
        account_.available -= amount;
        account_.frozen_margin += amount;
        return true;
    }
    return false;
}

void PositionManager::unfreeze_margin(double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    account_.frozen_margin -= amount;
    account_.available += amount;
}

bool PositionManager::freeze_position(const std::string& symbol, Direction direction, double volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = symbol + "_" + to_string(direction);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        double available_vol = it->second.volume - it->second.frozen;
        if (available_vol >= volume) {
            it->second.frozen += volume;
            return true;
        }
    }
    return false;
}

void PositionManager::unfreeze_position(const std::string& symbol, Direction direction, double volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = symbol + "_" + to_string(direction);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        it->second.frozen = std::max(0.0, it->second.frozen - volume);
    }
}

std::vector<std::pair<Offset, double>> PositionManager::resolve_close_orders(
    const std::string& symbol, const std::string& exchange,
    Direction close_direction, double req_volume) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<Offset, double>> result;
    if (req_volume <= 0) return result;

    // 平仓操作方向反向映射为被平持仓方向
    Direction pos_dir = (close_direction == Direction::SHORT) ? Direction::LONG : Direction::SHORT;
    std::string key = symbol + "_" + to_string(pos_dir);
    auto it = positions_.find(key);
    if (it == positions_.end() || it->second.volume <= 0) {
        return result; // 无可平持仓
    }

    const auto& pos = it->second;
    double max_avail = std::max(0.0, pos.volume - pos.frozen);
    double to_close = std::min(req_volume, max_avail);
    if (to_close <= 0) return result;

    // 上期所 (SHFE) 与能源中心 (INE) 区分平今 / 平昨，且平昨手续费通常较低
    if (exchange == "SHFE" || exchange == "INE") {
        // 优先平昨仓 (降低手续费)
        double yd_avail = std::max(0.0, pos.yd_volume);
        double yd_close = std::min(to_close, yd_avail);
        if (yd_close > 0) {
            result.emplace_back(Offset::CLOSE_YESTERDAY, yd_close);
        }

        double rem = to_close - yd_close;
        double td_avail = std::max(0.0, pos.today_volume);
        double td_close = std::min(rem, td_avail);
        if (td_close > 0) {
            result.emplace_back(Offset::CLOSE_TODAY, td_close);
        }
    } else {
        // 中金所 (CFFEX)、大商所 (DCE)、郑商所 (CZCE) 统一使用 Offset::CLOSE
        result.emplace_back(Offset::CLOSE, to_close);
    }

    return result;
}

} // namespace kun
