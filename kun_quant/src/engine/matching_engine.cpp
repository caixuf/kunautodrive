#include "kun/engine/matching_engine.hpp"
#include "kun/core/logger.hpp"
#include <algorithm>
#include <cmath>

namespace kun {

MatchingEngine::MatchingEngine(double slippage_points, double commission_ratio)
    : slippage_points_(slippage_points), commission_ratio_(commission_ratio) {}

void MatchingEngine::set_callbacks(OrderCallback on_order, TradeCallback on_trade) {
    on_order_cb_ = std::move(on_order);
    on_trade_cb_ = std::move(on_trade);
}

uint64_t MatchingEngine::submit_order(const OrderRequest& req) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t order_id = next_order_id_.fetch_add(1);

    OrderData order;
    order.order_id = order_id;
    order.order_ref = req.order_ref;
    order.symbol = req.symbol;
    order.exchange = req.exchange;
    order.direction = req.direction;
    order.offset = req.offset;
    order.order_type = req.order_type;
    order.price = req.price;
    order.total_volume = req.volume;
    order.traded_volume = 0.0;
    order.status = OrderStatus::ACCEPTED; // 模拟直接挂入撮合队列
    order.strategy_name = req.strategy_name;

    active_orders_.push_back(order);
    all_orders_.push_back(order);

    if (on_order_cb_) {
        on_order_cb_(order);
    }
    return order_id;
}

bool MatchingEngine::cancel_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = active_orders_.begin(); it != active_orders_.end(); ++it) {
        if (it->order_id == order_id) {
            it->status = OrderStatus::CANCELED;
            OrderData canceled_order = *it;
            active_orders_.erase(it);

            // 更新 all_orders
            for (auto& o : all_orders_) {
                if (o.order_id == order_id) {
                    o.status = OrderStatus::CANCELED;
                    break;
                }
            }

            if (on_order_cb_) {
                on_order_cb_(canceled_order);
            }
            return true;
        }
    }
    return false;
}

double MatchingEngine::calculate_market_impact(double volume, double available_depth, double base_price) const {
    if (impact_coeff_ <= 0.0 || volume <= 0.0) return 0.0;
    double depth = std::max(1.0, available_depth);
    // 平方根市场冲击模型: Delta_P = gamma * base_price * sqrt(volume / depth)
    double impact = impact_coeff_ * base_price * std::sqrt(volume / depth);
    return impact;
}

void MatchingEngine::match_tick(const TickData& tick) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_orders_.empty()) return;

    std::vector<OrderData> remaining;
    for (auto& order : active_orders_) {
        if (order.symbol != tick.symbol) {
            remaining.push_back(order);
            continue;
        }

        bool can_fill = false;
        double fill_price = tick.last_price;
        double available_depth = 100.0;

        if (order.direction == Direction::LONG) {
            available_depth = tick.ask_volume[0] > 0.0 ? tick.ask_volume[0] : 50.0;
        } else {
            available_depth = tick.bid_volume[0] > 0.0 ? tick.bid_volume[0] : 50.0;
        }

        if (order.order_type == OrderType::MARKET) {
            can_fill = true;
            fill_price = (order.direction == Direction::LONG) ? 
                (tick.ask_price[0] > 0 ? tick.ask_price[0] : tick.last_price) :
                (tick.bid_price[0] > 0 ? tick.bid_price[0] : tick.last_price);
        } else if (order.order_type == OrderType::LIMIT || order.order_type == OrderType::FAK || order.order_type == OrderType::FOK) {
            if (order.direction == Direction::LONG) {
                double target_ask = tick.ask_price[0] > 0 ? tick.ask_price[0] : tick.last_price;
                if (order.price >= target_ask) {
                    can_fill = true;
                    fill_price = order.price;
                }
            } else {
                double target_bid = tick.bid_price[0] > 0 ? tick.bid_price[0] : tick.last_price;
                if (order.price <= target_bid) {
                    can_fill = true;
                    fill_price = order.price;
                }
            }
        }

        double unfulfilled = order.total_volume - order.traded_volume;

        if (can_fill) {
            // FOK 检查：若盘口深度不足以全量成交，立即全单撤销
            if (order.order_type == OrderType::FOK) {
                if (available_depth < unfulfilled) {
                    order.status = OrderStatus::CANCELED;
                    if (on_order_cb_) on_order_cb_(order);
                    continue;
                }
            }

            // 深度约束下的单次成交量 (实现真实部分成交)
            double fill_vol = std::min(unfulfilled, available_depth);
            if (fill_vol <= 0.0) {
                remaining.push_back(order);
                continue;
            }

            // 加入真实固定滑点 + 动态市场冲击成本 (Market Impact)
            double impact = calculate_market_impact(fill_vol, available_depth, fill_price);
            if (order.direction == Direction::LONG) {
                fill_price += (slippage_points_ + impact);
            } else {
                fill_price -= (slippage_points_ + impact);
            }

            execute_fill(order, fill_price, fill_vol, tick.timestamp_us, tick.datetime_str);

            if (order.order_type == OrderType::FAK) {
                // FAK: 部分成交后未满足部分自动撤销
                if (order.traded_volume < order.total_volume) {
                    order.status = OrderStatus::CANCELED;
                    if (on_order_cb_) on_order_cb_(order);
                }
            } else {
                // 限价单未满，保留在队列中等待后续深度撮合
                if (order.traded_volume < order.total_volume) {
                    remaining.push_back(order);
                }
            }
        } else {
            remaining.push_back(order);
        }
    }
    active_orders_ = std::move(remaining);
}

