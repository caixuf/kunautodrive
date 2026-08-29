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
 *
 * 变与不变 (ADR-6): 品种→新浪代码的映射是"策略", 不在本类硬编码。
 * 构造时传入 (symbol, sina_code) 对; sina_code 为空则按通用规则推导:
 *   nf_ + 品种字母前缀大写 + "0"  (如 rb2405 → nf_RB0)
 * 特例通过 quant_config.json 的 "sina_code" 字段覆盖。
 * 响应解析用自己发出的请求码反查, 无需任何硬编码反查表。
 */
class SinaMarketFetcher {
public:
    explicit SinaMarketFetcher(MessageBus* bus,
                               std::vector<std::pair<std::string, std::string>> symbol_codes,
                               int poll_interval_ms = 500);
    ~SinaMarketFetcher();

    bool start();
    void stop();

    // 单次抓取与解析 (供单步调试或单测使用)
    bool fetch_once();

private:
    void run_loop();
    std::string http_get_sina(const std::string& query_symbols);
    void parse_and_publish(const std::string& response_text);
    static std::string derive_sina_code(const std::string& symbol);

    MessageBus* bus_;
    std::vector<std::pair<std::string, std::string>> symbol_codes_; // (symbol, sina_code)
    int poll_interval_ms_{500};

    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

} // namespace kun
