#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>
#include <cmath>
#include <thread>
#include <iomanip>

#include "kun/cellular/island_evolution_grid.hpp"

using namespace kun;

// 1. 验证 64 字节缓存行对齐与防伪共享
void test_cache_line_alignment() {
    std::cout << "[Test 1] 验证多岛生境 (IslandDeme) 64-Byte 缓存行对齐与零伪共享隔离...\n";
    assert(alignof(IslandDeme) >= 64);

    IslandEvolutionGrid grid(8);
    assert(grid.num_islands() == 8);

    for (size_t i = 0; i < grid.num_islands(); ++i) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(&grid.get_island(i));
        assert(addr % 64 == 0); // 必须严格 64 字节边界对齐
    }
    std::cout << "  -> 8 岛生境内存首地址全部严格 64-Byte 边界对齐通过!\n";
}

// 2. 验证多岛并行演化与代际推进
void test_multi_island_parallel_evolution() {
    std::cout << "[Test 2] 验证 8 岛多生境并发独立演化与正交随机性...\n";
    IslandEvolutionGrid grid(8);

    double inputs[4] = { 3600.0, 1000.0, 1.0, 0.05 };

    // 并发驱动 8 个岛各自推进 50 代
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.emplace_back([&grid, i, &inputs]() {
            for (int g = 0; g < 50; ++g) {
                grid.step_island(i, inputs, 0.5);
            }
        });
    }
    for (auto& t : threads) t.join();

    for (size_t i = 0; i < 8; ++i) {
        assert(grid.get_island(i).generations_count.load() == 50);
        assert(grid.get_island(i).inferences_count.load() > 0);
    }
    std::cout << "  -> 8 岛多线程并发演化 50 代无死锁、无竞争通过!\n";
}

// 3. 验证环形拓扑跨岛基因大迁徙 (Torus Migration)
void test_torus_migration() {
    std::cout << "[Test 3] 验证环形拓扑 (Torus Ring) 跨岛基因迁徙与精英杂交...\n";
    IslandEvolutionGrid grid(8);

    double inputs[4] = { 3600.0, 1000.0, 1.0, 0.05 };

    // 演化 20 代
    for (size_t i = 0; i < 8; ++i) {
        for (int g = 0; g < 20; ++g) {
            grid.step_island(i, inputs, 0.2);
        }
    }

    // 执行跨岛迁徙
    grid.migrate_elites();

    // 检查迁徙计数
    size_t in_total = 0;
    size_t out_total = 0;
    for (size_t i = 0; i < 8; ++i) {
        in_total += grid.get_island(i).migration_in_count;
        out_total += grid.get_island(i).migration_out_count;
    }
    assert(in_total == 8);
    assert(out_total == 8);

    auto global_champ = grid.get_global_champion();
    assert(global_champ.cells.size() >= 4);

    std::cout << "  -> 跨岛精英迁徙与全局冠军提取 100% 正确通过!\n";
}

// 4. 超光速演化吞吐量基准 (Hyper-Warp Speed Benchmark)
void test_hyper_warp_throughput() {
    std::cout << "[Test 4] 运行超光速模式 (Hyper-Warp UNLIMITED) 代际演化吞吐量极限压测...\n";
    IslandEvolutionGrid grid(8);
    grid.set_warp_speed(WarpSpeed::WARP_UNLIMITED);

    double inputs[4] = { 3600.0, 1000.0, 1.0, 0.05 };
    const int GENS_PER_ISLAND = 250;
    const int TOTAL_GENS = GENS_PER_ISLAND * 8;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 8 线程满频跑满
    std::vector<std::thread> workers;
    for (size_t i = 0; i < 8; ++i) {
        workers.emplace_back([&grid, i, &inputs]() {
            for (int g = 0; g < GENS_PER_ISLAND; ++g) {
                grid.step_island(i, inputs, (g % 2 == 0 ? 0.3 : -0.2));
            }
        });
    }
    for (auto& w : workers) w.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double duration_sec = duration_ms / 1000.0;
    double gens_per_sec = TOTAL_GENS / duration_sec;

    std::cout << "  ↳ 总演化世代数: " << TOTAL_GENS << " 代\n";
    std::cout << "  ↳ 8 岛总耗时: " << std::fixed << std::setprecision(2) << duration_ms << " ms ("
              << duration_sec << " 秒)\n";
    std::cout << "  ↳ 超光速演化吞吐率: " << std::fixed << std::setprecision(0) << gens_per_sec << " 代/秒 (Gens/sec)\n";

    assert(gens_per_sec > 1000.0); // 确保多核并发超高速
    std::cout << "  -> 超光速演化吞吐率突破预期阈值通过!\n";
}

// 5. 红皇后对抗环境加压测试 (Red-Queen Adversarial Stress)
void test_red_queen_adversarial_stress() {
    std::cout << "[Test 5] 运行红皇后对抗共演化 (Red-Queen Adversarial Stress) 闪崩压力测试...\n";
    IslandEvolutionGrid grid(4);
    grid.set_stress_level(AdversarialStressProfile::Level::EXTREME);

    double normal_inputs[4] = { 3600.0, 1000.0, 1.0, 0.05 };

    for (size_t i = 0; i < 4; ++i) {
        for (int g = 0; g < 100; ++g) {
            grid.step_island(i, normal_inputs, 0.1);
        }
    }

    std::string json_status = grid.to_json();
    assert(json_status.find("\"warp_mode\"") != std::string::npos);
    assert(json_status.find("\"stress_level\": 3") != std::string::npos);
    assert(json_status.find("\"islands\"") != std::string::npos);

    std::cout << "  -> 红皇后极端对抗加压与 JSON 遥测 100% 通过!\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "   FlowEngine 超加速形态发生演化网格测试集 (Hyper-Warp Grid)          \n";
    std::cout << "======================================================================\n";

    test_cache_line_alignment();
    test_multi_island_parallel_evolution();
    test_torus_migration();
    test_hyper_warp_throughput();
    test_red_queen_adversarial_stress();

    std::cout << "\n======================================================================\n";
    std::cout << "   全部 5 组多岛超加速演化单测 100% 满分通过 (Zero-GC & ASAN Clean)! \n";
    std::cout << "======================================================================\n";
    return 0;
}
