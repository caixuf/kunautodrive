#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <cmath>

namespace kun {

enum class SmartExecutionStyle {
    PASSIVE_QUEUE = 0,    // 被动排队 (赚取买卖价差，0 滑点，流动性充裕时首选)
    AGGRESSIVE_TAKER = 1  // 主动吃单 (对价抢单，动量爆发瞬间首选，防踏空)
};

inline std::string to_string(SmartExecutionStyle s) {
    switch (s) {
        case SmartExecutionStyle::PASSIVE_QUEUE:    return "PASSIVE_QUEUE";
        case SmartExecutionStyle::AGGRESSIVE_TAKER: return "AGGRESSIVE_TAKER";
        default: return "UNKNOWN";
    }
}

struct RoutedOrder {
    std::string symbol;
    Direction direction{Direction::LONG};
    Offset offset{Offset::OPEN};
    double target_price{0.0};
    double volume{1.0};
    OrderType order_type{OrderType::LIMIT};
    SmartExecutionStyle style{SmartExecutionStyle::PASSIVE_QUEUE};
    double calculated_ofi{0.0}; // 订单流不平衡量 (Order Flow Imbalance)
    std::string route_reason;
};

/**
 * @brief 订单流微观不平衡 (OFI) 智能报单路由器 (SmartOrderRouter)
 * 核心解决 CTP 柜台外部物理延迟与滑点磨损：
 * 1. 静态/平稳盘口：使用买一/卖一排队价被动挂单，省下 1~2 跳滑点与高额对价手续费
 * 2. OFI 动量突发：瞬间切换为对价主动吃单，保障动量突破 100% 抢先成交
 */
class SmartOrderRouter {
public:
    explicit SmartOrderRouter(double ofi_momentum_threshold = 40.0)
        : ofi_momentum_threshold_(ofi_momentum_threshold) {}

    /**
     * @brief 根据实时 5 档盘口与策略原始开仓意图，计算最优执行路由
     */
    RoutedOrder route_order(
        const QuantTickMsg& tick,
        Direction dir,
        Offset offset,
        double target_vol,
        double base_price
    ) {
        RoutedOrder res;
        res.symbol = tick.symbol;
        res.direction = dir;
        res.offset = offset;
        res.volume = target_vol;

        // 计算 5 档盘口微观买卖气压不平衡 (Order Flow Imbalance proxy)
        double total_bid_vol = tick.bid_volume1;
        double total_ask_vol = tick.ask_volume1;
        double ofi = total_bid_vol - total_ask_vol;
        res.calculated_ofi = ofi;

        if (dir == Direction::LONG) {
            // 买入方向：若买盘力量压倒性占优 (OFI > 阈值)，说明突破在即，采用对价快速吃单
            if (ofi >= ofi_momentum_threshold_) {
                res.style = SmartExecutionStyle::AGGRESSIVE_TAKER;
                res.target_price = (tick.ask_price1 > 0.0) ? tick.ask_price1 : base_price;
                res.order_type = OrderType::LIMIT;
                res.route_reason = "OFI买盘动量爆发，对价主动吃单 (Aggressive Taker)";
            } else {
                // 平稳盘口：挂买一排队价，被动等待对手单撞击，节省滑点
                res.style = SmartExecutionStyle::PASSIVE_QUEUE;
                res.target_price = (tick.bid_price1 > 0.0) ? tick.bid_price1 : base_price;
                res.order_type = OrderType::LIMIT;
                res.route_reason = "盘口微观平稳，买一排队挂单 (Passive Queueing)";
            }
        } else {
            // 卖出方向：若卖盘力量压倒性占优 (OFI < -阈值)，采用买一价主动砸盘
            if (ofi <= -ofi_momentum_threshold_) {
                res.style = SmartExecutionStyle::AGGRESSIVE_TAKER;
                res.target_price = (tick.bid_price1 > 0.0) ? tick.bid_price1 : base_price;
                res.order_type = OrderType::LIMIT;
                res.route_reason = "OFI卖盘动量下砸，对价主动吃单 (Aggressive Taker)";
            } else {
                // 挂卖一排队价
                res.style = SmartExecutionStyle::PASSIVE_QUEUE;
                res.target_price = (tick.ask_price1 > 0.0) ? tick.ask_price1 : base_price;
                res.order_type = OrderType::LIMIT;
                res.route_reason = "盘口微观平稳，卖一排队挂单 (Passive Queueing)";
            }
        }

        return res;
    }

private:
    double ofi_momentum_threshold_{40.0};
};

} // namespace kun
