#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <sstream>

namespace kun {

enum class MarketHealthStatus : uint8_t {
    HEALTHY = 0,     // 正常低延迟行情流
    STALE_FREEZE = 1 // 超时断流，进入防御冻结态
};

/**
 * @brief 数据流心跳看门狗 (MarketHeartbeatWatchdog)
 * 实时监测全市场行情流延迟与断流风险；若超过阈值未更新，自动触发策略开仓冻结
 */
class MarketHeartbeatWatchdog {
public:
    explicit MarketHeartbeatWatchdog(uint32_t stale_timeout_ms = 3000)
        : stale_timeout_ms_(stale_timeout_ms) {}

    void on_tick_received(const std::string& symbol, uint64_t timestamp_us = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (timestamp_us == 0) {
            timestamp_us = current_time_us();
        }
        last_tick_time_us_[symbol] = timestamp_us;
    }

    bool is_symbol_healthy(const std::string& symbol, uint64_t now_us = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (now_us == 0) now_us = current_time_us();

        auto it = last_tick_time_us_.find(symbol);
        if (it == last_tick_time_us_.end()) return false;

        uint64_t elapsed_ms = (now_us > it->second) ? (now_us - it->second) / 1000 : 0;
        return elapsed_ms <= stale_timeout_ms_;
    }

    bool is_all_healthy(const std::vector<std::string>& symbols, uint64_t now_us = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (now_us == 0) now_us = current_time_us();

        for (const auto& sym : symbols) {
            auto it = last_tick_time_us_.find(sym);
            if (it == last_tick_time_us_.end()) return false;
            uint64_t elapsed_ms = (now_us > it->second) ? (now_us - it->second) / 1000 : 0;
            if (elapsed_ms > stale_timeout_ms_) return false;
        }
        return true;
    }

    uint64_t get_elapsed_ms(const std::string& symbol, uint64_t now_us = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (now_us == 0) now_us = current_time_us();
        auto it = last_tick_time_us_.find(symbol);
        if (it == last_tick_time_us_.end()) return 9999999;
        return (now_us > it->second) ? (now_us - it->second) / 1000 : 0;
    }

    uint32_t get_stale_timeout_ms() const { return stale_timeout_ms_; }
    void set_stale_timeout_ms(uint32_t ms) { stale_timeout_ms_ = ms; }

private:
    static uint64_t current_time_us() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    mutable std::mutex mutex_;
    uint32_t stale_timeout_ms_{3000};
    std::unordered_map<std::string, uint64_t> last_tick_time_us_;
};

} // namespace kun
