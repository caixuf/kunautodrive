#include "kun/gateway/sim_gateway.hpp"
#include "kun/core/logger.hpp"
#include <random>
#include <chrono>

namespace kun {

SimGateway::SimGateway(std::string name)
    : IGateway(std::move(name)), matching_engine_(0.0, 0.0001) {
    matching_engine_.set_callbacks(
        [this](const OrderData& order) {
            if (cb_) cb_->on_order(order);
        },
        [this](const TradeData& trade) {
            if (cb_) cb_->on_trade(trade);
        }
    );
}

SimGateway::~SimGateway() {
    disconnect();
}

bool SimGateway::connect() {
    if (is_running_.load()) return true;
    is_running_.store(true);
    quote_thread_ = std::thread(&SimGateway::quote_generator_loop, this);
    KUN_LOG_INFO(gateway_name_, "Connected to SimGateway successfully.");
    if (cb_) cb_->on_connected();
    return true;
}

void SimGateway::disconnect() {
    if (!is_running_.load()) return;
    is_running_.store(false);
    if (quote_thread_.joinable()) {
        quote_thread_.join();
    }
    KUN_LOG_INFO(gateway_name_, "Disconnected from SimGateway.");
    if (cb_) cb_->on_disconnected(0);
}

void SimGateway::subscribe(const std::string& symbol) {
    subscribed_symbols_.push_back(symbol);
    KUN_LOG_INFO(gateway_name_, "Subscribed to symbol: " + symbol);
}

void SimGateway::unsubscribe(const std::string& symbol) {
    // 简化实现
}

void SimGateway::add_mock_symbol(const std::string& symbol, double base_price) {
    base_prices_[symbol] = base_price;
}

uint64_t SimGateway::send_order(const OrderRequest& req) {
    return matching_engine_.submit_order(req);
}

bool SimGateway::cancel_order(uint64_t order_id) {
    return matching_engine_.cancel_order(order_id);
}

void SimGateway::query_account() {
    // 模拟账户回报
}

void SimGateway::query_position() {
    // 模拟持仓回报
}

void SimGateway::quote_generator_loop() {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.5); // 价格随机扰动

    while (is_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms_));
        if (!is_running_.load()) break;

        for (const auto& symbol : subscribed_symbols_) {
            double& price = base_prices_[symbol];
            if (price <= 0.0) price = 3600.0;
            price += dist(rng);

            auto now = std::chrono::system_clock::now();
            auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

            TickData tick;
            tick.symbol = symbol;
            tick.exchange = "SHFE";
            tick.timestamp_us = now_us;
            tick.last_price = price;
            tick.volume += 50.0;
            tick.bid_price[0] = price - 1.0;
            tick.ask_price[0] = price + 1.0;
            tick.bid_volume[0] = 20.0;
            tick.ask_volume[0] = 20.0;

            // 撮合挂单
            matching_engine_.match_tick(tick);

            // 推送行情
            if (cb_) {
                cb_->on_tick(tick);
            }
        }
    }
}

} // namespace kun
