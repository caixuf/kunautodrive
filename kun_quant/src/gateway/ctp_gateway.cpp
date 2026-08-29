#include "kun/gateway/ctp_gateway.hpp"
#include <iostream>
#include <cstring>

namespace kun {

CtpGateway::CtpGateway(MessageBus* bus, const std::string& account_id, CtpAccountConfig config)
    : ICtpGateway(account_id), bus_(bus), account_id_(account_id), config_(std::move(config)) {
    last_query_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
}

CtpGateway::~CtpGateway() {
    disconnect();
}

bool CtpGateway::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_.load()) return true;

    std::cout << "[CtpGateway::" << account_id_ << "] 正在连接 CTP 前置机: " << config_.front_trade_addr << "...\n";
    connected_.store(true);

    // 顺序执行 CTP 标准认证与登录流程
    if (authenticate() && login()) {
        req_settlement_confirm();
        std::cout << "[CtpGateway::" << account_id_ << "] CTP 柜台就绪，会话已建立。\n";
        return true;
    }
    return false;
}

void CtpGateway::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_.store(false);
    authenticated_.store(false);
    logged_in_.store(false);
    std::cout << "[CtpGateway::" << account_id_ << "] CTP 会话已安全断开。\n";
}

bool CtpGateway::is_connected() const {
    return connected_.load() && logged_in_.load();
}

bool CtpGateway::authenticate() {
    std::cout << "[CtpGateway::" << account_id_ << "] 发送客户端认证请求 (AppID=" << config_.app_id << ")...\n";
    authenticated_.store(true);
    return true;
}

bool CtpGateway::login() {
    std::cout << "[CtpGateway::" << account_id_ << "] 发送用户登录请求 (BrokerID=" << config_.broker_id << ", UserID=" << config_.user_id << ")...\n";
    logged_in_.store(true);
    return true;
}

void CtpGateway::req_settlement_confirm() {
    std::cout << "[CtpGateway::" << account_id_ << "] 发送投资者结算结果确认报文...\n";
}

void CtpGateway::subscribe(const std::string& symbol) {
    if (!connected_.load()) return;
    std::cout << "[CtpGateway::" << account_id_ << "] 订阅 CTP 行情合约: " << symbol << "\n";
}

void CtpGateway::unsubscribe(const std::string& symbol) {
    std::cout << "[CtpGateway::" << account_id_ << "] 退订 CTP 行情合约: " << symbol << "\n";
}

uint64_t CtpGateway::send_order(const OrderRequest& req) {
    if (!is_connected()) return 0;

    uint64_t order_id = next_order_id_++;
    QuantOrderReqMsg msg{};
    std::strncpy(msg.symbol, req.symbol.c_str(), sizeof(msg.symbol) - 1);
    std::strncpy(msg.exchange, req.exchange.c_str(), sizeof(msg.exchange) - 1);
    std::strncpy(msg.strategy_name, req.strategy_name.c_str(), sizeof(msg.strategy_name) - 1);
    msg.order_req_id = order_id;
    msg.direction = static_cast<uint8_t>(req.direction);
    msg.offset = static_cast<uint8_t>(req.offset);
    msg.order_type = static_cast<uint8_t>(req.order_type);
    msg.price = req.price;
    msg.volume = req.volume;

    std::string topic = "trader/" + account_id_ + "/order_req";
    message_bus_publish(bus_, topic.c_str(), "CtpGateway", &msg, sizeof(msg));
    return order_id;
}

bool CtpGateway::cancel_order(uint64_t order_id) {
    if (!is_connected()) return false;
    std::cout << "[CtpGateway::" << account_id_ << "] 发送撤单请求 order_id=" << order_id << "\n";
    return true;
}

void CtpGateway::query_account() {
    query_account_safe();
}

void CtpGateway::query_position() {
    query_positions_safe();
}

bool CtpGateway::check_query_rate_limit() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_query_time_).count();
    if (elapsed_ms < 1000) {
        std::cout << "[CtpGateway::" << account_id_ << "] [RATE_LIMIT] 触发 CTP 1秒1次限速拦截! 距上次仅 " << elapsed_ms << " ms\n";
        return false;
    }
    last_query_time_ = now;
    return true;
}

bool CtpGateway::query_account_safe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!check_query_rate_limit()) return false;
    std::cout << "[CtpGateway::" << account_id_ << "] 发送投资者资金账户查询 ReqQryTradingAccount\n";
    return true;
}

bool CtpGateway::query_positions_safe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!check_query_rate_limit()) return false;
    std::cout << "[CtpGateway::" << account_id_ << "] 发送投资者持仓明细查询 ReqQryInvestorPosition\n";
    return true;
}

void CtpGateway::on_ctp_trade_raw(const QuantTradeMsg& trade) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 核心防线：成交回报去重
    if (seen_trade_ids_.count(trade.trade_id)) {
        duplicate_trades_filtered_++;
        std::cout << "[CtpGateway::" << account_id_ << "] 成功拦截并过滤 CTP 重复推送成交回报! trade_id=" << trade.trade_id << "\n";
        return;
    }

    seen_trade_ids_.insert(trade.trade_id);

    // 广播至总线 trader/<account_id>/trade_rtn
    std::string topic = "trader/" + account_id_ + "/trade_rtn";
    message_bus_publish(bus_, topic.c_str(), "CtpGateway", &trade, sizeof(trade));
}

void CtpGateway::on_ctp_order_raw(const QuantOrderRtnMsg& order) {
    std::string topic = "trader/" + account_id_ + "/order_rtn";
    message_bus_publish(bus_, topic.c_str(), "CtpGateway", &order, sizeof(order));
}

} // namespace kun
