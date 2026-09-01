#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstring>
#include <csignal>
#include <cmath>
#include <random>
#include <memory>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/ecosystem_biosphere.hpp"
#include "kun/cellular/quantum_radiation_field.hpp"
#include "kun/cellular/maze_navigator.hpp"
#include "kun/cellular/island_evolution_grid.hpp"

namespace kun {

// 全局运行时状态与多线程互斥锁
static std::atomic<bool> g_running{true};
static std::mutex g_morph_mutex;
static std::mutex g_biosphere_mutex;
static std::mutex g_quantum_mutex;
static std::mutex g_maze_mutex;

// 核心组件实例
static IslandEvolutionGrid g_island_grid(8, SeedInitMode::HANDCRAFTED_PROGENITOR);
static MorphogeneticEvolutionEngine g_morph_engine(20, 42, SeedInitMode::HANDCRAFTED_PROGENITOR);
static EcoBiosphere g_biosphere(4, 20);
static QuantumRadiationField g_radiation_field;
static MazeEvolutionEngine g_maze_engine(24, 21, 42, SeedInitMode::HANDCRAFTED_PROGENITOR);

static std::atomic<uint64_t> g_total_inferences{0};
static std::atomic<uint64_t> g_total_cosmic_hits{0};
static std::atomic<uint64_t> g_total_tunneling_events{0};

// 零阻塞无锁快照双缓冲 (Atomic Snapshot Cache for Zero-Lock HTTP Reads)
static std::shared_ptr<const std::string> g_snap_champ_json;
static std::shared_ptr<const std::string> g_snap_pop_json;
static std::shared_ptr<const std::string> g_snap_biosphere_json;
static std::shared_ptr<const std::string> g_snap_quantum_json;
static std::shared_ptr<const std::string> g_snap_maze_json;
static std::shared_ptr<const std::string> g_snap_islands_json;

static inline void publish_champ_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_champ_json, p);
}
static inline void publish_pop_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_pop_json, p);
}
static inline void publish_biosphere_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_biosphere_json, p);
}
static inline void publish_quantum_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_quantum_json, p);
}
static inline void publish_maze_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_maze_json, p);
}
static inline void publish_islands_snap(const std::string& str) {
    auto p = std::make_shared<const std::string>(str);
    std::atomic_store(&g_snap_islands_json, p);
}

// MIME 类型解析
static std::string get_mime_type(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".css"))  return "text/css; charset=utf-8";
    if (path.ends_with(".js"))   return "application/javascript; charset=utf-8";
    if (path.ends_with(".json")) return "application/json; charset=utf-8";
    if (path.ends_with(".png"))  return "image/png";
    if (path.ends_with(".svg"))  return "image/svg+xml";
    return "text/plain; charset=utf-8";
}

// 独立的轻量高性能 HTTP 服务器
class CellularHttpServer {
public:
    CellularHttpServer(int port, const std::string& static_dir)
        : port_(port), static_dir_(static_dir), server_fd_(-1) {}

    ~CellularHttpServer() {
        stop();
    }

    bool start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "[DAEMON_ERR] Failed to create TCP socket" << std::endl;
            return false;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "[DAEMON_ERR] Failed to bind port " << port_ << std::endl;
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        if (listen(server_fd_, 128) < 0) {
            std::cerr << "[DAEMON_ERR] Failed to listen on socket" << std::endl;
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        std::cout << "[DAEMON] Morphogenetic Cellular Cybernetics Server listening on http://0.0.0.0:" << port_ << std::endl;
        std::cout << "[DAEMON] Static assets directory: " << static_dir_ << std::endl;
        return true;
    }

    void run() {
        while (g_running) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd_, &read_fds);