void MatchingEngine::match_bar(const BarData& bar) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_orders_.empty()) return;

    std::vector<OrderData> remaining;
    for (auto& order : active_orders_) {
        if (order.symbol != bar.symbol) {
            remaining.push_back(order);
            continue;
        }

        bool can_fill = false;
        double fill_price = bar.open_price;

        if (order.order_type == OrderType::MARKET) {
            can_fill = true;
            fill_price = bar.open_price;
        } else if (order.order_type == OrderType::LIMIT) {
            if (order.direction == Direction::LONG) {
                if (order.price >= bar.low_price) {
                    can_fill = true;
                    fill_price = std::min(order.price, bar.open_price);
                }
            } else {
                if (order.price <= bar.high_price) {
                    can_fill = true;
                    fill_price = std::max(order.price, bar.open_price);
                }
            }
        }

        if (can_fill) {
            if (order.direction == Direction::LONG) {
                fill_price += slippage_points_;
            } else {
                fill_price -= slippage_points_;
            }

            double fill_vol = order.total_volume - order.traded_volume;
            execute_fill(order, fill_price, fill_vol, bar.timestamp_us, bar.datetime_str);
        } else {
            remaining.push_back(order);
        }
    }
    active_orders_ = std::move(remaining);
}

void MatchingEngine::execute_fill(OrderData& order, double fill_price, double fill_volume, int64_t timestamp_us, const std::string& datetime_str) {
    order.traded_volume += fill_volume;
    if (order.traded_volume >= order.total_volume) {
        order.status = OrderStatus::FILLED;
    } else {
        order.status = OrderStatus::PARTIALLY_FILLED;
    }
    order.update_time_us = timestamp_us;

    // 更新 all_orders_ 状态
    for (auto& o : all_orders_) {
        if (o.order_id == order.order_id) {
            o.traded_volume = order.traded_volume;
            o.status = order.status;
            o.update_time_us = timestamp_us;
            break;
        }
    }

    double mult = get_multiplier(order.symbol);

    // 生成成交记录
    TradeData trade;
    trade.trade_id = next_trade_id_.fetch_add(1);
    trade.order_id = order.order_id;
    trade.order_ref = order.order_ref;
    trade.symbol = order.symbol;
    trade.exchange = order.exchange;
    trade.direction = order.direction;
    trade.offset = order.offset;
    trade.price = fill_price;
    trade.volume = fill_volume;
    trade.commission = fill_price * fill_volume * mult * commission_ratio_;
    trade.trade_time_us = timestamp_us;
    trade.datetime_str = datetime_str;
    trade.strategy_name = order.strategy_name;

    all_trades_.push_back(trade);

    if (on_order_cb_) {
        on_order_cb_(order);
    }
    if (on_trade_cb_) {
        on_trade_cb_(trade);
    }
}

std::vector<OrderData> MatchingEngine::get_active_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_orders_;
}

} // namespace kun
