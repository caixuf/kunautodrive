#include "kun/market/secondary_fetcher.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace kun {

SecondaryMarketFetcher::SecondaryMarketFetcher(
    MessageBus* bus,
    std::vector<std::pair<std::string, std::string>> symbol_codes,
    int poll_interval_ms
) : bus_(bus), symbol_codes_(std::move(symbol_codes)), poll_interval_ms_(poll_interval_ms) {}

SecondaryMarketFetcher::~SecondaryMarketFetcher() {
    stop();
}

bool SecondaryMarketFetcher::start() {
    if (running_.load()) return true;
    running_.store(true);
    worker_thread_ = std::thread(&SecondaryMarketFetcher::run_loop, this);
    return true;
}

void SecondaryMarketFetcher::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void SecondaryMarketFetcher::inject_raw_tick(
    const std::string& symbol, double price, double volume, double oi,
    double bid1, double ask1, double bid_vol1, double ask_vol1
) {
    QuantTickMsg msg{};
    std::strncpy(msg.symbol, symbol.c_str(), sizeof(msg.symbol) - 1);
    std::strncpy(msg.exchange, "SHFE", sizeof(msg.exchange) - 1);
    msg.last_price = price;
    msg.volume = volume;
    msg.open_interest = oi;
    msg.bid_price1 = bid1;
    msg.ask_price1 = ask1;
    msg.bid_volume1 = bid_vol1;
    msg.ask_volume1 = ask_vol1;
    msg.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    std::string topic = "market/source/secondary/" + symbol;
    message_bus_publish(bus_, topic.c_str(), "SecondaryMarketFetcher", &msg, sizeof(msg));
}

void SecondaryMarketFetcher::run_loop() {
    while (running_.load()) {
        fetch_once();

        int elapsed_ms = 0;
        while (running_.load() && elapsed_ms < poll_interval_ms_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            elapsed_ms += 20;
        }
    }
}

bool SecondaryMarketFetcher::fetch_once() {
    std::string query_param;
    for (size_t i = 0; i < symbol_codes_.size(); ++i) {
        query_param += symbol_codes_[i].second;
        if (i + 1 < symbol_codes_.size()) query_param += ",";
    }
    std::string resp = http_get("hq.sinajs.cn", "/list=" + query_param);
    if (resp.empty()) return false;
    parse_and_publish(resp);
    return true;
}

std::string SecondaryMarketFetcher::http_get(const std::string& host, const std::string& path) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res) {
        return "";
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return "";
    }

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
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Referer: https://finance.sina.com.cn\r\n"
        "User-Agent: Mozilla/5.0 (KunQuant Secondary Fetcher)\r\n"
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

    size_t body_pos = resp.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        return resp.substr(body_pos + 4);
    }
    return "";
}

void SecondaryMarketFetcher::parse_and_publish(const std::string& raw_resp) {
    std::istringstream stream(raw_resp);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        size_t name_start = line.find("var hq_str_");
        size_t name_end = line.find("=");
        if (name_start == std::string::npos || name_end == std::string::npos) continue;

        std::string code = line.substr(name_start + 11, name_end - (name_start + 11));

        std::string symbol;
        for (const auto& [sym, c] : symbol_codes_) {
            if (c == code) {
                symbol = sym;
                break;
            }
        }
        if (symbol.empty()) continue;

        size_t data_start = line.find('\"');
        size_t data_end = line.rfind('\"');
        if (data_start == std::string::npos || data_end == std::string::npos || data_end <= data_start) continue;

        std::string data_str = line.substr(data_start + 1, data_end - data_start - 1);
        std::istringstream ss(data_str);
        std::string token;
        std::vector<std::string> tokens;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        if (tokens.size() < 14) continue;

        try {
            double open_p = std::stod(tokens[2]);
            double high_p = std::stod(tokens[3]);
            double low_p  = std::stod(tokens[4]);
            double last_p = std::stod(tokens[8]);
            if (last_p <= 0.0) last_p = std::stod(tokens[6]);
            if (last_p <= 0.0) last_p = open_p;

            double bid1 = std::stod(tokens[6]);
            double ask1 = std::stod(tokens[7]);
            double bid_vol1 = std::stod(tokens[11]);
            double ask_vol1 = std::stod(tokens[12]);
            double total_vol = std::stod(tokens[13]);
            double total_oi  = (tokens.size() > 14) ? std::stod(tokens[14]) : 0.0;

            if (last_p > 0.0) {
                inject_raw_tick(symbol, last_p, total_vol, total_oi, bid1, ask1, bid_vol1, ask_vol1);
            }
        } catch (...) {}
    }
}

} // namespace kun
