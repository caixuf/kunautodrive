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
    double max_order_volume{100.0};       // 单笔最大报单手数
    double max_symbol_position{500.0};    // 单标的最大持仓手数
    int max_orders_per_second{50};        // 每秒最大报单次数 (防死循环刷单)
    bool enable_self_trade_check{true};   // 开启自成交防范
    bool enable_cash_check{true};         // 开启可用资金检查
};

/**
 * @brief 事前风控模块 (Pre-Trade Risk Manager)
 * 在订单发送至柜台/撮合引擎前进行严格校验与拦截
 */
class RiskManager {
public:
    RiskManager(PositionManager& pos_mgr, RiskRuleConfig config = {});

    void update_config(const RiskRuleConfig& config);

    // 校验报单请求，返回 pair<是否通过, 拒绝原因>
    std::pair<bool, std::string> check_order(const OrderRequest& req, const std::vector<OrderData>& active_orders);

private:
    std::mutex mutex_;
    PositionManager& pos_mgr_;
    RiskRuleConfig config_;
    std::deque<int64_t> order_timestamps_us_;
};

} // namespace kun
