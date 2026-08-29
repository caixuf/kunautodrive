#pragma once

#include "kun/core/types.hpp"
#include "kun/gateway/gateway_base.hpp"
#include "message_bus.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <atomic>

namespace kun {

struct CtpAccountConfig {
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string front_trade_addr;
    std::string front_market_addr;
};

/**
 * @brief 生产级 CTP (综合交易平台) 实盘与 SimNow 网关抽象接口
 */
class ICtpGateway : public IGateway {
public:
    explicit ICtpGateway(std::string name) : IGateway(std::move(name)) {}
    ~ICtpGateway() override = default;
    virtual bool authenticate() = 0;
    virtual bool login() = 0;
    virtual void req_settlement_confirm() = 0;
};

/**
 * @brief 生产级 CTP 仿真与实盘网关实现 (CtpGateway)
 * 
 * 核心特性：
 * 1. 查询频率限速流控 (Rate Limiter)：严格遵守 CTP 柜台 1 秒最多 1 次查询的合规要求，防止封号
 * 2. 成交回报去重器 (Deduplication Filter)：解决断线重连时 CTP 重发回报导致的总线重复成交与记账错乱
 * 3. 指数退避断线自动重连机 (Auto Reconnect)
 * 4. 深度桥接 KunAutoDrive MessageBus (trader/<account_id>/...)
 */
class CtpGateway : public ICtpGateway {
public:
    CtpGateway(MessageBus* bus, const std::string& account_id, CtpAccountConfig config);
    ~CtpGateway() override;

    bool connect() override;
    void disconnect() override;
    bool is_connected() const;

    bool authenticate() override;
    bool login() override;
    void req_settlement_confirm() override;

    void subscribe(const std::string& symbol) override;
    void unsubscribe(const std::string& symbol) override;

    uint64_t send_order(const OrderRequest& req) override;
    bool cancel_order(uint64_t order_id) override;

    void query_account() override;
    void query_position() override;

    // 限速查询接口 (受 Rate Limiter 保护)
    bool query_account_safe();
    bool query_positions_safe();

    // 接收 CTP 原始成交回报 (带去重过滤)
    void on_ctp_trade_raw(const QuantTradeMsg& trade);
    void on_ctp_order_raw(const QuantOrderRtnMsg& order);

    const CtpAccountConfig& get_config() const { return config_; }
    uint64_t get_duplicate_trades_filtered() const { return duplicate_trades_filtered_; }

private:
    bool check_query_rate_limit();

    MessageBus* bus_;
    std::string account_id_;
    CtpAccountConfig config_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> authenticated_{false};
    std::atomic<bool> logged_in_{false};
    std::atomic<uint64_t> next_order_id_{1000};

    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point last_query_time_;
    
    // 成交回报去重集合 (trade_id / sys_id)
    std::unordered_set<uint64_t> seen_trade_ids_;
    uint64_t duplicate_trades_filtered_{0};
};

} // namespace kun
