#pragma once

#include "kun/gateway/gateway_base.hpp"
#include "kun/engine/matching_engine.hpp"
#include <thread>
#include <atomic>
#include <vector>

namespace kun {

/**
 * @brief 实时仿真交易网关 (SimGateway)
 * 可模拟真实 500ms Tick 推送与实时撮合回报，用于实盘策略压力测试
 */
class SimGateway : public IGateway {
public:
    explicit SimGateway(std::string name = "SimGateway");
    ~SimGateway() override;

    bool connect() override;
    void disconnect() override;

    void subscribe(const std::string& symbol) override;
    void unsubscribe(const std::string& symbol) override;

    uint64_t send_order(const OrderRequest& req) override;
    bool cancel_order(uint64_t order_id) override;

    void query_account() override;
    void query_position() override;

    std::vector<OrderData> get_active_orders() const {
        return matching_engine_.get_active_orders();
    }

    void set_tick_interval_ms(int ms) { tick_interval_ms_ = ms; }
    void add_mock_symbol(const std::string& symbol, double base_price);

private:
    void quote_generator_loop();

    std::atomic<bool> is_running_{false};
    std::thread quote_thread_;
    int tick_interval_ms_{200}; // 模拟 200ms Tick 刷新频率

    MatchingEngine matching_engine_;
    std::unordered_map<std::string, double> base_prices_;
    std::vector<std::string> subscribed_symbols_;
};

} // namespace kun