            timeval timeout{};
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int activity = select(server_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
            if (activity < 0 && errno != EINTR) break;
            if (activity <= 0) continue;

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd >= 0) {
                std::thread(&CellularHttpServer::handle_client, this, client_fd).detach();
            }
        }
    }

    void stop() {
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    int port_;
    std::string static_dir_;
    int server_fd_;

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
        if (method_end == std::string::npos) { close(client_fd); return; }
        std::string method = request.substr(0, method_end);

        size_t url_end = request.find(' ', method_end + 1);
        if (url_end == std::string::npos) { close(client_fd); return; }

        std::string full_url = request.substr(method_end + 1, url_end - method_end - 1);
        std::string path = full_url;
        std::string query = "";
        size_t q_pos = full_url.find('?');
        if (q_pos != std::string::npos) {
            path = full_url.substr(0, q_pos);
            query = full_url.substr(q_pos + 1);
        }

        if (path == "/" || path.empty()) {
            path = "/cellular.html";
        }

        // 安全检查
        if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
            send_response(client_fd, 403, "text/plain", "Forbidden");
            return;
        }

        // 1. API: /api/status
        if (path == "/api/status") {
            std::ostringstream ss;
            ss << "{"
               << "\"status\":\"RUNNING\","
               << "\"server\":\"FlowEngine Morphogenetic Cellular Daemon\","
               << "\"version\":\"2.0-24primitives\","
               << "\"warp_speed\":\"" << to_string(g_island_grid.get_warp_speed()) << "\","
               << "\"total_inferences\":" << g_total_inferences.load() << ","
               << "\"cosmic_hits\":" << g_total_cosmic_hits.load() << ","
               << "\"quantum_tunneling_events\":" << g_total_tunneling_events.load() << ","
               << "\"morph_population\":20,"
               << "\"total_islands\":8,"
               << "\"primitives_count\":24"
               << "}";
            send_response(client_fd, 200, "application/json; charset=utf-8", ss.str());
            return;
        }

        // 2. API: /api/cellular/organism 或 /api/cellular/champion (零锁快照读取)
        if (path == "/api/cellular/organism" || path == "/api/cellular/champion") {
            auto snap = std::atomic_load(&g_snap_champ_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                std::lock_guard<std::mutex> lk(g_morph_mutex);
                send_response(client_fd, 200, "application/json; charset=utf-8", g_morph_engine.get_champion().to_json());
            }
            return;
        }

        // 3. API: /api/cellular/population (零锁快照读取)
        if (path == "/api/cellular/population") {
            auto snap = std::atomic_load(&g_snap_pop_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                std::lock_guard<std::mutex> lk(g_morph_mutex);
                std::ostringstream ss;
                ss << "{\"population_size\":20,\"organisms\":[";
                const auto& pop = g_morph_engine.population();
                for (size_t i = 0; i < pop.size(); ++i) {
                    if (i > 0) ss << ",";
                    ss << pop[i].to_json();
                }
                ss << "]}";
                send_response(client_fd, 200, "application/json; charset=utf-8", ss.str());
            }
            return;
        }

        // 4. API: /api/islands/status (8 岛超加速演化态势)
        if (path == "/api/islands/status" || path == "/api/islands") {
            auto snap = std::atomic_load(&g_snap_islands_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                send_response(client_fd, 200, "application/json; charset=utf-8", g_island_grid.to_json());
            }
            return;
        }

        // 5. API: /api/control/warp (控制时空曲率演化档位)
        if (path == "/api/control/warp") {
            if (query.find("speed=100x") != std::string::npos) {
                g_island_grid.set_warp_speed(WarpSpeed::WARP_100X);
            } else if (query.find("speed=1000x") != std::string::npos) {
                g_island_grid.set_warp_speed(WarpSpeed::WARP_1000X);
            } else if (query.find("speed=unlimited") != std::string::npos || query.find("speed=max") != std::string::npos) {
                g_island_grid.set_warp_speed(WarpSpeed::WARP_UNLIMITED);
            } else {
                g_island_grid.set_warp_speed(WarpSpeed::REALTIME_1X);
            }
            std::ostringstream ss;
            ss << "{\"status\":\"OK\",\"warp_speed\":\"" << to_string(g_island_grid.get_warp_speed()) << "\"}";
            send_response(client_fd, 200, "application/json; charset=utf-8", ss.str());
            return;
        }

        // 6. API: /api/control/stress (红皇后对抗压力注入)
        if (path == "/api/control/stress") {
            if (query.find("level=low") != std::string::npos) {
                g_island_grid.set_stress_level(AdversarialStressProfile::Level::LOW);
            } else if (query.find("level=medium") != std::string::npos) {
                g_island_grid.set_stress_level(AdversarialStressProfile::Level::MEDIUM);
            } else if (query.find("level=extreme") != std::string::npos) {
                g_island_grid.set_stress_level(AdversarialStressProfile::Level::EXTREME);
            } else {
                g_island_grid.set_stress_level(AdversarialStressProfile::Level::OFF);
            }
            send_response(client_fd, 200, "application/json; charset=utf-8", "{\"status\":\"OK\",\"stress\":\"UPDATED\"}");
            return;
        }

        // 7. API: /api/biosphere/status (零锁快照读取)
        if (path == "/api/biosphere/status" || path == "/api/biosphere") {
            auto snap = std::atomic_load(&g_snap_biosphere_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                std::lock_guard<std::mutex> lk(g_biosphere_mutex);
                send_response(client_fd, 200, "application/json; charset=utf-8", g_biosphere.to_json());
            }
            return;
        }

        // 8. API: /api/quantum/field (零锁快照读取)
        if (path == "/api/quantum/field") {
            auto snap = std::atomic_load(&g_snap_quantum_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                std::lock_guard<std::mutex> lk(g_quantum_mutex);
                send_response(client_fd, 200, "application/json; charset=utf-8", g_radiation_field.to_json());
            }
            return;
        }

        // 9. API: /api/maze/status (零锁快照读取)
        if (path == "/api/maze/status" || path == "/api/maze") {
            auto snap = std::atomic_load(&g_snap_maze_json);
            if (snap) {
                send_response(client_fd, 200, "application/json; charset=utf-8", *snap);
            } else {
                std::lock_guard<std::mutex> lk(g_maze_mutex);
                send_response(client_fd, 200, "application/json; charset=utf-8", g_maze_engine.to_json());
            }
            return;
        }

        // 10. 静态文件分发 (如 /cellular.html)
        std::string file_path = static_dir_ + path;
        std::ifstream file(file_path, std::ios::binary);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            send_response(client_fd, 200, get_mime_type(file_path), ss.str());
            return;
        }

        // 404 Not Found
        send_response(client_fd, 404, "text/plain", "Not Found");
    }

    void send_response(int client_fd, int status_code, const std::string& content_type, const std::string& body) {
        std::string status_msg = (status_code == 200) ? "OK" : ((status_code == 404) ? "Not Found" : "Forbidden");
        std::ostringstream header;
        header << "HTTP/1.1 " << status_code << " " << status_msg << "\r\n"
               << "Content-Type: " << content_type << "\r\n"
               << "Access-Control-Allow-Origin: *\r\n"
               << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
               << "Access-Control-Allow-Headers: Content-Type\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n";
        std::string header_str = header.str();
        send(client_fd, header_str.c_str(), header_str.size(), MSG_NOSIGNAL);
        if (!body.empty()) {
            send(client_fd, body.c_str(), body.size(), MSG_NOSIGNAL);
        }
        close(client_fd);
    }
};

} // namespace kun

