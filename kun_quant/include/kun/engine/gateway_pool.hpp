#pragma once

#include "kun/core/types.hpp"
#include "kun/gateway/sim_gateway.hpp"
#include "kun/engine/multi_account_router.hpp"
#include "kun/engine/position_manager.hpp"
#include "kun/engine/risk_manager.hpp"
#include "kun/engine/storage_manager.hpp"
#include "message_bus.h"
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <iostream>
#include <cstring>
#include <chrono>

namespace kun {

/**
 * @brief 柜台网关池管理器 (GatewayPool)
 * 深度复用 FlowEngine MessageBus 的命名空间机制 (trader/<account_id>/*)
 * 支持多账户并行挂载、状态监控、事前风控前置拦截 (Pre-Trade Risk Gate) 与统一断线重连
 */
class GatewayPool : public IGatewayCallback {
public:
    struct AccountContext {
        AccountProfile profile;
        std::shared_ptr<SimGateway> gateway;
        std::shared_ptr<PositionManager> pos_mgr;
        std::shared_ptr<RiskManager> risk_mgr;
    };

    explicit GatewayPool(MessageBus* bus, StorageManager* storage = nullptr)
        : bus_(bus), storage_(storage) {}
    ~GatewayPool() override {
        disconnect_all();
    }

    bool register_account(const AccountProfile& profile, const RiskRuleConfig& risk_cfg = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (accounts_.find(profile.account_id) != accounts_.end()) {
            return false;
        }

        auto ctx = std::make_shared<AccountContext>();
        ctx->profile = profile;
        ctx->gateway = std::make_shared<SimGateway>(profile.account_id);
        ctx->gateway->register_callback(this);

        // 初始化各账户独立记账与事前风控
        ctx->pos_mgr = std::make_shared<PositionManager>(profile.initial_balance);
        SymbolInfo default_symbol{"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001, "RB0"};
        ctx->pos_mgr->set_symbol_info(default_symbol);
        ctx->risk_mgr = std::make_shared<RiskManager>(*ctx->pos_mgr, risk_cfg);

        accounts_[profile.account_id] = ctx;
        acc_order_.push_back(profile.account_id); // 记住注册顺序 (首个 = 主账户)

        // 订阅专属报单管道: trader/<account_id>/order_req
        std::string order_topic = "trader/" + profile.account_id + "/order_req";
        message_bus_subscribe(bus_, order_topic.c_str(), &GatewayPool::on_order_request_static, this);

        std::cout << "[GatewayPool] 账户网关注册成功 (含事前风控门禁): " << profile.account_id 
                  << " (" << profile.broker_name << ") -> 监听 Topic: " << order_topic << "\n";
        return true;
    }

    void connect_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, ctx] : accounts_) {
            ctx->gateway->connect();
        }
    }

    void disconnect_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, ctx] : accounts_) {
            ctx->gateway->disconnect();
        }
    }

    std::shared_ptr<SimGateway> get_gateway(const std::string& account_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) return it->second->gateway;
        return nullptr;
    }

    std::shared_ptr<PositionManager> get_position_manager(const std::string& account_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) return it->second->pos_mgr;
        return nullptr;
    }

    // 为账户设置合约参数 (乘数/保证金率, 供逐持仓精确记账)
    void set_symbol_info(const std::string& account_id, const SymbolInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end() && it->second->pos_mgr) {
            it->second->pos_mgr->set_symbol_info(info);
        }
    }

    // 关闭所有账户网关的内部随机报价 (接入真实行情后不再向总线发布合成 tick)
    void disable_sim_quotes() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, ctx] : accounts_) {
            ctx->gateway->set_internal_quotes(false);
        }
    }

    // 真实行情喂单: 融合后的真实 tick 驱动各账户撮合引擎与盯市盈亏。
    // 闭环: 策略挂单 → 真实价格成交 → on_trade 落账 → 绩效分析。
    // 注意: 必须在池锁外调用网关/记账 — 撮合会同步回调 on_trade/on_order 再入池锁。
    void feed_real_tick(const TickData& tick) {
        std::vector<std::pair<std::shared_ptr<SimGateway>, std::shared_ptr<PositionManager>>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(accounts_.size());
            for (auto& [id, ctx] : accounts_) {
                snapshot.emplace_back(ctx->gateway, ctx->pos_mgr);
            }
        }
        for (auto& [gw, pm] : snapshot) {
            if (gw) gw->on_external_tick(tick);
            if (!pm) continue;
            kun::BarData bar;
            bar.symbol = tick.symbol;
            bar.exchange = tick.exchange;
            bar.close_price = tick.last_price;
            bar.open_price = tick.last_price;
            bar.high_price = tick.last_price;
            bar.low_price = tick.last_price;
            bar.volume = tick.volume;
            pm->on_bar(bar); // 盯市盈亏 (公开接口)
        }
    }

    std::shared_ptr<RiskManager> get_risk_manager(const std::string& account_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) return it->second->risk_mgr;
        return nullptr;
    }

    // ================= IGatewayCallback 实现 =================
    void on_connected() override {}
    void on_disconnected(int /*reason*/) override {}

    void on_tick(const TickData& tick) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, ctx] : accounts_) {
                if (ctx->pos_mgr) {
                    ctx->pos_mgr->on_tick(tick);
                }
            }
        }

        // 安全打包 POD 载荷，消除含 std::string 强转导致的未定义行为 (UB)
        QuantTickMsg msg{};
        std::strncpy(msg.symbol, tick.symbol.c_str(), sizeof(msg.symbol) - 1);
        std::strncpy(msg.exchange, tick.exchange.c_str(), sizeof(msg.exchange) - 1);
        msg.timestamp_us = tick.timestamp_us;
        msg.last_price = tick.last_price;
        msg.volume = tick.volume;
        msg.open_interest = tick.open_interest;
        msg.bid_price1 = tick.bid_price[0];
        msg.bid_volume1 = tick.bid_volume[0];
        msg.ask_price1 = tick.ask_price[0];
        msg.ask_volume1 = tick.ask_volume[0];

        std::string topic = "market/tick/" + tick.symbol;
        message_bus_publish(bus_, topic.c_str(), "GatewayPool", &msg, sizeof(msg));
    }

    void on_bar(const BarData& /*bar*/) override {}

    void on_order(const OrderData& order) override {
        std::string acc_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = order_to_account_.find(order.order_id);
            if (it != order_to_account_.end()) {
                acc_id = it->second;
            } else if (!acc_order_.empty()) {
                acc_id = acc_order_.front(); // 未知订单归属首个注册账户 (主账户)
            }
        }
        if (acc_id.empty()) return;

        QuantOrderRtnMsg msg{};
        std::strncpy(msg.symbol, order.symbol.c_str(), sizeof(msg.symbol) - 1);
        std::strncpy(msg.exchange, order.exchange.c_str(), sizeof(msg.exchange) - 1);
        std::strncpy(msg.strategy_name, order.strategy_name.c_str(), sizeof(msg.strategy_name) - 1);
        std::strncpy(msg.order_ref, order.order_ref.c_str(), sizeof(msg.order_ref) - 1);
        msg.order_id = order.order_id;
        msg.direction = static_cast<uint8_t>(order.direction);
        msg.offset = static_cast<uint8_t>(order.offset);
        msg.status = static_cast<uint8_t>(order.status);
        msg.price = order.price;
        msg.total_volume = order.total_volume;
        msg.traded_volume = order.traded_volume;
        msg.update_time_us = order.update_time_us;

        std::string topic = "trader/" + acc_id + "/order_rtn";
        message_bus_publish(bus_, topic.c_str(), "GatewayPool", &msg, sizeof(msg));
    }

    void on_trade(const TradeData& trade) override {
        std::string acc_id;
        std::shared_ptr<PositionManager> pos_mgr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = order_to_account_.find(trade.order_id);
            if (it != order_to_account_.end()) {
                acc_id = it->second;
            } else if (!acc_order_.empty()) {
                acc_id = acc_order_.front(); // 未知订单归属首个注册账户 (主账户)
            }
            if (!acc_id.empty()) {
                auto acc_it = accounts_.find(acc_id);
                if (acc_it != accounts_.end()) {
                    pos_mgr = acc_it->second->pos_mgr;
                }
            }
        }
            if (pos_mgr) {
                // 撮合引擎生成的成交缺时间戳, 落账前统一补上 (绩效按时间窗口过滤依赖此字段)
                TradeData stamped = trade;
                if (stamped.trade_time_us <= 0) {
                    stamped.trade_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                }
                pos_mgr->on_trade(stamped);

                // 成交落账 (SQLite 真实账本, 绩效分析数据源)
                if (storage_) {
                    storage_->save_trade(stamped, acc_id);
                    auto pos = pos_mgr->get_position(stamped.symbol, pos_dir_of(stamped));
                    if (!pos.symbol.empty()) {
                        storage_->save_position(pos, acc_id);
                    }
                }
            }
        if (acc_id.empty()) return;

        QuantTradeMsg msg{};
        std::strncpy(msg.symbol, trade.symbol.c_str(), sizeof(msg.symbol) - 1);
        std::strncpy(msg.exchange, trade.exchange.c_str(), sizeof(msg.exchange) - 1);
        std::strncpy(msg.strategy_name, trade.strategy_name.c_str(), sizeof(msg.strategy_name) - 1);
        std::strncpy(msg.order_ref, trade.order_ref.c_str(), sizeof(msg.order_ref) - 1);
        msg.trade_id = trade.trade_id;
        msg.order_id = trade.order_id;
        msg.direction = static_cast<uint8_t>(trade.direction);
        msg.offset = static_cast<uint8_t>(trade.offset);
        msg.price = trade.price;
        msg.volume = trade.volume;
        msg.commission = trade.commission;
        msg.trade_time_us = trade.trade_time_us;

        std::string topic = "trader/" + acc_id + "/trade_rtn";
        message_bus_publish(bus_, topic.c_str(), "GatewayPool", &msg, sizeof(msg));
    }

    void on_position(const PositionData& /*pos*/) override {}
    void on_account(const AccountData& /*acc*/) override {}

