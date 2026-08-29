#pragma once

#include "kun/core/types.hpp"
#include "kun/engine/position_manager.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <cstdint>

namespace kun {

enum class LedgerEventType : uint8_t {
    CASH_DEPOSIT = 1,
    CASH_WITHDRAW = 2,
    ORDER_SUBMITTED = 10,
    ORDER_CANCELED = 11,
    TRADE_EXECUTED = 20,
    DAILY_SETTLEMENT = 30
};

struct LedgerEvent {
    uint64_t sequence_id{0};
    uint64_t timestamp_us{0};
    LedgerEventType event_type{LedgerEventType::ORDER_SUBMITTED};
    std::string account_id;
    std::string symbol;
    Direction direction{Direction::LONG};
    Offset offset{Offset::OPEN};
    double price{0.0};
    double volume{0.0};
    double amount{0.0}; // 资金变动量或成交额
    double fee{0.0};
};

/**
 * @brief 金融级事件溯源与账务重放引擎 (EventSourcingReplayer)
 * 遵循不可篡改账本规范：任何时刻的账户状态、今昨仓持仓与保证金均可从全量 Event 日志精准重放重构
 */
class EventSourcingReplayer {
public:
    explicit EventSourcingReplayer(double initial_capital = 1000000.0);

    // 追加不可篡改事件
    void append_event(const LedgerEvent& ev);

    // 从事件序列重放恢复最新状态
    AccountData replay_all();

    // 重放至指定历史时间戳
    AccountData replay_until(uint64_t target_timestamp_us);

    const std::vector<LedgerEvent>& get_events() const { return event_log_; }
    size_t event_count() const { return event_log_.size(); }

    // 账本防篡改校验和 (Checksum)
    uint64_t calculate_audit_checksum() const;

private:
    double initial_capital_{1000000.0};
    std::vector<LedgerEvent> event_log_;
    uint64_t next_seq_id_{1};
};

} // namespace kun
