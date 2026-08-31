#include "kun/engine/risk_manager.hpp"
#include "kun/market/market_heartbeat_watchdog.hpp"
#include "kun/core/logger.hpp"

namespace kun {

RiskManager::RiskManager(PositionManager& pos_mgr, RiskRuleConfig config)
    : pos_mgr_(pos_mgr), config_(config) {
    initial_equity_ = pos_mgr_.get_account().balance;
}

void RiskManager::update_config(const RiskRuleConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

bool RiskManager::is_circuit_breaker_tripped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return circuit_breaker_tripped_;
}

void RiskManager::trip_circuit_breaker(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    circuit_breaker_tripped_ = true;
    trip_reason_ = reason;
    std::cerr << "[RiskManager::CIRCUIT_BREAKER_TRIPPED] 触发硬熔断! 账户所有开仓已锁死. 原因: " << reason << "\n";
}

void RiskManager::reset_circuit_breaker() {
    std::lock_guard<std::mutex> lock(mutex_);
    circuit_breaker_tripped_ = false;
    trip_reason_.clear();
    consecutive_rejections_ = 0;
    initial_equity_ = pos_mgr_.get_account().balance;
}

std::string RiskManager::get_trip_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trip_reason_;
}

std::pair<bool, std::string> RiskManager::check_order(const OrderRequest& req, const std::vector<OrderData>& active_orders) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 0. 硬熔断状态检查 (Circuit Breaker Gate)
    if (circuit_breaker_tripped_) {
        if (req.offset == Offset::OPEN) {
            return {false, "[CIRCUIT_BREAKER_LOCKED] 触发全局硬熔断，严禁新开仓! 原因: " + trip_reason_};
        }
    }

    // 0.1 动态回撤硬熔断检查 (Max Daily Drawdown Gate)
    if (initial_equity_ > 0.0 && req.offset == Offset::OPEN) {
        double current_equity = pos_mgr_.get_account().dynamic_equity();
        double max_allowed_loss = initial_equity_ * config_.max_daily_loss_pct;
        if (initial_equity_ - current_equity >= max_allowed_loss) {
            circuit_breaker_tripped_ = true;
            trip_reason_ = "单日动态回撤超限 (" + std::to_string((initial_equity_ - current_equity)) + 
                           " >= " + std::to_string(max_allowed_loss) + ")";
            std::cerr << "[RiskManager::CIRCUIT_BREAKER_TRIPPED] " << trip_reason_ << "\n";
            return {false, "[CIRCUIT_BREAKER_LOCKED] " + trip_reason_};
        }
    }

    // 0.2 行情流心跳断流检查 (Market Stream Staleness Gate)
    if (watchdog_ && req.offset == Offset::OPEN) {
        if (!watchdog_->is_symbol_healthy(req.symbol)) {
            consecutive_rejections_++;
            return {false, "[MARKET_STALE_FREEZE] 标的 [" + req.symbol + "] 行情断流超时已达 " +
                           std::to_string(watchdog_->get_elapsed_ms(req.symbol)) + "ms，风控已冻结新开仓防护!"};
        }
    }

    // 1. 单笔报单量检查
    if (req.volume <= 0.0) {
        consecutive_rejections_++;
        return {false, "Order volume must be positive."};
    }
    if (req.volume > config_.max_order_volume) {
        consecutive_rejections_++;
        return {false, "Order volume " + std::to_string(req.volume) + " exceeds max limit " + std::to_string(config_.max_order_volume)};
    }

    // 2. 报单频率限流检查 (1秒滑动窗口)
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    while (!order_timestamps_us_.empty() && (now_us - order_timestamps_us_.front() > 1000000)) {
        order_timestamps_us_.pop_front();
    }
    if (static_cast<int>(order_timestamps_us_.size()) >= config_.max_orders_per_second) {
        consecutive_rejections_++;
        return {false, "Order rate limit exceeded (" + std::to_string(config_.max_orders_per_second) + "/s)"};
    }

    // 3. 最大持仓量检查 (开仓时检查)
    if (req.offset == Offset::OPEN) {
        auto cur_pos = pos_mgr_.get_position(req.symbol, req.direction);
        if (cur_pos.volume + req.volume > config_.max_symbol_position) {
            consecutive_rejections_++;
            return {false, "Target position would exceed max limit " + std::to_string(config_.max_symbol_position)};
        }
    }

    // 4. 自成交防范检查 (检查反向未成交挂单价格)
    if (config_.enable_self_trade_check) {
        for (const auto& active_order : active_orders) {
            if (active_order.symbol == req.symbol && active_order.status == OrderStatus::ACCEPTED) {
                if (req.direction == Direction::LONG && active_order.direction == Direction::SHORT) {
                    if (req.price >= active_order.price) {
                        consecutive_rejections_++;
                        return {false, "Potential self-trade detected with active order " + std::to_string(active_order.order_id)};
                    }
                } else if (req.direction == Direction::SHORT && active_order.direction == Direction::LONG) {
                    if (req.price <= active_order.price) {
                        consecutive_rejections_++;
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
                consecutive_rejections_++;
                return {false, "Insufficient available cash. Required: " + std::to_string(req_margin) + ", Available: " + std::to_string(account.available)};
            }
        }
    } else {
        Direction pos_dir = (req.direction == Direction::LONG) ? Direction::SHORT : Direction::LONG;
        auto cur_pos = pos_mgr_.get_position(req.symbol, pos_dir);
        if (req.volume > cur_pos.volume) {
            consecutive_rejections_++;
            return {false, "Close volume " + std::to_string(req.volume) + " exceeds current position " + std::to_string(cur_pos.volume)};
        }
    }

    // 检查连续拒单熔断
    if (config_.max_consecutive_rejections > 0 && consecutive_rejections_ >= config_.max_consecutive_rejections) {
        circuit_breaker_tripped_ = true;
        trip_reason_ = "连续拒单达到 " + std::to_string(consecutive_rejections_) + " 次，触发防护熔断";
        std::cerr << "[RiskManager::CIRCUIT_BREAKER_TRIPPED] " << trip_reason_ << "\n";
        return {false, "[CIRCUIT_BREAKER_LOCKED] " + trip_reason_};
    }

    consecutive_rejections_ = 0; // 成功通过校验，清零连续拒单
    order_timestamps_us_.push_back(now_us);
    return {true, ""};
}

} // namespace kun
