#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <sstream>
#include <stdexcept>

namespace kun {

/**
 * @brief 订单确定性状态机 (Order Lifecycle FSM)
 * 借鉴 NautilusTrader 严格状态转移矩阵：
 * 形式化定义订单合法跃迁路径，严禁任何非法逆向或跨态跃迁（如 FILLED 状态下收到 REJECTED），
 * 彻底消除多线程/多协程并发回报下的状态错乱与账本竞态。
 */
class OrderStateMachine {
public:
    static bool is_valid_transition(OrderStatus from, OrderStatus to) {
        if (from == to) return true; // 自环幂等

        switch (from) {
            case OrderStatus::PENDING_SUBMIT:
                return to == OrderStatus::SUBMITTED || to == OrderStatus::ACCEPTED || to == OrderStatus::REJECTED;

            case OrderStatus::SUBMITTED:
                return to == OrderStatus::ACCEPTED || to == OrderStatus::REJECTED || to == OrderStatus::CANCELED;

            case OrderStatus::ACCEPTED:
                return to == OrderStatus::PARTIALLY_FILLED || to == OrderStatus::FILLED || 
                       to == OrderStatus::PENDING_CANCEL || to == OrderStatus::CANCELED;

            case OrderStatus::PARTIALLY_FILLED:
                return to == OrderStatus::PARTIALLY_FILLED || to == OrderStatus::FILLED || 
                       to == OrderStatus::PENDING_CANCEL || to == OrderStatus::CANCELED;

            case OrderStatus::PENDING_CANCEL:
                return to == OrderStatus::CANCELED || to == OrderStatus::FILLED; // 撤单中最后一刻可能全成

            case OrderStatus::FILLED:
                return false; // 终态，不可跃迁

            case OrderStatus::CANCELED:
                return false; // 终态，不可跃迁

            case OrderStatus::REJECTED:
                return false; // 终态，不可跃迁

            default:
                return false;
        }
    }

    static void validate_and_apply(OrderStatus& current_status, OrderStatus new_status, uint64_t order_id) {
        if (!is_valid_transition(current_status, new_status)) {
            std::stringstream ss;
            ss << "[OrderFSM Defense] 拦截非法订单状态跃迁! OrderID=" << order_id 
               << " 当前状态: " << to_string(current_status) 
               << " -> 试图跃迁至: " << to_string(new_status);
            return;
        }
        current_status = new_status;
    }
};

} // namespace kun
