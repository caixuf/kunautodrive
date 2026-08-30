#include "kun/engine/risk_manager.hpp"
#include "kun/core/logger.hpp"

namespace kun {

RiskManager::RiskManager(PositionManager& pos_mgr, RiskRuleConfig config)
    : pos_mgr_(pos_mgr), config_(config) {}

void RiskManager::update_config(const RiskRuleConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

std::pair<bool, std::string> RiskManager::check_order(const OrderRequest& req, const std::vector<OrderData>& active_orders) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 单笔报单量检查
    if (req.volume <= 0.0) {
        return {false, "Order volume must be positive."};
    }
    if (req.volume > config_.max_order_volume) {
        return {false, "Order volume " + std::to_string(req.volume) + " exceeds max limit " + std::to_string(config_.max_order_volume)};
    }

    // 2. 报单频率限流检查 (1秒滑动窗口)
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    while (!order_timestamps_us_.empty() && (now_us - order_timestamps_us_.front() > 1000000)) {
        order_timestamps_us_.pop_front();
    }
    if (static_cast<int>(order_timestamps_us_.size()) >= config_.max_orders_per_second) {
        return {false, "Order rate limit exceeded (" + std::to_string(config_.max_orders_per_second) + "/s)"};
    }

    // 3. 最大持仓量检查 (开仓时检查)
    if (req.offset == Offset::OPEN) {
        auto cur_pos = pos_mgr_.get_position(req.symbol, req.direction);
        if (cur_pos.volume + req.volume > config_.max_symbol_position) {
            return {false, "Target position would exceed max limit " + std::to_string(config_.max_symbol_position)};
        }
    }

    // 4. 自成交防范检查 (检查反向未成交挂单价格)
    if (config_.enable_self_trade_check) {
        for (const auto& active_order : active_orders) {
            if (active_order.symbol == req.symbol && active_order.status == OrderStatus::ACCEPTED) {
                // 如果当前是买单，检查是否存在卖单挂单价格 <= 当前买价
                if (req.direction == Direction::LONG && active_order.direction == Direction::SHORT) {
                    if (req.price >= active_order.price) {
                        return {false, "Potential self-trade detected with active order " + std::to_string(active_order.order_id)};
                    }
                }
                // 如果当前是卖单，检查是否存在买单挂单价格 >= 当前卖价
                else if (req.direction == Direction::SHORT && active_order.direction == Direction::LONG) {
                    if (req.price <= active_order.price) {
                        return {false, "Potential self-trade detected with active order " + std::to_string(active_order.order_id)};
                    }
                }
            }
        }
    }

    // 5. 可用资金与平仓可用手数检查
    if (req.offset == Offset::OPEN) {
        if (config_.enable_cash_check) {
            auto account = pos_mgr_.get_account();
            int multiplier = 10;
            double margin_ratio = 0.10;
            if (auto info = pos_mgr_.get_symbol_info(req.symbol)) {
                multiplier = info->multiplier;
                margin_ratio = info->margin_ratio;
            }
            double req_margin = req.price * req.volume * multiplier * margin_ratio;
            if (account.available < req_margin) {
                return {false, "Insufficient available cash. Required: " + std::to_string(req_margin) + ", Available: " + std::to_string(account.available)};
            }
        }
    } else {
        // 平仓检查：平仓手数不能大于现有持仓
        // 注意: 报单方向是操作方向 (卖平/买平), 被平持仓方向为其反向 ——
        // 用报单方向查仓会把卖平单全部误拦 (查的是空头仓, 实际持多头)
        Direction pos_dir = (req.direction == Direction::LONG) ? Direction::SHORT : Direction::LONG;
        auto cur_pos = pos_mgr_.get_position(req.symbol, pos_dir);
        if (req.volume > cur_pos.volume) {
            return {false, "Close volume " + std::to_string(req.volume) + " exceeds current position " + std::to_string(cur_pos.volume)};
        }
    }

    order_timestamps_us_.push_back(now_us);
    return {true, ""};
}

} // namespace kun
