#include "kun/storage/event_sourcing.hpp"
#include <algorithm>
#include <cstring>

namespace kun {

EventSourcingReplayer::EventSourcingReplayer(double initial_capital)
    : initial_capital_(initial_capital) {}

void EventSourcingReplayer::append_event(const LedgerEvent& ev) {
    LedgerEvent e = ev;
    if (e.sequence_id == 0) {
        e.sequence_id = next_seq_id_++;
    } else {
        if (e.sequence_id >= next_seq_id_) {
            next_seq_id_ = e.sequence_id + 1;
        }
    }
    event_log_.push_back(e);
}

AccountData EventSourcingReplayer::replay_all() {
    return replay_until(UINT64_MAX);
}

AccountData EventSourcingReplayer::replay_until(uint64_t target_timestamp_us) {
    PositionManager pos_mgr(initial_capital_);

    for (const auto& ev : event_log_) {
        if (ev.timestamp_us > target_timestamp_us) break;

        switch (ev.event_type) {
            case LedgerEventType::TRADE_EXECUTED: {
                TradeData td{};
                td.trade_id = ev.sequence_id;
                td.symbol = ev.symbol;
                td.direction = ev.direction;
                td.offset = ev.offset;
                td.price = ev.price;
                td.volume = ev.volume;
                td.commission = ev.fee;
                td.trade_time_us = ev.timestamp_us;
                pos_mgr.on_trade(td);
                break;
            }
            default:
                break;
        }
    }

    return pos_mgr.get_account();
}

uint64_t EventSourcingReplayer::calculate_audit_checksum() const {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit init
    for (const auto& ev : event_log_) {
        hash ^= ev.sequence_id;
        hash *= 1099511628211ULL;
        hash ^= static_cast<uint8_t>(ev.event_type);
        hash *= 1099511628211ULL;
        uint64_t price_bits = 0;
        std::memcpy(&price_bits, &ev.price, sizeof(double));
        hash ^= price_bits;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace kun