private:
    static void on_order_request_static(const Message* msg, void* user_data) {
        auto* self = static_cast<GatewayPool*>(user_data);
        if (!msg || !self || msg->data_size < sizeof(QuantOrderReqMsg)) return;

        // 从 topic 提取 account_id (e.g. "trader/acc_01/order_req")
        std::string topic(msg->topic);
        size_t first = topic.find('/');
        size_t second = topic.find('/', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            std::string acc_id = topic.substr(first + 1, second - first - 1);
            std::shared_ptr<AccountContext> ctx;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                auto it = self->accounts_.find(acc_id);
                if (it != self->accounts_.end()) {
                    ctx = it->second;
                }
            }

            if (ctx && ctx->gateway && ctx->risk_mgr) {
                const auto* pod = reinterpret_cast<const QuantOrderReqMsg*>(msg->data);
                OrderRequest req;
                req.symbol = pod->symbol;
                req.exchange = pod->exchange;
                req.strategy_name = pod->strategy_name;
                req.order_ref = std::to_string(pod->order_req_id);
                req.direction = static_cast<Direction>(pod->direction);
                req.offset = static_cast<Offset>(pod->offset);
                req.order_type = static_cast<OrderType>(pod->order_type);
                req.price = pod->price;
                req.volume = pod->volume;

                // ── 事前风控门禁 (Pre-Trade Risk Gate) ──
                auto active_orders = ctx->gateway->get_active_orders();
                auto [passed, reason] = ctx->risk_mgr->check_order(req, active_orders);
                if (!passed) {
                    std::cout << "[GatewayPool] 账户 [" << acc_id << "] 事前风控拦截报单! 原因: " << reason << "\n";

                    // 向总线发布拒单回报
                    QuantOrderRtnMsg rej_msg{};
                    std::strncpy(rej_msg.symbol, req.symbol.c_str(), sizeof(rej_msg.symbol) - 1);
                    std::strncpy(rej_msg.strategy_name, req.strategy_name.c_str(), sizeof(rej_msg.strategy_name) - 1);
                    std::strncpy(rej_msg.order_ref, req.order_ref.c_str(), sizeof(rej_msg.order_ref) - 1);
                    rej_msg.order_id = 0;
                    rej_msg.direction = static_cast<uint8_t>(req.direction);
                    rej_msg.offset = static_cast<uint8_t>(req.offset);
                    rej_msg.status = static_cast<uint8_t>(OrderStatus::REJECTED);
                    rej_msg.price = req.price;
                    rej_msg.total_volume = req.volume;
                    rej_msg.traded_volume = 0.0;
                    rej_msg.update_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    std::string rtn_topic = "trader/" + acc_id + "/order_rtn";
                    message_bus_publish(self->bus_, rtn_topic.c_str(), "GatewayPoolRiskGate", &rej_msg, sizeof(rej_msg));
                    return; // 严禁进入柜台/撮合引擎
                }

                uint64_t order_id = ctx->gateway->send_order(req);
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->order_to_account_[order_id] = acc_id;
                }
            }
        }
    }

    MessageBus* bus_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<AccountContext>> accounts_;
    std::vector<std::string> acc_order_; // 注册顺序, 首个 = 主账户 (兜底归属用)
    std::unordered_map<uint64_t, std::string> order_to_account_;
    StorageManager* storage_{nullptr}; // 真实账本落盘 (成交/持仓快照)

    // 成交回报方向映射为持仓方向: 开仓保持原方向, 平仓回报方向 (卖平/买平) 翻转为被平持仓方向
    static Direction pos_dir_of(const TradeData& t) {
        if (t.offset == Offset::OPEN) {
            return t.direction;
        }
        return (t.direction == Direction::LONG) ? Direction::SHORT : Direction::LONG;
    }
};

} // namespace kun
