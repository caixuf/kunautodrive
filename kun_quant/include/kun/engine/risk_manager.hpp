#pragma once

#include "kun/core/types.hpp"
#include "kun/engine/position_manager.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <chrono>

namespace kun {

struct RiskRuleConfig {
    double max_order_volume{100.0};          // 单笔最大报单手数
    double max_symbol_position{500.0};       // 单标的最大持仓手数
    int max_orders_per_second{50};           // 每秒最大报单次数 (防死循环刷单)
    bool enable_self_trade_check{true};      // 开启自成交防范
    bool enable_cash_check{true};            // 开启可用资金检查
    double max_daily_loss_pct{0.025};        // 单日最大亏损熔断阈值 (2.5%)
    int max_consecutive_rejections{10};      // 最大连续拒单熔断阈值
};

/**
 * @brief 生产级事前风控与硬熔断模块 (Pre-Trade Risk Manager & Circuit Breaker)
 * 在订单发送至柜台/撮合引擎前进行严格校验与拦截，内置单日回撤与连续错单硬熔断
 */
class RiskManager {
public:
    RiskManager(PositionManager& pos_mgr, RiskRuleConfig config = {});

    void update_config(const RiskRuleConfig& config);

    // 校验报单请求，返回 pair<是否通过, 拒绝原因>
    std::pair<bool, std::string> check_order(const OrderRequest& req, const std::vector<OrderData>& active_orders);

    // 熔断状态查询与触发
    bool is_circuit_breaker_tripped() const;
    void trip_circuit_breaker(const std::string& reason);
    void reset_circuit_breaker(); // 仅限结算换日调用
    std::string get_trip_reason() const;

    // 行情心跳看门狗挂载
    void set_market_watchdog(const class MarketHeartbeatWatchdog* watchdog) {
        watchdog_ = watchdog;
    }

private:
    mutable std::mutex mutex_;
    PositionManager& pos_mgr_;
    RiskRuleConfig config_;
    std::deque<int64_t> order_timestamps_us_;
    const class MarketHeartbeatWatchdog* watchdog_{nullptr};

    double initial_equity_{0.0};
    bool circuit_breaker_tripped_{false};
    std::string trip_reason_;
    int consecutive_rejections_{0};
};

} // namespace kun
