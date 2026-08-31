#pragma once

#include "kun/core/types.hpp"
#include "kun/market/market_source_base.hpp"
#include "message_bus.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <utility>

namespace kun {

/**
 * @brief 第二备用实时行情抓取器 (SecondaryMarketFetcher / Eastmoney)
 * 采用独立线程与容错套接字，抓取备用行情源并发布至 market/source/secondary/{symbol}
 */
class SecondaryMarketFetcher : public IMarketFetcher {
public:
    SecondaryMarketFetcher(MessageBus* bus,
                           std::vector<std::pair<std::string, std::string>> symbol_codes,
                           int poll_interval_ms = 1000);
    ~SecondaryMarketFetcher() override;

    bool start() override;
    void stop() override;
    bool fetch_once() override;
    std::string get_source_name() const override { return "secondary"; }

    // 手动注入（用于单元测试和回放）
    void inject_raw_tick(const std::string& symbol, double price, double volume, double oi,
                         double bid1, double ask1, double bid_vol1, double ask_vol1);

private:
    void run_loop();
    std::string http_get(const std::string& host, const std::string& path);
    void parse_and_publish(const std::string& raw_resp);

    MessageBus* bus_;
    std::vector<std::pair<std::string, std::string>> symbol_codes_;
    int poll_interval_ms_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

} // namespace kun
