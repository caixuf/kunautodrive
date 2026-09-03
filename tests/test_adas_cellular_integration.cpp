#include <iostream>
#include <iomanip>
#include <cassert>
#include "adas_scenario_suite.hpp"
#include "sdsc_cortex.h"
#include "sdsc_apex_cortex.h"

using namespace kun;
using namespace kun::adas_test;

int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "  FlowEngine × KunCellular 6-Scenario Closed-Loop Dynamics Test" << std::endl;
    std::cout << "  (Bicycle Kinematics, 2DOF Dynamics, Microsecond Determinism)" << std::endl;
    std::cout << "==================================================================" << std::endl;

    // 1. 初始化 ADAS 先天祖先有机体 (ADAS_PROGENITOR)
    auto organism = CellularOrganism::create_by_mode(SeedInitMode::ADAS_PROGENITOR, 1001, 42);
    organism.compile();

    std::cout << "  - Organism ID: " << organism.organism_id << std::endl;
    std::cout << "  - Initial Cells: " << organism.cells.size() << std::endl;
    std::cout << "  - Initial Synapses: " << organism.synapses.size() << std::endl;

    // 2. 李雅普诺夫稳定性与免疫通路验证
    auto lyapunov = organism.check_lyapunov_stability();
    std::cout << "  - Lyapunov Stability: " << (lyapunov.is_stable ? "STABLE" : "UNSTABLE")
              << " (Max Gain: " << lyapunov.max_loop_gain << ")" << std::endl;
    assert(lyapunov.is_stable && "Progenitor must be Lyapunov BIBO stable!");

    bool immune_valid = organism.verify_immune_connectivity();
    std::cout << "  - Immune Path to ACT_IMMUNE_BLOCK: " << (immune_valid ? "CONNECTED" : "SEVERED") << std::endl;
    assert(immune_valid && "Immune path must be valid!");

    // 3. 运行 6 大确定性工况闭环评估
    std::cout << "\n[Running 6-Scenario Closed-Loop Simulation]..." << std::endl;
    auto fitness = evaluate_adas_fitness(organism);

    const char* scenario_names[6] = {
        "1. Curve Tracking (S-Bend)",
        "2. Cut-in AEB Hazard",
        "3. Lane Change",
        "4. Stop & Go (Traffic Wave)",
        "5. Ramp Merge",
        "6. Obstacle Swerve"
    };

    std::cout << "------------------------------------------------------------------" << std::endl;
    std::cout << std::left << std::setw(30) << "Scenario"
              << std::setw(12) << "Status"
              << std::setw(15) << "Key Metric"
              << "Mean Latency (ns)" << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    for (int i = 0; i < 6; ++i) {
        std::cout << std::left << std::setw(30) << scenario_names[i]
                  << std::setw(12) << (fitness.passed[i] ? "PASS" : "FAIL")
                  << std::setw(15) << std::fixed << std::setprecision(4) << fitness.metric[i]
                  << std::fixed << std::setprecision(1) << fitness.latency_mean_ns[i] << " ns"
                  << std::endl;
    }
    std::cout << "------------------------------------------------------------------" << std::endl;
    std::cout << "  Overall Score: " << fitness.score << std::endl;
    std::cout << "  Mean Latency across scenarios: " << fitness.latency_ns << " ns" << std::endl;
    std::cout << "  All 6 Scenarios Passed: " << (fitness.all_passed() ? "YES" : "NO") << std::endl;

    assert(fitness.all_passed() && "All 6 scenarios must pass!");

    // 4. 验证 C11 Cortex 零 GC 结构体在前向仿真中的无缝等价性
    std::cout << "\n[Validating Exported Zero-GC C11 Cortex in Kinematics]..." << std::endl;
    SdscCortex cortex;
    sdsc_cortex_init_default_adas(&cortex);
    float in[4] = {8.0f, -10.0f, 0.0f, 0.35f}; // 极危加塞
    float out[4] = {0};
    sdsc_cortex_forward(&cortex, in, out);
    std::cout << "  - C11 Cortex Emergency AEB Response: Accel=" << out[0] 
              << ", Decel=" << out[1] << ", Immune=" << (int)out[3] << std::endl;
    assert((int)out[3] == 1 && "C11 cortex must trigger AEB on extreme hazard!");

    // 5. 验证 5 功能柱 Apex 复杂机动大脑 (无保护左转对向博弈与窄路掉头)
    std::cout << "\n[Validating Apex 5-Column Complex Maneuver C11 Cortex]..." << std::endl;
    SdscApexCortex apex_cortex;
    sdsc_apex_cortex_init(&apex_cortex);

    // 5.1 无保护左转有对向车逼近 (TTC=2.2s) -> 必须减速让行
    float apex_in[6] = {0.0f, 0.0f, 5.0f, 2.2f, 0.08f, 30.0f};
    float apex_out[4] = {0};
    sdsc_apex_cortex_step(&apex_cortex, apex_in, apex_out);
    std::cout << "  - Unprotected Left-Turn Yielding: Steer=" << apex_out[0]
              << ", Accel=" << apex_out[1] << " (Maneuver=" << apex_cortex.active_maneuver << ")" << std::endl;
    assert(apex_cortex.active_maneuver == APEX_MANEUVER_LEFT_TURN);
    assert(apex_out[1] < 0.0f && "Must yield and decelerate for oncoming traffic!");

    // 5.2 极限窄路掉头 -> 必须进入 UTurn 状态并在末端自动挂 R 倒挡
    float uturn_in[6] = {0.0f, 0.0f, 0.0f, 99.0f, 0.25f, 4.0f};
    sdsc_apex_cortex_step(&apex_cortex, uturn_in, apex_out);
    sdsc_apex_cortex_step(&apex_cortex, uturn_in, apex_out);
    std::cout << "  - Multi-Point U-Turn Phase 1: Steer=" << apex_out[0]
              << ", Gear=" << (int)apex_out[2] << " (Reverse R)" << std::endl;
    assert(apex_cortex.active_maneuver == APEX_MANEUVER_UTURN);
    assert(apex_out[2] == -1.0f && "Must engage Reverse R gear during multi-point U-Turn!");
    std::cout << "  - Apex 5-Column Complex Maneuver: VERIFIED" << std::endl;

    std::cout << "\n==================================================================" << std::endl;
    std::cout << "  FLOWENGINE × KUNCELLULAR INTEGRATION VERIFICATION COMPLETE (100%)" << std::endl;
    std::cout << "==================================================================" << std::endl;
    return 0;
}
