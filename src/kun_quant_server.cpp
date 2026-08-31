/**
 * @file kun_quant_server.cpp
 * @brief 鲲量化 (KunQuant) 生产级原生 C++ 守护进程服务 (System-Service Daemon)
 *
 * 特性：
 *   1. 内置纯 C/C++ 高性能 HTTP/WebSocket 服务器 (零 Python 依赖)
 *   2. 挂载 tools/kunboard 静态前端，提供 /ws/quant 实时双向通信
 *   3. 桥接 FlowEngine MessageBus 与 flowcoro 协程运行时
 *   4. 生产级 POSIX 信号安全 (SIGPIPE 忽略, SIGTERM 优雅退出)
 */

#include "kun_quant_flowcoro.h"
#include "kun/core/logger.hpp"
#include "kun/core/config_loader.hpp"
#include "kun/engine/multi_account_router.hpp"
#include "kun/engine/gateway_pool.hpp"
#include "kun/engine/storage_manager.hpp"
#include "kun/engine/reconciler.hpp"
#include "kun/market/market_fusion_node.hpp"
#include "kun/market/sina_fetcher.hpp"
#include "kun/market/secondary_fetcher.hpp"
#include "kun/market/market_heartbeat_watchdog.hpp"
#include "kun/backtest/performance.hpp"
#include "kun/cellular/ecosystem_biosphere.hpp"
#include "cJSON.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <unordered_map>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace kun {

static std::atomic<bool> g_server_running{true};
static StorageManager* g_storage_mgr = nullptr;
static GatewayPool* g_live_pool = nullptr; // 融合真值 → 网关池喂单 (实盘闭环)

// ── 历史数据快速回放 (把真实落盘 tick 按 N 倍速重新灌入总线) ──
// 数据 100% 来自 ticks 表真实行情, 仅时间轴加速, 不生成任何合成价格。
// exchange 标记 "REPLAY", 行情落盘协程据此跳过, 防止回放数据污染真实 tick 账本。
static std::atomic<bool> g_replay_running{false};
static std::string g_replay_symbol;
static std::atomic<uint64_t> g_replay_pos{0};
static std::atomic<uint64_t> g_replay_total{0};
static MessageBus* g_replay_bus = nullptr;
static std::thread g_replay_thread;

static void replay_worker(std::string sym, double speed) {
    auto ticks = g_storage_mgr ? g_storage_mgr->load_ticks(sym, 100000) : std::vector<StorageManager::TickRow>{};
    if (ticks.size() < 2) { g_replay_running.store(false); return; }

    uint64_t avg_dt_us = static_cast<uint64_t>(
        (ticks.back().ts - ticks.front().ts) / (ticks.size() - 1));
    if (avg_dt_us == 0) avg_dt_us = 500000;
    uint64_t send_interval_us = std::max<uint64_t>(20000,  // 上限 50 tick/s 防总线过载
        static_cast<uint64_t>(avg_dt_us / std::max(1.0, speed)));

    g_replay_total.store(ticks.size());
    g_replay_pos.store(0);
    for (size_t i = 0; i < ticks.size() && g_replay_running.load() && g_server_running.load(); ++i) {
        const auto& t = ticks[i];
        QuantTickMsg msg{};
        std::strncpy(msg.symbol, t.symbol.c_str(), sizeof(msg.symbol) - 1);
        std::strncpy(msg.exchange, "REPLAY", sizeof(msg.exchange) - 1);
        msg.timestamp_us = static_cast<uint64_t>(t.ts);
        msg.last_price = t.last_price;
        msg.bid_price1 = t.bid1;
        msg.ask_price1 = t.ask1;
        msg.bid_volume1 = t.bid_vol1;
        msg.ask_volume1 = t.ask_vol1;
        msg.volume = t.volume;
        msg.open_interest = t.open_interest;
        std::string topic = "market/tick/" + sym;
        message_bus_publish(g_replay_bus, topic.c_str(), "HistoryReplay", &msg, sizeof(msg));
        g_replay_pos.store(i + 1);
        std::this_thread::sleep_for(std::chrono::microseconds(send_interval_us));
    }
    g_replay_running.store(false);
}

static void sig_handler(int sig) {
    std::cout << "[KunQuant Daemon] Signal received: " << sig << std::endl;
    g_server_running.store(false);
}

class NativeHttpWsServer {
public:
    NativeHttpWsServer(int port, std::string static_dir, MessageBus* bus, StorageManager* storage, int ai_port = 8901)
        : port_(port), static_dir_(std::move(static_dir)), bus_(bus), storage_(storage), ai_port_(ai_port) {}

    ~NativeHttpWsServer() {
        stop();
    }

    bool start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "[Server] Failed to create socket\n";
            return false;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[Server] Failed to bind to port " << port_ << "\n";
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        if (listen(server_fd_, 128) < 0) {
            std::cerr << "[Server] Failed to listen\n";
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        std::cout << "[INFO ] [Server] KunQuant Native Daemon listening on http://0.0.0.0:" << port_ << "\n";
        return true;
    }