// 信号捕获
static void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[DAEMON] Shutting down Morphogenetic Cellular Daemon..." << std::endl;
        kun::g_running = false;
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);

    int port = 8920;
    std::string static_dir = "/home/caixuf/code/FlowEngine/tools/kunboard";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--static-dir" && i + 1 < argc) {
            static_dir = argv[++i];
        }
    }

    std::cout << "======================================================================\n";
    std::cout << "  FlowEngine Morphogenetic Cellular Cybernetics Standalone Daemon     \n";
    std::cout << "  Version: 2.0 (8-Island Hyper-Warp Grid & 24-Primitive Taxonomy)   \n";
    std::cout << "======================================================================\n";

    // 线程 1: 8 岛超加速并发形态发生演化工作池 (Hyper-Warp Multi-Island Workers)
    std::vector<std::thread> island_workers;
    for (size_t i = 0; i < 8; ++i) {
        island_workers.emplace_back([i]() {
            double synthetic_price = 3600.0;
            std::mt19937_64 rng(1337 + i * 997);
            std::normal_distribution<double> step_dist(0.0, 0.8);
            uint64_t local_step = 0;

            while (kun::g_running) {
                synthetic_price += step_dist(rng);
                double dummy_inputs[4] = { synthetic_price, 1000.0, 1.0, 0.05 };

                kun::g_island_grid.step_island(i, dummy_inputs, step_dist(rng));
                kun::g_total_inferences.fetch_add(20, std::memory_order_relaxed);

                // 周期性跨岛大迁徙 (每 100 代)
                if (i == 0 && ++local_step % 100 == 0) {
                    kun::g_island_grid.migrate_elites();
                }

                // 根据 WarpSpeed 动态决定休眠节拍
                auto speed = kun::g_island_grid.get_warp_speed();
                if (speed == kun::WarpSpeed::REALTIME_1X) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // ~50Hz
                } else if (speed == kun::WarpSpeed::WARP_100X) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                } else if (speed == kun::WarpSpeed::WARP_1000X) {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                } else {
                    // WARP_UNLIMITED: 硅基极限全速推进，不让渡任何时钟周期
                    if (local_step % 1000 == 0) {
                        std::this_thread::yield();
                    }
                }
            }
        });
    }

    // 线程 2: 零锁快照定时发布器 (10 Hz 定时更新原子双缓冲快照)
    std::thread snapshot_publisher([]() {
        while (kun::g_running) {
            {
                // 发布 8 岛网格全局冠军与岛屿态势
                auto global_champ = kun::g_island_grid.get_global_champion();
                kun::publish_champ_snap(global_champ.to_json());
                kun::publish_islands_snap(kun::g_island_grid.to_json());
            }
            {
                std::lock_guard<std::mutex> lk(kun::g_biosphere_mutex);
                kun::publish_biosphere_snap(kun::g_biosphere.to_json());
            }
            {
                std::lock_guard<std::mutex> lk(kun::g_quantum_mutex);
                kun::publish_quantum_snap(kun::g_radiation_field.to_json());
            }
            {
                std::lock_guard<std::mutex> lk(kun::g_maze_mutex);
                kun::publish_maze_snap(kun::g_maze_engine.to_json());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10 Hz 快照刷新
        }
    });

    // 线程 3: 宏观生态圈生境演化 (EcoBiosphere, 1Hz)
    std::thread biosphere_thread([]() {
        while (kun::g_running) {
            {
                std::lock_guard<std::mutex> lk(kun::g_biosphere_mutex);
                kun::g_biosphere.step_ecosystem(1.0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });

    // 线程 4: 量子辐射场波函数干涉 (QuantumRadiationField, 20Hz)
    std::thread quantum_thread([]() {
        while (kun::g_running) {
            {
                std::lock_guard<std::mutex> lk_q(kun::g_quantum_mutex);
                kun::g_radiation_field.step(0.05f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // 线程 5: 2D 连续力学迷宫空间自主寻径演化 (Maze Navigator, ~80 步/秒)
    std::thread maze_thread([]() {
        while (kun::g_running) {
            {
                std::lock_guard<std::mutex> lk(kun::g_maze_mutex);
                kun::g_maze_engine.step_simulation();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    });

    // 启动 HTTP 服务
    kun::CellularHttpServer server(port, static_dir);
    if (!server.start()) {
        std::cerr << "[DAEMON_FATAL] Server start failed on port " << port << std::endl;
        kun::g_running = false;
    } else {
        server.run();
    }

    // 等待所有后台工作线程平稳退出
    for (auto& w : island_workers) {
        if (w.joinable()) w.join();
    }
    if (snapshot_publisher.joinable()) snapshot_publisher.join();
    if (biosphere_thread.joinable()) biosphere_thread.join();
    if (quantum_thread.joinable()) quantum_thread.join();
    if (maze_thread.joinable()) maze_thread.join();

    std::cout << "[DAEMON] Morphogenetic Cellular Daemon exited cleanly." << std::endl;
    return 0;
}
