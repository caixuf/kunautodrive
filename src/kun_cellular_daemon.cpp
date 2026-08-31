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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/ecosystem_biosphere.hpp"
#include "kun/cellular/quantum_radiation_field.hpp"
#include "kun/cellular/maze_navigator.hpp"
#include "kun/cellular/quant_cellular_adapter.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"

namespace kun {

// 全局运行时状态与多线程互斥锁
static std::atomic<bool> g_running{true};
static std::mutex g_morph_mutex;
static std::mutex g_biosphere_mutex;
static std::mutex g_quantum_mutex;
static std::mutex g_maze_mutex;

static MorphogeneticEvolutionEngine g_morph_engine(20, 42, SeedInitMode::HANDCRAFTED_PROGENITOR);
static EcoBiosphere g_biosphere(4, 20);
static QuantumRadiationField g_radiation_field;
static MazeEvolutionEngine g_maze_engine(24, 21, 42, SeedInitMode::HANDCRAFTED_PROGENITOR);

static std::atomic<uint64_t> g_total_inferences{0};
static std::atomic<uint64_t> g_total_cosmic_hits{0};
static std::atomic<uint64_t> g_total_tunneling_events{0};

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

        if (listen(server_fd_, 64) < 0) {
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
        size_t url_end = request.find(' ', method_end + 1);
        if (url_end == std::string::npos) { close(client_fd); return; }

        std::string path = request.substr(method_end + 1, url_end - method_end - 1);
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
               << "\"total_inferences\":" << g_total_inferences.load() << ","
               << "\"cosmic_hits\":" << g_total_cosmic_hits.load() << ","
               << "\"quantum_tunneling_events\":" << g_total_tunneling_events.load() << ","
               << "\"morph_population\":20,"
               << "\"primitives_count\":24"
               << "}";
            send_response(client_fd, 200, "application/json; charset=utf-8", ss.str());
            return;
        }

        // 2. API: /api/cellular/organism 或 /api/cellular/champion
        if (path == "/api/cellular/organism" || path == "/api/cellular/champion") {
            std::lock_guard<std::mutex> lk(g_morph_mutex);
            const auto& champ = g_morph_engine.get_champion();
            send_response(client_fd, 200, "application/json; charset=utf-8", champ.to_json());
            return;
        }

        // 3. API: /api/cellular/population
        if (path == "/api/cellular/population") {
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
            return;
        }

        // 4. API: /api/biosphere/status
        if (path == "/api/biosphere/status" || path == "/api/biosphere") {
            std::lock_guard<std::mutex> lk(g_biosphere_mutex);
            send_response(client_fd, 200, "application/json; charset=utf-8", g_biosphere.to_json());
            return;
        }

        // 5. API: /api/quantum/field
        if (path == "/api/quantum/field") {
            std::lock_guard<std::mutex> lk(g_quantum_mutex);
            send_response(client_fd, 200, "application/json; charset=utf-8", g_radiation_field.to_json());
            return;
        }

        // 6. API: /api/maze/status
        if (path == "/api/maze/status" || path == "/api/maze") {
            std::lock_guard<std::mutex> lk(g_maze_mutex);
            send_response(client_fd, 200, "application/json; charset=utf-8", g_maze_engine.to_json());
            return;
        }

        // 7. API: /api/universe (兼容模拟行情)
        if (path == "/api/universe") {
            static double sim_price = 3620.0;
            static std::mt19937_64 rng(42);
            static std::normal_distribution<double> dist(0.0, 1.2);
            sim_price += dist(rng);
            if (sim_price < 3000.0) sim_price = 3000.0;

            std::ostringstream ss;
            ss << "{\"symbols\":[{\"symbol\":\"rb2405\",\"last_price\":" << sim_price 
               << ",\"volume\":185420,\"bid1\":" << (sim_price - 1.0) 
               << ",\"ask1\":" << (sim_price + 1.0) << ",\"change_pct\":0.45}]}";
            send_response(client_fd, 200, "application/json; charset=utf-8", ss.str());
            return;
        }

        // 8. 静态文件分发 (如 /cellular.html)
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
    std::cout << "  Version: 2.0 (24-Primitive Taxonomy & Force-Field Zero-GC Engine)  \n";
    std::cout << "======================================================================\n";

    // 线程 1: 7x24 形态发生代际进化与力场松弛 (50 Hz 物理 + 每 150 步演化一代)
    std::thread evo_thread([]() {
        uint64_t tick_count = 0;
        double synthetic_price = 3600.0;
        std::mt19937_64 rng(1337);
        std::normal_distribution<double> step_dist(0.0, 0.8);

        while (kun::g_running) {
            synthetic_price += step_dist(rng);
            double dummy_inputs[4] = { synthetic_price, 1000.0, 1.0, 0.05 };

            {
                std::lock_guard<std::mutex> lk(kun::g_morph_mutex);
                for (auto& org : kun::g_morph_engine.population()) {
                    org.step_force_field_physics(0.02f);
                    auto actions = org.forward(dummy_inputs);
                    kun::g_total_inferences.fetch_add(1, std::memory_order_relaxed);

                    double pnl = (actions.positive_action - actions.negative_action) * step_dist(rng) * 100.0;
                    org.fitness_score = std::max(-500.0, org.fitness_score * 0.95 + pnl);
                }

                // 每 150 步 (约 3s) 演化一代
                if (++tick_count % 150 == 0) {
                    kun::g_morph_engine.evolve_generation();

                    const auto& champ = kun::g_morph_engine.get_champion();
                    if (champ.generation % 10 == 0) {
                        std::cout << "[GEN] Generation " << champ.generation
                                  << " | Champion ID=" << champ.organism_id
                                  << " | Cells=" << champ.cells.size()
                                  << " | Synapses=" << champ.synapses.size()
                                  << " | Fitness=" << champ.fitness_score << std::endl;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 50 Hz
        }
    });

    // 线程 2: 宏观生态圈生境演化 (EcoBiosphere, 1Hz)
    std::thread biosphere_thread([]() {
        while (kun::g_running) {
            {
                std::lock_guard<std::mutex> lk(kun::g_biosphere_mutex);
                kun::g_biosphere.step_ecosystem(1.0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });

    // 线程 3: 量子辐射场波函数干涉与粒子轰击 (QuantumRadiationField, 20Hz)
    std::thread quantum_thread([]() {
        while (kun::g_running) {
            {
                std::lock_guard<std::mutex> lk_q(kun::g_quantum_mutex);
                std::lock_guard<std::mutex> lk_m(kun::g_morph_mutex);

                kun::g_radiation_field.step(0.05f);

                for (auto& org : kun::g_morph_engine.population()) {
                    kun::g_radiation_field.irradiate_organism(org, 0.0f, 0.0f, 0.0f, 0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // 线程 4: 2D 连续力学迷宫空间自主寻径演化 (Maze Navigator, ~80 步/秒)
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
    if (evo_thread.joinable()) evo_thread.join();
    if (biosphere_thread.joinable()) biosphere_thread.join();
    if (quantum_thread.joinable()) quantum_thread.join();
    if (maze_thread.joinable()) maze_thread.join();

    std::cout << "[DAEMON] Morphogenetic Cellular Daemon exited cleanly." << std::endl;
    return 0;
}