    void handle_connections() {
        while (g_server_running.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd_, &read_fds);

            timeval tv{0, 200000};
            int ret = select(server_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) continue;

            handle_client(client_fd);
        }
    }

    void stop() {
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    void handle_client(int client_fd) {
        // 设置 500ms 超时，防止客户端半开连接/挂起请求阻塞主服务线程
        struct timeval tv{0, 500000};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        char buffer[4096];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            close(client_fd);
            return;
        }
        buffer[bytes] = '\0';

        std::string request(buffer);
        size_t method_end = request.find(' ');
        if (method_end == std::string::npos) {
            close(client_fd);
            return;
        }
        size_t url_end = request.find(' ', method_end + 1);
        if (url_end == std::string::npos) {
            close(client_fd);
            return;
        }

        std::string path = request.substr(method_end + 1, url_end - method_end - 1);
        if (path == "/" || path.empty()) {
            path = "/index.html";
        }

        // 安全检查：防止 ../ 路径穿越攻击
        if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
            std::string forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
            send(client_fd, forbidden.c_str(), forbidden.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        // 处理 API 接口请求
        if (path == "/api/status") {
            std::string json = "{\"status\":\"RUNNING\",\"server\":\"KunQuant Native Daemon\",\"flowcoro\":\"ONLINE\",\"storage\":\"SQLITE_WAL\",\"multi_account\":3}";
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path == "/api/cellular/organism" || path == "/api/cellular/champion") {
            auto org = kun::CellularOrganism::create_seed_organism(888);
            org.step_force_field_physics(0.016f);
            std::string json = org.to_json();
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path == "/api/biosphere/status" || path == "/api/biosphere") {
            static kun::EcoBiosphere global_biosphere(8, 12345);
            global_biosphere.step_ecosystem(1.0);
            std::string json = global_biosphere.to_json();
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path.rfind("/api/report", 0) == 0) {
            // 真实绩效分析: 全部指标与曲线均来自 SQLite 成交账本, 严禁任何虚构数据
            // 支持统计周期过滤: /api/report?range=24h|7d|30d|all
            std::string range = "all";
            size_t q = path.find("range=");
            if (q != std::string::npos) {
                range = path.substr(q + 6);
                size_t amp = range.find('&');
                if (amp != std::string::npos) range.resize(amp);
            }
            std::string json = build_report_json(range);
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path.rfind("/api/bars", 0) == 0) {
            // 真实 K 线数据, 严禁 sin()/随机数合成:
            //   tf=1d          → data/history/{品种}.csv (fetch_history.py 抓取的新浪日线)
            //   tf=1m/5m/15m   → SQLite ticks 表真实落盘行情聚合 (OHLCV)
            // 无数据时返回空数组, 前端如实显示空态。
            std::string sym = "rb2405";
            std::string tf = "1d";
            size_t q = path.find("symbol=");
            if (q != std::string::npos) {
                sym = path.substr(q + 7);
                size_t amp = sym.find('&');
                if (amp != std::string::npos) sym.resize(amp);
            }
            q = path.find("tf=");
            if (q != std::string::npos) {
                tf = path.substr(q + 3);
                size_t amp = tf.find('&');
                if (amp != std::string::npos) tf.resize(amp);
            }

            auto emit_json = [&](const std::vector<std::string>& times,
                                 const std::vector<double>& opens, const std::vector<double>& highs,
                                 const std::vector<double>& lows, const std::vector<double>& closes,
                                 const std::vector<double>& vols) {
                constexpr size_t kMaxChartBars = 120; // 图表只取最近 N 根
                size_t n = times.size();
                size_t start = n > kMaxChartBars ? n - kMaxChartBars : 0;
                std::ostringstream ss;
                ss << "[";
                for (size_t i = start; i < n; ++i) {
                    if (i > start) ss << ",";
                    ss << "{\"time\":\"" << times[i] << "\",\"open\":" << opens[i]
                       << ",\"high\":" << highs[i] << ",\"low\":" << lows[i]
                       << ",\"close\":" << closes[i] << ",\"volume\":" << vols[i] << "}";
                }
                ss << "]";
                std::string json = ss.str();
                std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                       std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
                send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
                close(client_fd);
            };

            if (tf == "1m" || tf == "5m" || tf == "15m") {
                // 真实落盘 tick → 分钟级 OHLCV 桶聚合。
                // volume 为新浪当日累计量, 桶内取 max-min 增量 (负值防御为 0)。
                int64_t bucket_s = (tf == "1m") ? 60 : (tf == "5m" ? 300 : 900);
                std::vector<StorageManager::TickRow> ticks;
                if (storage_) ticks = storage_->load_ticks(sym, 20000);
                if (!ticks.empty()) {
                    std::vector<std::string> times;
                    std::vector<double> opens, highs, lows, closes, vols;
                    int64_t cur_bucket = 0;
                    double b_open = 0, b_high = -1e18, b_low = 1e18, b_close = 0, b_vol_min = 1e18, b_vol_max = -1e18;
                    auto flush_bucket = [&]() {
                        if (b_high < b_low) return; // 空桶
                        times.push_back([bucket_start = cur_bucket * bucket_s]() {
                            std::time_t tt = static_cast<std::time_t>(bucket_start);
                            std::tm bt{};
                            localtime_r(&tt, &bt);
                            char buf[8];
                            std::strftime(buf, sizeof(buf), "%H:%M", &bt);
                            return std::string(buf);
                        }());
                        opens.push_back(b_open);
                        highs.push_back(b_high);
                        lows.push_back(b_low);
                        closes.push_back(b_close);
                        double dv = b_vol_max - b_vol_min;
                        vols.push_back(dv > 0 ? dv : 0);
                    };
                    for (const auto& t : ticks) {
                        int64_t bucket = t.ts / 1000000 / bucket_s;
                        if (bucket != cur_bucket) {
                            flush_bucket();
                            cur_bucket = bucket;
                            b_open = t.last_price;
                            b_high = -1e18;
                            b_low = 1e18;
                            b_vol_min = 1e18;
                            b_vol_max = -1e18;
                        }
                        b_high = std::max(b_high, t.last_price);
                        b_low = std::min(b_low, t.last_price);
                        b_close = t.last_price;
                        b_vol_min = std::min(b_vol_min, t.volume);
                        b_vol_max = std::max(b_vol_max, t.volume);
                    }
                    flush_bucket();
                    emit_json(times, opens, highs, lows, closes, vols);
                } else {
                    emit_json({}, {}, {}, {}, {}, {});
                }
                return;
            }

            // tf=1d: 真实日线 CSV
            std::string family = sym;
            while (!family.empty() && isdigit(static_cast<unsigned char>(family.back()))) family.pop_back();

            std::vector<std::string> times;
            std::vector<double> opens, highs, lows, closes, vols;
            if (!family.empty()) {
                std::ifstream csv("data/history/" + family + ".csv");
                std::string line;
                std::getline(csv, line); // 表头
                while (std::getline(csv, line)) {
                    if (line.empty()) continue;
                    std::stringstream lss(line);
                    std::string tok;
                    std::vector<std::string> cols;
                    while (std::getline(lss, tok, ',')) cols.push_back(tok);
                    if (cols.size() < 7) continue;
                    try {
                        times.push_back(cols[1]);
                        opens.push_back(std::stod(cols[2]));
                        highs.push_back(std::stod(cols[3]));
                        lows.push_back(std::stod(cols[4]));
                        closes.push_back(std::stod(cols[5]));
                        vols.push_back(std::stod(cols[6]));
                    } catch (...) { continue; } // 跳过脏行, 不伪造补齐
                }
            }
            emit_json(times, opens, highs, lows, closes, vols);
            return;
        }

        if (path.rfind("/api/tick", 0) == 0) {
            // 最新真实行情快照 (新浪落盘 tick), 供前端实时面板轮询
            std::string sym = "rb2405";
            size_t q = path.find("symbol=");
            if (q != std::string::npos) {
                sym = path.substr(q + 7);
                size_t amp = sym.find('&');
                if (amp != std::string::npos) sym.resize(amp);
            }
            std::string json = "{}";
            StorageManager::TickRow t;
            if (storage_ && storage_->load_latest_tick(sym, t)) {
                json = "{\"symbol\":\"" + t.symbol + "\",\"last\":" + std::to_string(t.last_price) +
                       ",\"bid\":" + std::to_string(t.bid1) + ",\"ask\":" + std::to_string(t.ask1) +
                       ",\"bid_vol\":" + std::to_string(t.bid_vol1) + ",\"ask_vol\":" + std::to_string(t.ask_vol1) +
                       ",\"volume\":" + std::to_string(t.volume) +
                       ",\"ts\":" + std::to_string(t.ts) + "}";
            }
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path == "/api/order") {
            // 解析 POST 请求体的 JSON 订单参数 (基于 cJSON 标准库，杜绝截断与类型转换异常)
            size_t body_pos = request.find("\r\n\r\n");
            std::string body = (body_pos != std::string::npos) ? request.substr(body_pos + 4) : "";

            std::string acc_id = "acc_master_simnow";
            std::string sym = "rb2405";
            double price = 0.0;
            double volume = 1.0;
            uint8_t direction = 0; // LONG
            uint8_t offset = 0;    // OPEN

            cJSON* root = cJSON_Parse(body.c_str());
            if (root) {
                cJSON* item = cJSON_GetObjectItem(root, "account_id");
                if (cJSON_IsString(item) && item->valuestring) acc_id = item->valuestring;

                item = cJSON_GetObjectItem(root, "symbol");
                if (cJSON_IsString(item) && item->valuestring) sym = item->valuestring;

                item = cJSON_GetObjectItem(root, "price");
                if (cJSON_IsNumber(item)) price = item->valuedouble;

                item = cJSON_GetObjectItem(root, "volume");
                if (cJSON_IsNumber(item)) volume = item->valuedouble;

                item = cJSON_GetObjectItem(root, "direction");
                if (cJSON_IsString(item) && item->valuestring) {
                    std::string d = item->valuestring;
                    if (d == "SHORT" || d == "SELL" || d == "卖出") direction = 1;
                } else if (cJSON_IsNumber(item)) {
                    direction = static_cast<uint8_t>(item->valueint);
                }

                item = cJSON_GetObjectItem(root, "offset");
                if (cJSON_IsString(item) && item->valuestring) {
                    std::string off = item->valuestring;
                    if (off == "CLOSE" || off == "平仓") offset = 1;
                    else if (off == "CLOSE_TODAY" || off == "平今" || off == "平今仓") offset = 2;
                } else if (cJSON_IsNumber(item)) {
                    offset = static_cast<uint8_t>(item->valueint);
                }

                cJSON_Delete(root);
            }

            QuantOrderReqMsg req{};
            static std::atomic<uint64_t> s_order_req_seq{20000};
            req.order_req_id = s_order_req_seq.fetch_add(1);
            std::strncpy(req.symbol, sym.c_str(), sizeof(req.symbol) - 1);
            std::strncpy(req.exchange, "SHFE", sizeof(req.exchange) - 1);
            std::strncpy(req.strategy_name, "ManualDesk", sizeof(req.strategy_name) - 1);
            req.direction = direction;
            req.offset = offset;
            req.order_type = 0; // LIMIT
            req.price = price;
            req.volume = volume;

            std::string topic = "trader/" + acc_id + "/order_req";
            message_bus_publish(bus_, topic.c_str(), "HttpServerOrderDesk", &req, sizeof(req));

            std::string json = "{\"status\":\"SUBMITTED\",\"order_req_id\":" + std::to_string(req.order_req_id) +
                               ",\"account_id\":\"" + acc_id + "\",\"symbol\":\"" + sym +
                               "\",\"direction\":" + std::to_string((int)direction) +
                               ",\"price\":" + std::to_string(price) +
                               ",\"volume\":" + std::to_string(volume) + "}";
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path == "/api/replay" || path == "/api/replay/stop" || path == "/api/replay/status") {
            // 历史数据快速回放: 把真实落盘 tick 按倍速重新发布至 market/tick/{sym},
            // 策略/进化引擎/风控全链路即刻消费一个完整交易日的真实行情。仅加速时间轴, 无任何合成数据。
            auto send_json = [&](const std::string& json) {
                std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                       std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
                send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
                close(client_fd);
            };

            if (path == "/api/replay/status") {
                std::string json = std::string("{\"running\":") + (g_replay_running.load() ? "true" : "false") +
                                   ",\"symbol\":\"" + g_replay_symbol + "\"" +
                                   ",\"pos\":" + std::to_string(g_replay_pos.load()) +
                                   ",\"total\":" + std::to_string(g_replay_total.load()) + "}";
                send_json(json);
                return;
            }

            if (path == "/api/replay/stop") {
                g_replay_running.store(false);
                send_json("{\"status\":\"STOPPING\"}");
                return;
            }

            // POST /api/replay  {symbol, speed}
            size_t body_pos = request.find("\r\n\r\n");
            std::string body = (body_pos != std::string::npos) ? request.substr(body_pos + 4) : "";
            std::string sym = "rb2405";
            double speed = 600.0;
            cJSON* root = cJSON_Parse(body.c_str());
            if (root) {
                cJSON* item = cJSON_GetObjectItem(root, "symbol");
                if (cJSON_IsString(item) && item->valuestring) sym = item->valuestring;
                item = cJSON_GetObjectItem(root, "speed");
                if (cJSON_IsNumber(item) && item->valuedouble > 0) speed = item->valuedouble;
                cJSON_Delete(root);
            }
            bool expected = false;
            if (!g_replay_running.compare_exchange_strong(expected, true)) {
                send_json("{\"error\":\"REPLAY_ALREADY_RUNNING\"}");
                return;
            }
            g_replay_symbol = sym;
            if (g_replay_thread.joinable()) g_replay_thread.join();
            g_replay_thread = std::thread(replay_worker, sym, speed);
            send_json("{\"status\":\"STARTED\",\"symbol\":\"" + sym + "\",\"speed\":" + std::to_string(speed) + "}");
            return;
        }

        if (path.rfind("/api/trend", 0) == 0) {
            // 趋势分析: 全部基于真实日线 (data/history CSV) 统计计算, 无任何预测性虚构。
            // 输出均线排列/动量/波动率/关键位, 供手动小资金参考; 明示非投资建议。
            std::string sym = "rb2405";
            size_t q = path.find("symbol=");
            if (q != std::string::npos) {
                sym = path.substr(q + 7);
                size_t amp = sym.find('&');
                if (amp != std::string::npos) sym.resize(amp);
            }
            std::string family = sym;
            while (!family.empty() && isdigit(static_cast<unsigned char>(family.back()))) family.pop_back();

            std::vector<double> closes, highs, lows;
            std::string last_date;
            if (!family.empty()) {
                std::ifstream csv("data/history/" + family + ".csv");
                std::string line;
                std::getline(csv, line);
                while (std::getline(csv, line)) {
                    if (line.empty()) continue;
                    std::stringstream lss(line);
                    std::string tok;
                    std::vector<std::string> cols;
                    while (std::getline(lss, tok, ',')) cols.push_back(tok);
                    if (cols.size() < 7) continue;
                    try {
                        last_date = cols[1];
                        closes.push_back(std::stod(cols[5]));
                        highs.push_back(std::stod(cols[3]));
                        lows.push_back(std::stod(cols[4]));
                    } catch (...) { continue; }
                }
            }

            auto ma_of = [&](int n) -> double {
                if (closes.size() < static_cast<size_t>(n)) return 0.0;
                double s = 0;
                for (int i = 0; i < n; ++i) s += closes[closes.size() - 1 - i];
                return s / n;
            };

            std::string json;
            if (closes.size() < 60) {
                json = "{\"ok\":false,\"reason\":\"真实日线数据不足 (60 根), 请先运行 fetch_history.py\"}";
            } else {
                double last = closes.back();
                double ma5 = ma_of(5), ma10 = ma_of(10), ma20 = ma_of(20), ma60 = ma_of(60);
                bool bull = ma5 > ma10 && ma10 > ma20 && ma20 > ma60 && last > ma5;
                bool bear = ma5 < ma10 && ma10 < ma20 && ma20 < ma60 && last < ma5;
                const char* trend = bull ? "BULL" : (bear ? "BEAR" : "RANGE");
                const char* trend_cn = bull ? "多头趋势" : (bear ? "空头趋势" : "震荡市");

                double mom = (closes[closes.size() - 21] > 0)
                    ? (last / closes[closes.size() - 21] - 1.0) * 100.0 : 0.0;
                double hi20 = *std::max_element(highs.end() - 20, highs.end());
                double lo20 = *std::min_element(lows.end() - 20, lows.end());
                double range_pos = (hi20 > lo20) ? (last - lo20) / (hi20 - lo20) * 100.0 : 50.0;

                // 20 日波动率 (日收益率标准差)
                double vol20 = 0.0;
                {
                    std::vector<double> rets;
                    for (size_t i = closes.size() - 20; i < closes.size(); ++i) {
                        if (closes[i - 1] > 0) rets.push_back((closes[i] - closes[i - 1]) / closes[i - 1]);
                    }
                    if (!rets.empty()) {
                        double m = 0;
                        for (double r : rets) m += r;
                        m /= rets.size();
                        double v = 0;
                        for (double r : rets) v += (r - m) * (r - m);
                        vol20 = std::sqrt(v / rets.size()) * 100.0;
                    }
                }

                // 诚实的参考结论 (只陈述统计事实 + 常规应对思路, 不承诺未来)
                std::string suggestion;
                if (bull) {
                    suggestion = "均线多头排列且价在 MA5 上方, 短期动能偏强。常规思路: 趋势跟随为主, 回踩 MA20 企稳可关注, 收盘跌破 MA20 视为趋势转弱信号离场。";
                } else if (bear) {
                    suggestion = "均线空头排列且价在 MA5 下方, 短期动能偏弱。常规思路: 不抄底不扛单, 反弹至 MA20 受压可关注做空, 收盘站上 MA20 视为转强信号离场。";
                } else {
                    suggestion = "均线缠绕无明确方向, 属震荡市。常规思路: 小资金以观望为佳, 或仅在区间上下沿轻仓高抛低吸并严格止损, 不追突破。";
                }

                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2);
                ss << "{\"ok\":true,\"symbol\":\"" << sym << "\",\"last_date\":\"" << last_date
                   << "\",\"last\":" << last
                   << ",\"ma5\":" << ma5 << ",\"ma10\":" << ma10 << ",\"ma20\":" << ma20 << ",\"ma60\":" << ma60
                   << ",\"trend\":\"" << trend << "\",\"trend_cn\":\"" << trend_cn << "\""
                   << ",\"momentum_20d_pct\":" << mom
                   << ",\"volatility_20d_pct\":" << vol20
                   << ",\"high_20d\":" << hi20 << ",\"low_20d\":" << lo20
                   << ",\"range_pos_pct\":" << range_pos
                   << ",\"suggestion\":\"" << suggestion
                   << " (统计截至 " << last_date << ", 数据源: 新浪真实日线)\""
                   << ",\"disclaimer\":\"以上为历史数据统计参考, 不预测未来, 不构成投资建议。期货杠杆高, 小资金试仓务必轻仓+硬止损。\"}";
                json = ss.str();
            }
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path.rfind("/api/universe", 0) == 0) {
            // 合约宇宙: config/universe.json (唯一数据源) + 每品种 CSV 真实收盘价。
            // 前端自选列表由此动态构建, 不再写死合约清单。
            std::ifstream uf("config/universe.json");
            std::string json = "[]";
            if (uf) {
                std::stringstream ubuf;
                ubuf << uf.rdbuf();
                cJSON* root = cJSON_Parse(ubuf.str().c_str());
                cJSON* arr = root ? cJSON_GetObjectItem(root, "universe") : nullptr;
                if (arr && cJSON_IsArray(arr)) {
                    std::ostringstream ss;
                    ss << "[";
                    int n = cJSON_GetArraySize(arr);
                    for (int i = 0; i < n; ++i) {
                        cJSON* e = cJSON_GetArrayItem(arr, i);
                        if (i > 0) ss << ",";
                        auto str_field = [&](const char* k) -> std::string {
                            cJSON* it = cJSON_GetObjectItem(e, k);
                            return (it && it->valuestring) ? std::string(it->valuestring) : "";
                        };
                        auto num_field = [&](const char* k) -> double {
                            cJSON* it = cJSON_GetObjectItem(e, k);
                            return it ? it->valuedouble : 0.0;
                        };
                        std::string symbol = str_field("symbol");
                        // CSV 真实日线收盘价与涨跌幅 (无数据则 last=0, 前端如实显示 --)
                        double last_close = 0, prev_close = 0;
                        std::string date;
                        std::ifstream csv("data/history/" + symbol + ".csv");
                        if (csv) {
                            std::string line;
                            std::getline(csv, line);
                            while (std::getline(csv, line)) {
                                if (line.empty()) continue;
                                std::stringstream lss(line);
                                std::string tok;
                                std::vector<std::string> cols;
                                while (std::getline(lss, tok, ',')) cols.push_back(tok);
                                if (cols.size() < 6) continue;
                                try {
                                    prev_close = last_close;
                                    last_close = std::stod(cols[5]);
                                    date = cols[1];
                                } catch (...) { continue; }
                            }
                        }
                        double chg = (prev_close > 0) ? (last_close / prev_close - 1.0) * 100.0 : 0.0;
                        ss << "{\"symbol\":\"" << symbol << "\",\"name\":\"" << str_field("name")
                           << "\",\"category\":\"" << str_field("category")
                           << "\",\"exchange\":\"" << str_field("exchange")
                           << "\",\"tick\":" << num_field("tick")
                           << ",\"mult\":" << num_field("multiplier")
                           << ",\"last\":" << last_close
                           << ",\"chg\":" << chg
                           << ",\"date\":\"" << date << "\"}";
                    }
                    ss << "]";
                    json = ss.str();
                }
                if (root) cJSON_Delete(root);
            }
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        if (path == "/api/reconcile") {
            // 生成对账报告
            std::vector<TradeData> trades;
            std::vector<PositionData> positions;
            if (storage_) {
                trades = storage_->load_trades("acc_master_simnow");
                positions = storage_->load_positions("acc_master_simnow");
            }
            auto rep = SettlementReconciler::reconcile("acc_master_simnow", "2026-08-30", trades, positions, {}, {}, 0.0, 0.0);
            std::string json = "{\"account_id\":\"acc_master_simnow\",\"matched\":" + std::string(rep.is_matched ? "true" : "false") +
                                ",\"local_trades\":" + std::to_string(trades.size()) +
                                ",\"local_positions\":" + std::to_string(positions.size()) + "}";
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        // 统一 AI 命名空间代理 (机制, 不含策略): /api/ 下除上述 C++ 原生端点外,
        // 全部转发至内部 ai_service (ai_port)。Python 侧增删路由无需改动 C++。
        if (path.rfind("/api/", 0) == 0) {
            proxy_to_ai_service(client_fd, request);
            return;
        }

        std::string file_path = static_dir_ + path;
        std::ifstream file(file_path, std::ios::binary);

        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            std::string content = ss.str();

            std::string content_type = "text/html";
            if (path.ends_with(".css")) content_type = "text/css";
            else if (path.ends_with(".js")) content_type = "application/javascript";
            else if (path.ends_with(".json")) content_type = "application/json";

            std::string response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: " + content_type + "; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(content.size()) + "\r\n"
                "Connection: close\r\n\r\n" + content;

            send(client_fd, response.c_str(), response.size(), MSG_NOSIGNAL);
        } else {
            std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
            send(client_fd, not_found.c_str(), not_found.size(), MSG_NOSIGNAL);
        }

        close(client_fd);
    }

    void proxy_to_ai_service(int client_fd, std::string raw_request) {
        int ai_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ai_fd < 0) {
            std::string err = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 15\r\n\r\nAI Unavailable";
            send(client_fd, err.c_str(), err.size(), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        // 设置 300s 超时 (LLM 深度复盘报告可达 60s+，过短会被掐断导致前端报"AI 服务通信异常")
        timeval tv{300, 0};
        setsockopt(ai_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(ai_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        sockaddr_in ai_addr{};
        ai_addr.sin_family = AF_INET;
        ai_addr.sin_port = htons(ai_port_);
        inet_pton(AF_INET, "127.0.0.1", &ai_addr.sin_addr);

        if (connect(ai_fd, (sockaddr*)&ai_addr, sizeof(ai_addr)) < 0) {
            std::string err = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"AI service offline on 8901\"}";
            send(client_fd, err.c_str(), err.size(), MSG_NOSIGNAL);
            close(ai_fd);
            close(client_fd);
            return;
        }

        // 注入 Connection: close 确保 Python 处理完毕后主动关闭连接
        size_t pos = raw_request.find("\r\n\r\n");
        if (pos != std::string::npos) {
            raw_request.insert(pos, "\r\nConnection: close");
        }

        send(ai_fd, raw_request.c_str(), raw_request.size(), MSG_NOSIGNAL);

        char buf[8192];
        ssize_t n;
        while ((n = recv(ai_fd, buf, sizeof(buf), 0)) > 0) {
            send(client_fd, buf, n, MSG_NOSIGNAL);
        }

        close(ai_fd);
        close(client_fd);
    }

    int port_;
    std::string static_dir_;
    MessageBus* bus_;
    StorageManager* storage_{nullptr};
    int server_fd_{-1};
    int ai_port_{8901};   // 内部 ai_service 端口 (变与不变的分界: C++ 只认端口不认路由)

private:
    /// 从 SQLite 成交账本构建真实绩效报告 (FIFO 开平配对)
    /// range: 24h/7d/30d/all — 按平仓时间过滤统计窗口; 全部指标仅陈述账本事实
    std::string build_report_json(const std::string& range = "all") {
        std::ostringstream ss;
        ss << std::fixed;

        std::vector<TradeData> trades;
        if (storage_) trades = storage_->load_all_trades();

        constexpr double kInitialCapital = 1000000.0;

        // FIFO 开平配对: 每笔平仓成交按配对量计算真实已实现盈亏;
        // 开仓手续费按配对量比例摊入平仓回合 (成本随平仓兑现)
        struct OpenPos { Direction dir; double price; double volume; double commission; };
        std::unordered_map<std::string, std::vector<OpenPos>> open_queues;
        struct ClosedTrip {
            const TradeData* close;
            double pnl;      // 价差毛盈亏
            double comm;     // 摊入本回合的手续费 (开仓比例 + 平仓全额)
        };
        std::vector<ClosedTrip> all_trips;

        for (const auto& t : trades) {
            if (t.offset == Offset::OPEN) {
                open_queues[t.symbol].push_back({t.direction, t.price, t.volume, t.commission});
            } else {
                auto& q = open_queues[t.symbol];
                double to_close = t.volume;
                double pnl = 0.0, comm = t.commission;
                while (to_close > 0.0 && !q.empty()) {
                    auto& op = q.front();
                    double match_vol = std::min(op.volume, to_close);
                    pnl += (op.dir == Direction::LONG)
                        ? (t.price - op.price) * match_vol * 10.0
                        : (op.price - t.price) * match_vol * 10.0;
                    comm += op.commission * (match_vol / std::max(op.volume, 0.0001));
                    op.volume -= match_vol;
                    to_close -= match_vol;
                    if (op.volume <= 0.0001) q.erase(q.begin());
                }
                all_trips.push_back({&t, pnl, comm});
            }
        }

        // 统计窗口过滤 (按平仓时间, 真实时间维度)
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t cutoff_us = 0;
        if (range == "24h") cutoff_us = now_us - 24LL * 3600 * 1000000;
        else if (range == "7d") cutoff_us = now_us - 7LL * 24 * 3600 * 1000000;
        else if (range == "30d") cutoff_us = now_us - 30LL * 24 * 3600 * 1000000;

        std::vector<const ClosedTrip*> trips;
        double total_in_window_comm = 0.0;
        for (const auto& t : trades) {
            bool is_close_leg = (t.offset != Offset::OPEN);
            bool in_window = (cutoff_us <= 0) || (t.trade_time_us >= cutoff_us);
            if (in_window && (cutoff_us <= 0 || is_close_leg)) total_in_window_comm += t.commission;
        }
        for (const auto& trip : all_trips) {
            if (cutoff_us <= 0 || trip.close->trade_time_us >= cutoff_us) trips.push_back(&trip);
        }

        // 净值与累计盈亏曲线: 初始资金 + 累计净盈亏 (毛盈亏 - 摊入手续费)
        std::vector<double> equity_history;
        equity_history.reserve(trips.size() + 1);
        equity_history.push_back(kInitialCapital);
        std::vector<double> pnl_series, dd_series;
        double cum_net = 0.0, peak_equity = kInitialCapital;
        int wins = 0, losses = 0;
        double total_profit = 0.0, total_loss = 0.0;
        for (const auto* trip : trips) {
            double net = trip->pnl - trip->comm;
            cum_net += net;
            double eq = kInitialCapital + cum_net;
            equity_history.push_back(eq);
            pnl_series.push_back(cum_net);
            peak_equity = std::max(peak_equity, eq);
            dd_series.push_back(peak_equity > 0 ? (peak_equity - eq) / peak_equity * 100.0 : 0.0);
            if (net > 0.0) { wins++; total_profit += net; }
            else { losses++; total_loss += std::abs(net); }
        }

        // 指标统计 (全部来自真实回合, 无任何虚构)
        bool has_data = !trips.empty();
        double mdd = 0.0, mdd_pct = 0.0, sharpe = 0.0, profit_factor = 0.0;
        {
            double peak = kInitialCapital;
            std::vector<double> rets;
            rets.reserve(equity_history.size());
            for (size_t i = 1; i < equity_history.size(); ++i) {
                peak = std::max(peak, equity_history[i]);
                double dd = peak - equity_history[i];
                if (peak > 0) mdd_pct = std::max(mdd_pct, dd / peak * 100.0);
                mdd = std::max(mdd, dd);
                if (equity_history[i - 1] > 0)
                    rets.push_back((equity_history[i] - equity_history[i - 1]) / equity_history[i - 1]);
            }
            // 逐回合夏普: mean/std * sqrt(N) (不规则时间序列, 不做年化假装)
            if (rets.size() >= 2) {
                double mean = 0.0;
                for (double r : rets) mean += r;
                mean /= rets.size();
                double var = 0.0;
                for (double r : rets) var += (r - mean) * (r - mean);
                double sd = std::sqrt(var / rets.size());
                if (sd > 1e-12) sharpe = mean / sd * std::sqrt(static_cast<double>(rets.size()));
            }
            profit_factor = (total_loss > 0.0) ? (total_profit / total_loss) : (total_profit > 0 ? 99.9 : 0.0);
        }

        // ── JSON 输出 ──
        ss << "{\"has_data\":" << (has_data ? "true" : "false")
           << ",\"initial_capital\":" << kInitialCapital
           << ",\"metrics\":{\"total_pnl\":" << cum_net
           << ",\"return_rate\":" << ((kInitialCapital + cum_net - kInitialCapital) / kInitialCapital * 100.0)
           << ",\"win_rate\":" << (wins + losses > 0 ? 100.0 * wins / (wins + losses) : 0.0)
           << ",\"win_trades\":" << wins
           << ",\"lose_trades\":" << losses
           << ",\"profit_factor\":" << profit_factor
           << ",\"max_drawdown_pct\":" << mdd_pct
           << ",\"max_drawdown_amt\":" << mdd
           << ",\"sharpe\":" << sharpe
           << ",\"commission\":" << total_in_window_comm
           << ",\"total_trades\":" << trips.size()
           << "},\"pnlSeries\":[";
        for (size_t i = 0; i < pnl_series.size(); ++i) {
            ss << pnl_series[i] << (i + 1 < pnl_series.size() ? "," : "");
        }
        ss << "],\"drawdownSeries\":[";
        for (size_t i = 0; i < dd_series.size(); ++i) {
            ss << dd_series[i] << (i + 1 < dd_series.size() ? "," : "");
        }
        ss << "],\"trades\":[";
        constexpr size_t kMaxRecent = 20;
        size_t start = trips.size() > kMaxRecent ? trips.size() - kMaxRecent : 0;
        bool first = true;
        for (size_t i = start; i < trips.size(); ++i) {
            const auto* trip = trips[i];
            const auto* t = trip->close;
            char time_buf[16]{};
            std::time_t secs = static_cast<std::time_t>(t->trade_time_us / 1000000);
            std::tm bt{};
            localtime_r(&secs, &bt);
            std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", bt.tm_hour, bt.tm_min, bt.tm_sec);
            if (!first) ss << ",";
            first = false;
            ss << "{\"id\":\"T" << t->trade_id << "\",\"time\":\"" << time_buf
               << "\",\"symbol\":\"" << t->symbol << "\",\"dir\":\""
               << (t->direction == Direction::LONG ? "买入" : "卖出") << "\",\"offset\":\""
               << (t->offset == Offset::OPEN ? "开仓" : "平仓") << "\",\"price\":"
               << t->price << ",\"vol\":" << t->volume
               << ",\"pnl\":" << (trip->pnl - trip->comm) << ",\"cumPnl\":" << pnl_series[i]
               << ",\"strat\":\"" << (t->strategy_name.empty() ? "manual" : t->strategy_name) << "\"}";
        }
        ss << "]}";
        return ss.str();
    }
};

} // namespace kun

int main(int argc, char* argv[]) {
    // 忽略终端作业控制与挂断信号，防止后台守护运行时意外退出
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);
    std::signal(SIGTTOU, SIG_IGN);
    std::signal(SIGTSTP, SIG_IGN);
    std::signal(SIGINT, kun::sig_handler);
    std::signal(SIGTERM, kun::sig_handler);
    std::signal(SIGQUIT, kun::sig_handler);

    // 0. 加载全局配置文件
    kun::QuantAppConfig cfg;
    kun::ConfigLoader::load_from_file("kun_quant/config/quant_config.json", cfg);

    int port = cfg.server.port;
    std::string static_dir = cfg.server.static_dir;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (std::string(argv[i]) == "--static-dir" && i + 1 < argc) {
            static_dir = argv[++i];
        } else if (std::string(argv[i]) == "--db-path" && i + 1 < argc) {
            cfg.server.db_path = argv[++i];
        } else if (std::string(argv[i]) == "--ai-port" && i + 1 < argc) {
            cfg.server.ai_port = std::stoi(argv[++i]);
        }
    }

    std::cout << "\n=======================================================\n";
    std::cout << "     鲲量化 (KunQuant Daemon) 生产级原生服务启动        \n";
    std::cout << "     服务端口: " << port << " | 静态资源: " << static_dir << "\n";
    std::cout << "     持久化库: " << cfg.server.db_path << "\n";
    std::cout << "=======================================================\n" << std::endl;

    // 1. 初始化 SQLite WAL 账务持久化管理器
    kun::StorageManager storage(cfg.server.db_path);
    storage.init();
    kun::g_storage_mgr = &storage;

    // 2. 创建 KunAutoDrive 核心消息总线
    MessageBus* bus = message_bus_create("kunquant_daemon_bus");
    kun::g_replay_bus = bus;

    // 3. 启动 flowcoro 协程运行时
    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex;

    // 4. 挂载多账户路由器与网关池 (深度复用 MessageBus 命名空间)
    kun::MultiAccountRouter router;
    kun::GatewayPool pool(bus, &storage);

    for (const auto& acc : cfg.accounts) {
        router.add_account(acc);
        pool.register_account(acc);
        // 注入全部合约参数 (乘数/保证金率), 保证逐持仓记账精确
        for (const auto& s : cfg.symbols) {
            pool.set_symbol_info(acc.account_id,
                kun::SymbolInfo{s.symbol, s.exchange, s.multiplier, s.price_tick,
                                 s.margin_ratio, s.commission_ratio, s.sina_code});
        }
    }
    pool.connect_all();
    pool.disable_sim_quotes(); // 已接入真实行情, 关闭网关内部随机报价
    kun::g_live_pool = &pool;

    // 协程任务对象池：确保协程运行期间 Task 对象生命周期常驻
    std::vector<std::unique_ptr<CoroutineTask>> spawned_tasks;

    // 5. 挂载多账户主从跟单协程
    auto follow_task = std::make_unique<kun::CoroFollowTradingTask>(
        bus, "acc_master_simnow",
        std::vector<kun::CoroFollowTradingTask::SlaveConfig>{
            // A/B 期间 zhongxin 账户专供 5m 策略独立测试, 暂不参与跟单
            // {"acc_slave_zhongxin", 1.5},
            {"acc_slave_yongan", 0.8}
        }
    );
    ex.spawn(follow_task->run(), "follow_trading_engine");
    spawned_tasks.push_back(std::move(follow_task));

    // 5. 挂载多源行情采集与数据融合协程任务 (原生 CoroutineTask + flowcoro 调度)
    // 每个配置合约一条融合协程, 各自发布 market/tick/{symbol} 真值流
    std::cout << "[KunQuant Daemon] 启动多源行情感知与数据融合协程 (CoroMarketFusionTask)...\n";
    std::vector<std::string> watch_symbols;
    for (const auto& s : cfg.symbols) watch_symbols.push_back(s.symbol);
    for (const auto& sym : watch_symbols) {
        auto fusion_task = std::make_unique<kun::CoroMarketFusionTask>(bus, sym, 0.03);
        ex.spawn(fusion_task->run(), "market_fusion_" + sym);
        spawned_tasks.push_back(std::move(fusion_task));
    }

    // 5.0.4 初始化行情断流看门狗 (MarketHeartbeatWatchdog)
    kun::MarketHeartbeatWatchdog watchdog(3000); // 3 秒断流门限

    // 5.0.5 订阅融合真值流 → 喂给网关池: 真实价格撮合挂单 + 逐 tick 盯市盈亏 + 刷新看门狗心跳
    for (const auto& sym : watch_symbols) {
        std::string topic = "market/tick/" + sym;
        message_bus_subscribe(bus, topic.c_str(), [](const Message* msg, void* ud) {
            if (!msg || msg->data_size < sizeof(kun::QuantTickMsg) || !kun::g_live_pool) return;
            const auto* t = reinterpret_cast<const kun::QuantTickMsg*>(msg->data);
            auto* wd = reinterpret_cast<kun::MarketHeartbeatWatchdog*>(ud);
            if (wd) wd->on_tick_received(t->symbol);

            kun::TickData td;
            td.symbol = t->symbol;
            td.exchange = t->exchange;
            td.last_price = t->last_price;
            td.bid_price[0] = t->bid_price1;
            td.ask_price[0] = t->ask_price1;
            td.bid_volume[0] = t->bid_volume1;
            td.ask_volume[0] = t->ask_volume1;
            td.volume = t->volume;
            td.open_interest = t->open_interest;
            kun::g_live_pool->feed_real_tick(td);
        }, &watchdog);
    }

    // 5.0.6 挂载实盘自动策略 (真实行情驱动, 事前风控把关):
    //   主账户: 5 分钟级双均线 (MA 5/20 + ATR 死区 + 趋势过滤)
    //   从账户 sim1: 5 分钟级双均线 (MA 10/30 稳健型参数)
    for (const auto& s : cfg.symbols) {
        auto strat = std::make_unique<kun::CoroLiveDualMA5mTask>(
            bus, s.symbol, "acc_master_simnow", 5, 20, 1.0);
        ex.spawn(strat->run(), "live_dualma5m_master_" + s.symbol);
        spawned_tasks.push_back(std::move(strat));

        auto strat5m = std::make_unique<kun::CoroLiveDualMA5mTask>(
            bus, s.symbol, "acc_slave_sim1", 10, 30, 1.0);
        ex.spawn(strat5m->run(), "live_dualma5m_slave_" + s.symbol);
        spawned_tasks.push_back(std::move(strat5m));
    }

    // 5.1 挂载行情落盘协程 (M3 行情侧): 为每个监控合约分配独立落盘协程，互不阻塞
    for (const auto& s : cfg.symbols) {
        auto tick_recorder = std::make_unique<kun::CoroSingleTickRecorderTask>(bus, s.symbol, &storage);
        ex.spawn(tick_recorder->run(), "tick_recorder_" + s.symbol);
        spawned_tasks.push_back(std::move(tick_recorder));
    }

    // 启动新浪主行情源 + 备用第二行情源 (双源常明烽燧)
    std::cout << "[KunQuant Daemon] 启动新浪真实行情感知节点 (SinaMarketFetcher)...\n";
    std::vector<std::pair<std::string, std::string>> symbol_codes;
    for (const auto& s : cfg.symbols) symbol_codes.emplace_back(s.symbol, s.sina_code);
    kun::SinaMarketFetcher sina_fetcher(bus, symbol_codes, 1000);
    sina_fetcher.start();

    std::cout << "[KunQuant Daemon] 启动第二备用行情感知节点 (SecondaryMarketFetcher)...\n";
    kun::SecondaryMarketFetcher secondary_fetcher(bus, symbol_codes, 1000);
    secondary_fetcher.start();

    // 6. 挂载 AI 自适应进化协程引擎
    std::string main_sym = cfg.symbols.empty() ? "rb2405" : cfg.symbols[0].symbol;
    auto evolution_task = std::make_unique<kun::CoroAdaptiveEvolutionTask>(bus, main_sym.c_str(), 20);
    ex.spawn(evolution_task->run(), "ai_adaptive_evolution");
    spawned_tasks.push_back(std::move(evolution_task));

    // 7. 挂载原生 HTTP 守护服务
    kun::NativeHttpWsServer server(port, static_dir, bus, &storage, cfg.server.ai_port);
    if (!server.start()) {
        std::cerr << "[KunQuant Daemon] Failed to start native daemon.\n";
        return 1;
    }

    // 启动服务端监听线程
    std::thread server_thread([&server]() {
        server.handle_connections();
    });

    // 主协程驱动循环 (node_pump)
    while (kun::g_server_running.load()) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    // 优雅停机
    std::cout << "[KunQuant Daemon] Shutting down workers and closing sockets...\n";
    sina_fetcher.stop();
    secondary_fetcher.stop();
    server.stop();

    if (server_thread.joinable()) server_thread.join();

    for (auto& t : spawned_tasks) {
        if (t) t->set_stop();
    }

    ex.request_stop();
    int grace_ms = 0;
    while (!ex.is_finished() && grace_ms++ < 100) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 关键生命周期契约：先断开网关连接并清空全局指针，确保在 message_bus 销毁前完成资源解绑
    kun::g_live_pool = nullptr;
    pool.disconnect_all();

    // 关键契约: 在 message_bus 销毁前清空所有任务，确保 BusChannel 析构反订阅时总线依然有效
    spawned_tasks.clear();
    g_node_exec = nullptr;
    message_bus_destroy(bus);

    std::cout << "[KunQuant Daemon] Service terminated cleanly." << std::endl;
    return 0;
}
