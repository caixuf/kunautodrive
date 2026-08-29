#pragma once

#include "kun/core/types.hpp"
#include "message_bus.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>

namespace kun {

/**
 * @brief 新浪期货实时行情采集节点 (SinaMarketFetcher)
 * 采用原生 C/C++ POSIX Socket 发起 HTTP GET 请求，零 Python 外部依赖
 * 毫秒级抓取国内真实商品期货实时 L1 盘口并发布至 market/source/sina/{symbol}
 */
class SinaMarketFetcher {
public:
    SinaMarketFetcher(MessageBus* bus, std::vector<std::string> symbols, int poll_interval_ms = 500);
    ~SinaMarketFetcher();

    bool start();
    void stop();

    // 单次抓取与解析 (供单步调试或单测使用)
    bool fetch_once();

private:
    void run_loop();
    std::string http_get_sina(const std::string& query_symbols);
    void parse_and_publish(const std::string& response_text);

    MessageBus* bus_;
    std::vector<std::string> symbols_;
    int poll_interval_ms_{500};

    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

} // namespace kun
