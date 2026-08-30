#include "kun/market/sina_fetcher.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <cctype>
#include <unordered_map>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace kun {

// 通用推导规则 (机制): rb2405 → nf_RB0, au2406 → nf_AU0
// 提取品种字母前缀并大写, 追加 "0" 表示主力连续; 特例由配置覆盖, 不在此硬编码。
std::string SinaMarketFetcher::derive_sina_code(const std::string& symbol) {
    std::string prefix;
    for (char c : symbol) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            prefix += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            break;
        }
    }
    if (prefix.empty()) return "nf_" + symbol;
    return "nf_" + prefix + "0";
}

SinaMarketFetcher::SinaMarketFetcher(MessageBus* bus,
                                     std::vector<std::pair<std::string, std::string>> symbol_codes,
                                     int poll_interval_ms)
    : bus_(bus), symbol_codes_(std::move(symbol_codes)), poll_interval_ms_(poll_interval_ms) {
    // 空的 sina_code 用通用规则补齐
    for (auto& [sym, code] : symbol_codes_) {
        if (code.empty()) code = derive_sina_code(sym);
    }
}

SinaMarketFetcher::~SinaMarketFetcher() {
    stop();
}

bool SinaMarketFetcher::start() {
    if (running_.load()) return true;
    running_.store(true);
    worker_thread_ = std::thread(&SinaMarketFetcher::run_loop, this);
    return true;
}

void SinaMarketFetcher::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void SinaMarketFetcher::run_loop() {
    std::string query_param;
    for (size_t i = 0; i < symbol_codes_.size(); ++i) {
        query_param += symbol_codes_[i].second;
        if (i + 1 < symbol_codes_.size()) query_param += ",";
    }

    while (running_.load()) {
        std::string resp = http_get_sina(query_param);
        if (!resp.empty()) {
            parse_and_publish(resp);
        }
        // 细粒度可中断 sleep, 保证 stop() 触发后能在 20ms 内即刻响应退出
        int elapsed_ms = 0;
        while (running_.load() && elapsed_ms < poll_interval_ms_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            elapsed_ms += 20;
        }
    }
}

bool SinaMarketFetcher::fetch_once() {
    std::string query_param;
    for (size_t i = 0; i < symbol_codes_.size(); ++i) {
        query_param += symbol_codes_[i].second;
        if (i + 1 < symbol_codes_.size()) query_param += ",";
    }
    std::string resp = http_get_sina(query_param);
    if (resp.empty()) return false;
    parse_and_publish(resp);
    return true;
}

std::string SinaMarketFetcher::http_get_sina(const std::string& query_symbols) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("hq.sinajs.cn", "80", &hints, &res) != 0 || !res) {
        return "";
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return "";
    }

    // 设置 2 秒超时
    struct timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        return "";
    }
    freeaddrinfo(res);

    std::string req =
        "GET /list=" + query_symbols + " HTTP/1.1\r\n"
        "Host: hq.sinajs.cn\r\n"
        "Referer: https://finance.sina.com.cn\r\n"
        "User-Agent: Mozilla/5.0 (KunQuant Market Fetcher)\r\n"
        "Connection: close\r\n\r\n";

    send(sock, req.c_str(), req.size(), MSG_NOSIGNAL);

    std::string resp;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        resp.append(buffer, bytes_read);
    }
    close(sock);

    // 截取 HTTP Body
    size_t body_pos = resp.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        return resp.substr(body_pos + 4);
    }
    return resp;
}

void SinaMarketFetcher::parse_and_publish(const std::string& response_text) {
    // 反查表: 用自己发出的请求码映射回内部 symbol (机制自洽, 零硬编码)
    std::unordered_map<std::string, std::string> code_to_symbol;
    for (const auto& [sym, code] : symbol_codes_) {
        code_to_symbol[code] = sym;
    }

    std::istringstream stream(response_text);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find("var hq_str_") == std::string::npos) continue;

        // 格式: var hq_str_nf_RB0="名字,时间,开,高,低,昨收,买1,卖1,最新价,昨结,今结,买量,卖量,持仓,成交量,...";
        size_t quote_start = line.find('\"');
        size_t quote_end = line.rfind('\"');
        if (quote_start == std::string::npos || quote_end == std::string::npos || quote_end <= quote_start + 1) {
            continue;
        }

        // 提取新浪代码并反查内部 symbol; 未登记品种直接跳过 (防脏数据)
        std::string raw_code = line.substr(11, quote_start - 12); // e.g. "nf_RB0"
        auto it = code_to_symbol.find(raw_code);
        if (it == code_to_symbol.end()) continue;
        const std::string& symbol = it->second;

        std::string content = line.substr(quote_start + 1, quote_end - quote_start - 1);
        std::vector<std::string> tokens;
        std::stringstream ss(content);
        std::string item;
        while (std::getline(ss, item, ',')) {
            tokens.push_back(item);
        }

        if (tokens.size() < 14) continue;

        try {
            double bid1 = std::stod(tokens[6]);
            double ask1 = std::stod(tokens[7]);
            double latest_p = std::stod(tokens[8]);
            double bid_vol1 = std::stod(tokens[11]);
            double ask_vol1 = std::stod(tokens[12]);
            double oi = std::stod(tokens[13]);
            double vol = tokens.size() > 14 ? std::stod(tokens[14]) : 1000.0;

            if (latest_p <= 0.0) latest_p = (bid1 > 0.0) ? bid1 : ask1;

            QuantTickMsg tick{};
            std::strncpy(tick.symbol, symbol.c_str(), sizeof(tick.symbol) - 1);
            std::strncpy(tick.exchange, "SHFE", sizeof(tick.exchange) - 1);
            tick.last_price = latest_p;
            tick.bid_price1 = bid1;
            tick.ask_price1 = ask1;
            tick.bid_volume1 = bid_vol1;
            tick.ask_volume1 = ask_vol1;
            tick.volume = vol;
            tick.open_interest = oi;
            tick.timestamp_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );

            // 发布至新浪源原始行情管道
            std::string topic = "market/source/sina/" + symbol;
            message_bus_publish(bus_, topic.c_str(), "SinaFetcher", &tick, sizeof(tick));

        } catch (const std::exception&) {
            // 忽略非数字格式解析异常
        }
    }
}

} // namespace kun
