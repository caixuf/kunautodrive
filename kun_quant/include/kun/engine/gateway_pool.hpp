#pragma once

#include "kun/core/types.hpp"
#include "kun/gateway/sim_gateway.hpp"
#include "kun/engine/multi_account_router.hpp"
#include "message_bus.h"
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <iostream>

namespace kun {

/**
 * @brief 柜台网关池管理器 (GatewayPool)
 * 深度复用 FlowEngine MessageBus 的命名空间机制 (trader/<account_id>/*)
 * 支持多账户并行挂载、状态监控与统一断线重连
 */
class GatewayPool : public IGatewayCallback {
public:
    explicit GatewayPool(MessageBus* bus) : bus_(bus) {}
    ~GatewayPool() override {
        disconnect_all();
    }

    bool register_account(const AccountProfile& profile) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gateways_.find(profile.account_id) != gateways_.end()) {
            return false;
        }

        // 创建专属网关实例 (仿真或CTP)
        auto gw = std::make_shared<SimGateway>(profile.account_id);
        gw->register_callback(this);
        gateways_[profile.account_id] = gw;
        profiles_[profile.account_id] = profile;

        // 订阅专属报单管道: trader/<account_id>/order_req
        std::string order_topic = "trader/" + profile.account_id + "/order_req";
        message_bus_subscribe(bus_, order_topic.c_str(), &GatewayPool::on_order_request_static, this);

        std::cout << "[GatewayPool] 账户网关注册成功: " << profile.account_id 
                  << " (" << profile.broker_name << ") -> 监听 Topic: " << order_topic << "\n";
        return true;
    }

    void connect_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, gw] : gateways_) {
            gw->connect();
        }
    }

    void disconnect_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, gw] : gateways_) {
            gw->disconnect();
        }
    }

    std::shared_ptr<SimGateway> get_gateway(const std::string& account_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gateways_.find(account_id);
        if (it != gateways_.end()) return it->second;
        return nullptr;
    }

    // ================= IGatewayCallback 实现 =================
    void on_connected() override {}
    void on_disconnected(int /*reason*/) override {}

    void on_tick(const TickData& tick) override {
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
            } else if (!profiles_.empty()) {
                acc_id = profiles_.begin()->first;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = order_to_account_.find(trade.order_id);
            if (it != order_to_account_.end()) {
                acc_id = it->second;
            } else if (!profiles_.empty()) {
                acc_id = profiles_.begin()->first;
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
            auto gw = self->get_gateway(acc_id);
            if (gw) {
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

                uint64_t order_id = gw->send_order(req);
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->order_to_account_[order_id] = acc_id;
                }
            }
        }
    }

    MessageBus* bus_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SimGateway>> gateways_;
    std::unordered_map<std::string, AccountProfile> profiles_;
    std::unordered_map<uint64_t, std::string> order_to_account_;
};

} // namespace kun
