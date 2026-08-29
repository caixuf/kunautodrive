#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <functional>

namespace kun {

// 网关回调接口定义
struct IGatewayCallback {
    virtual ~IGatewayCallback() = default;
    virtual void on_connected() = 0;
    virtual void on_disconnected(int reason) = 0;
    virtual void on_tick(const TickData& tick) = 0;
    virtual void on_bar(const BarData& bar) = 0;
    virtual void on_order(const OrderData& order) = 0;
    virtual void on_trade(const TradeData& trade) = 0;
    virtual void on_position(const PositionData& pos) = 0;
    virtual void on_account(const AccountData& acc) = 0;
};

/**
 * @brief 交易与行情网关接口基类 (IGateway)
 * 统一抽象 CTP / XTP / Binance / 仿真模拟网关
 */
class IGateway {
public:
    explicit IGateway(std::string name) : gateway_name_(std::move(name)) {}
    virtual ~IGateway() = default;

    const std::string& get_name() const { return gateway_name_; }

    virtual bool connect() = 0;
    virtual void disconnect() = 0;

    virtual void subscribe(const std::string& symbol) = 0;
    virtual void unsubscribe(const std::string& symbol) = 0;

    virtual uint64_t send_order(const OrderRequest& req) = 0;
    virtual bool cancel_order(uint64_t order_id) = 0;

    virtual void query_account() = 0;
    virtual void query_position() = 0;

    void register_callback(IGatewayCallback* cb) { cb_ = cb; }

protected:
    std::string gateway_name_;
    IGatewayCallback* cb_{nullptr};
};

} // namespace kun
