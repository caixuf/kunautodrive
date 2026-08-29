#pragma once

#include "kun/core/types.hpp"
#include "kun/core/logger.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace kun {

// 前向声明引擎类
class IStrategyContext {
public:
    virtual ~IStrategyContext() = default;

    virtual uint64_t send_order(const OrderRequest& req) = 0;
    virtual bool cancel_order(uint64_t order_id) = 0;

    virtual PositionData get_position(const std::string& symbol, Direction direction) const = 0;
    virtual AccountData get_account() const = 0;
    virtual const TickData* get_latest_tick(const std::string& symbol) const = 0;
};

/**
 * @brief 策略基类 (IStrategy)
 * 借鉴 WonderTrader / CTAStrategy 经典结构，规范生命周期与交易指令
 */
class IStrategy {
public:
    explicit IStrategy(std::string name) : strategy_name_(std::move(name)) {}
    virtual ~IStrategy() = default;

    void set_context(IStrategyContext* ctx) { ctx_ = ctx; }
    const std::string& get_name() const { return strategy_name_; }

    // ================= 策略生命周期回调 =================
    virtual void on_init() {}
    virtual void on_start() {
        KUN_LOG_INFO(strategy_name_, "Strategy started.");
    }
    virtual void on_stop() {
        KUN_LOG_INFO(strategy_name_, "Strategy stopped.");
    }

    // ================= 行情与回报回调 =================
    virtual void on_tick(const TickData& tick) {}
    virtual void on_bar(const BarData& bar) {}
    virtual void on_order(const OrderData& order) {}
    virtual void on_trade(const TradeData& trade) {}

    // ================= 快捷交易指令封装 =================
    uint64_t buy(const std::string& symbol, double price, double volume, const std::string& exchange = "") {
        return send_order_internal(symbol, exchange, Direction::LONG, Offset::OPEN, price, volume);
    }

    uint64_t sell(const std::string& symbol, double price, double volume, const std::string& exchange = "") {
        return send_order_internal(symbol, exchange, Direction::LONG, Offset::CLOSE, price, volume);
    }

    uint64_t short_sell(const std::string& symbol, double price, double volume, const std::string& exchange = "") {
        return send_order_internal(symbol, exchange, Direction::SHORT, Offset::OPEN, price, volume);
    }

    uint64_t cover(const std::string& symbol, double price, double volume, const std::string& exchange = "") {
        return send_order_internal(symbol, exchange, Direction::SHORT, Offset::CLOSE, price, volume);
    }

    bool cancel_order(uint64_t order_id) {
        if (ctx_) {
            return ctx_->cancel_order(order_id);
        }
        return false;
    }

    PositionData get_position(const std::string& symbol, Direction direction) const {
        if (ctx_) {
            return ctx_->get_position(symbol, direction);
        }
        return {};
    }

    AccountData get_account() const {
        if (ctx_) {
            return ctx_->get_account();
        }
        return {};
    }

protected:
    uint64_t send_order_internal(const std::string& symbol, const std::string& exchange,
                                 Direction direction, Offset offset, double price, double volume) {
        if (!ctx_) {
            KUN_LOG_ERROR(strategy_name_, "Cannot send order: StrategyContext is null!");
            return 0;
        }

        OrderRequest req;
        req.symbol = symbol;
        req.exchange = exchange;
        req.direction = direction;
        req.offset = offset;
        req.order_type = (price <= 0.0) ? OrderType::MARKET : OrderType::LIMIT;
        req.price = price;
        req.volume = volume;
        req.strategy_name = strategy_name_;
        req.order_ref = strategy_name_ + "_" + std::to_string(++order_seq_);

        return ctx_->send_order(req);
    }

    std::string strategy_name_;
    IStrategyContext* ctx_{nullptr};
    uint32_t order_seq_{0};
};

} // namespace kun
