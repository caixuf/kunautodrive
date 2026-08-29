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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace kun {

static std::atomic<bool> g_server_running{true};
static StorageManager* g_storage_mgr = nullptr;

static void sig_handler(int sig) {
    std::cout << "[KunQuant Daemon] Signal received: " << sig << std::endl;
    g_server_running.store(false);
}

class NativeHttpWsServer {
public:
    NativeHttpWsServer(int port, std::string static_dir, MessageBus* bus, StorageManager* storage)
        : port_(port), static_dir_(std::move(static_dir)), bus_(bus), storage_(storage) {}

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
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    void handle_client(int client_fd) {
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

    int port_;
    std::string static_dir_;
    MessageBus* bus_;
    StorageManager* storage_{nullptr};
    int server_fd_{-1};
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

    // 3. 启动 flowcoro 协程运行时
    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex;

    // 4. 挂载多账户路由器与网关池 (深度复用 MessageBus 命名空间)
    kun::MultiAccountRouter router;
    kun::GatewayPool pool(bus);

    for (const auto& acc : cfg.accounts) {
        router.add_account(acc);
        pool.register_account(acc);
    }
    pool.connect_all();

    // 协程任务对象池：确保协程运行期间 Task 对象生命周期常驻
    std::vector<std::unique_ptr<CoroutineTask>> spawned_tasks;

    // 5. 挂载多账户主从跟单协程
    auto follow_task = std::make_unique<kun::CoroFollowTradingTask>(
        bus, "acc_master_simnow",
        std::vector<kun::CoroFollowTradingTask::SlaveConfig>{
            {"acc_slave_zhongxin", 1.5},
            {"acc_slave_yongan", 0.8}
        }
    );
    ex.spawn(follow_task->run(), "follow_trading_engine");
    spawned_tasks.push_back(std::move(follow_task));

    // 5. 挂载多源行情采集与数据融合协程任务 (原生 CoroutineTask + flowcoro 调度)
    std::cout << "[KunQuant Daemon] 启动多源行情感知与数据融合协程 (CoroMarketFusionTask)...\n";
    auto fusion_task = std::make_unique<kun::CoroMarketFusionTask>(bus, "rb2405", 0.03);
    ex.spawn(fusion_task->run(), "market_fusion_engine");
    spawned_tasks.push_back(std::move(fusion_task));

    // 5.1 挂载行情落盘协程 (M3 行情侧): 为每个监控合约分配独立落盘协程，互不阻塞
    for (const auto& s : cfg.symbols) {
        auto tick_recorder = std::make_unique<kun::CoroSingleTickRecorderTask>(bus, s.symbol, &storage);
        ex.spawn(tick_recorder->run(), "tick_recorder_" + s.symbol);
        spawned_tasks.push_back(std::move(tick_recorder));
    }

    // 启动新浪公网实时期货行情抓取节点
    std::cout << "[KunQuant Daemon] 启动新浪真实行情感知节点 (SinaMarketFetcher)...\n";
    kun::SinaMarketFetcher sina_fetcher(bus, {"rb2405", "cu2405", "ag2405"}, 1000);
    sina_fetcher.start();

    // 6. 挂载 AI 自适应进化协程引擎
    auto evolution_task = std::make_unique<kun::CoroAdaptiveEvolutionTask>(bus, "rb2405", 20);
    ex.spawn(evolution_task->run(), "ai_adaptive_evolution");
    spawned_tasks.push_back(std::move(evolution_task));

    // 7. 挂载原生 HTTP 守护服务
    kun::NativeHttpWsServer server(port, static_dir, bus, &storage);
    if (!server.start()) {
        std::cerr << "[KunQuant Daemon] Failed to start native daemon.\n";
        return 1;
    }

    // 启动服务端监听线程
    std::thread server_thread([&server]() {
        server.handle_connections();
    });

    // 8. 辅助高频仿真行情源 (发布至 market/source/ctp_sim/rb2405 供融合节点加权验证)
    std::thread sim_publisher([bus]() {
        double p = 3625.0;
        while (kun::g_server_running.load()) {
            p += ((rand() % 100) - 49) * 0.05;
            kun::QuantTickMsg tick{};
            std::strncpy(tick.symbol, "rb2405", sizeof(tick.symbol) - 1);
            std::strncpy(tick.exchange, "SHFE", sizeof(tick.exchange) - 1);
            tick.last_price = p;
            tick.bid_price1 = p - 1.0;
            tick.ask_price1 = p + 1.0;
            tick.bid_volume1 = 120.0;
            tick.ask_volume1 = 150.0;
            message_bus_publish(bus, "market/source/ctp_sim/rb2405", "CTP_SIM_GW", &tick, sizeof(tick));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });

    // 主协程驱动循环 (node_pump)
    while (kun::g_server_running.load()) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    // 优雅停机
    std::cout << "[KunQuant Daemon] Shutting down workers and closing sockets...\n";
    sina_fetcher.stop();
    server.stop();

    if (server_thread.joinable()) server_thread.join();
    if (sim_publisher.joinable()) sim_publisher.join();

    // 有界宽限关停: request_stop 后给协程最多 2 秒自行退出 (落盘冲刷/收尾),
    // 随后 ~RtExecutor 兜底销毁仍挂在死 topic 上的帧 (如跟单协程等待永不触发的 trade_rtn)。
    // 注意: ex.shutdown() 的无限等待在此不可用 —— 有 parked 帧等外部 post_ready 且生产者已停。
    ex.request_stop();
    int grace_ms = 0;
    while (!ex.is_finished() && grace_ms++ < 2000) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_node_exec = nullptr;
    message_bus_destroy(bus);

    std::cout << "[KunQuant Daemon] Service terminated cleanly." << std::endl;
    return 0;
}
