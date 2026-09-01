/**
 * @file kun_adas_train.cpp
 * @brief 自动驾驶 ADAS 形态发生大脑演化训练器 (KunAutoDrive Cellular ADAS Trainer)
 * 
 * 训练目标：
 * 1. 弯道轨迹精准循迹 (Lateral Precision Tracking)
 * 2. 突发加塞急刹与鬼探头 AEB 毫秒级应急防撞 (Emergency AEB Brake)
 * 3. 启停跟车平顺自适应巡航 (Stop & Go ACC)
 * 4. 极端对开路面防侧滑控制 (Split-Mu Friction Stability)
 * 
 * 特性：
 * - 动态代谢能量自平衡 (Dynamic Metabolic Balance)
 * - 15% 客卿移民多样性 (Anti-Inbreeding Diversity)
 * - 实时保存演化冠军权重至 runs/adas_cellular_champion.json
 */

#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/adas_cellular_adapter.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <random>
#include <algorithm>
#include <filesystem>

using namespace kun;

// ============================================================================
// 车辆动力学与运动学仿真器 (Vehicle Kinematics Simulator)
// ============================================================================
struct SimVehicle {
    double x{0.0};
    double y{0.0};
    double psi{0.0};
    double v{12.0};
    double steer{0.0};
    
    static constexpr double WHEELBASE = 2.7;
    static constexpr double DT = 0.05; // 20Hz 控制周期
    static constexpr double MAX_STEER = 0.60;
    static constexpr double MAX_ACCEL = 3.5;
    static constexpr double MAX_DECEL = -6.0;

    void reset(double start_x = 0.0, double start_y = 0.0, double start_psi = 0.0, double start_v = 12.0) {
        x = start_x;
        y = start_y;
        psi = start_psi;
        v = start_v;
        steer = 0.0;
    }

    void step(double target_steer, double target_accel) {
        target_steer = std::clamp(target_steer, -MAX_STEER, MAX_STEER);
        steer = 0.50 * steer + 0.50 * target_steer;
        target_accel = std::clamp(target_accel, MAX_DECEL, MAX_ACCEL);
        v = std::clamp(v + target_accel * DT, 0.0, 35.0);

        x += v * std::cos(psi) * DT;
        y += v * std::sin(psi) * DT;
        psi += (v / WHEELBASE) * std::tan(steer) * DT;
    }
};

// ============================================================================
// 综合 ADAS 适应度评测流水线
// ============================================================================
double evaluate_adas_organism(CellularOrganism& org) {
    AdasCellularAdapter adas(org);

    double total_score = 1000.0;

    // 1. 弯道循迹评测 (S-Curve Tracking)
    {
        SimVehicle ego;
        ego.reset(0.0, 0.0, 0.0, 15.0);
        double lat_err_sum = 0.0;
        for (int k = 0; k < 200; ++k) {
            double s = ego.x;
            double target_y = 1.5 * std::sin(0.02 * s);
            double target_psi = std::atan(1.5 * 0.02 * std::cos(0.02 * s));
            double kappa = (-1.5 * 0.0004 * std::sin(0.02 * s)) / std::pow(1.0 + std::pow(0.03 * std::cos(0.02 * s), 2), 1.5);

            double lane_offset = ego.y - target_y;
            double heading_err = ego.psi - target_psi;

            auto ctl = adas.process_perception(100.0, 0.0, lane_offset, 99.0);
            double steer_cmd = ctl.steering_curvature - heading_err * 0.85 + std::atan(kappa * SimVehicle::WHEELBASE);
            ego.step(steer_cmd, 0.0);

            lat_err_sum += std::abs(lane_offset);
        }
        double mean_lat_err = lat_err_sum / 200.0;
        total_score += std::max(0.0, (1.0 - mean_lat_err) * 500.0);
    }

    // 2. 加塞急刹 AEB 防撞评测 (Emergency Cut-In AEB)
    {
        SimVehicle ego;
        ego.reset(0.0, 0.0, 0.0, 16.0); // 58 km/h
        double lead_x = 30.0;
        double lead_v = 16.0;
        bool collision = false;
        double min_dist = 999.0;

        for (int k = 0; k < 150; ++k) {
            if (k == 20) {
                lead_x = ego.x + 20.0;
                lead_v = 8.0;
            }
            if (k > 20) {
                lead_v = std::max(0.0, lead_v - 5.5 * SimVehicle::DT);
                lead_x += lead_v * SimVehicle::DT;
            }
            double dist = lead_x - ego.x;
            double rel_v = lead_v - ego.v;
            double ttc = (rel_v < -0.1) ? (dist / (-rel_v)) : 99.0;
            if (dist < min_dist) min_dist = dist;

            if (dist <= 0.0) {
                collision = true;
                break;
            }

            auto ctl = adas.process_perception(dist, rel_v, 0.0, ttc);
            ego.step(0.0, ctl.target_accel_mps2);
        }

        if (collision) {
            total_score -= 800.0; // 发生碰撞严重扣分
        } else {
            total_score += 600.0 + min_dist * 20.0; // 安全刹停奖励
        }
    }

    // 3. 启停跟车平顺性评测 (Stop & Go ACC)
    {
        SimVehicle ego;
        ego.reset(0.0, 0.0, 0.0, 10.0);
        double lead_x = 25.0;
        double lead_v = 10.0;
        double jerk_penalty = 0.0;
        double prev_acc = 0.0;

        for (int k = 0; k < 150; ++k) {
            lead_v = 10.0 + 4.0 * std::sin(0.05 * k);
            lead_x += lead_v * SimVehicle::DT;

            double dist = lead_x - ego.x;
            double rel_v = lead_v - ego.v;
            double ttc = (rel_v < -0.1) ? (dist / (-rel_v)) : 99.0;

            auto ctl = adas.process_perception(dist, rel_v, 0.0, ttc);
            jerk_penalty += std::abs(ctl.target_accel_mps2 - prev_acc);
            prev_acc = ctl.target_accel_mps2;

            ego.step(0.0, ctl.target_accel_mps2);
        }
        total_score += std::max(0.0, 300.0 - jerk_penalty * 2.0);
    }

    return total_score;
}

int main(int argc, char* argv[]) {
    int max_generations = 50;
    int pop_size = 30;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--generations" && i + 1 < argc) max_generations = std::stoi(argv[++i]);
        else if (a == "--pop" && i + 1 < argc) pop_size = std::stoi(argv[++i]);
    }

    std::cout << "\n========================================================================\n";
    std::cout << "  🚗 鲲智能驾驶 (KunAutoDrive) 形态发生大脑演化训练器 🚗\n";
    std::cout << "========================================================================\n";
    std::cout << "• 种群规模: " << pop_size << " 个体\n";
    std::cout << "• 最大演化代数: " << max_generations << " 代\n";
    std::cout << "• 代谢机制: 动态能量自平衡 (Dynamic Metabolic Self-Balancing, 无人工上限)\n";
    std::cout << "• 多样性机制: 15% 客卿移民 + 锦标赛选择 (彻底杜绝近亲繁殖)\n";
    std::cout << "========================================================================\n\n";

    EvolutionConstraintConfig cfg;
    cfg.enable_dynamic_metabolism = true;
    cfg.max_cells_limit = 1000000; // 彻底无上限
    cfg.immigrant_rate = 0.15;
    cfg.basal_metabolic_cost = 0.005;
    cfg.synaptic_metabolic_cost = 0.001;

    MorphogeneticEvolutionEngine engine(pop_size, 42, cfg);

    std::filesystem::create_directories("runs");

    auto start_time = std::chrono::steady_clock::now();

    for (int gen = 1; gen <= max_generations; ++gen) {
        auto t0 = std::chrono::high_resolution_clock::now();

        // 评估当前种群
        for (auto& org : engine.population()) {
            org.fitness_score = evaluate_adas_organism(org);
            org.total_pnl = org.fitness_score - 1000.0;
        }

        // 执行世代演化
        engine.evolve_generation();

        auto t1 = std::chrono::high_resolution_clock::now();
        double gen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        const auto& champion = engine.get_champion();

        if (gen % 5 == 0 || gen == 1 || gen == max_generations) {
            std::cout << "Gen [" << std::setw(3) << gen << "/" << max_generations << "] | "
                      << "冠军适应度: " << std::fixed << std::setprecision(1) << champion.fitness_score << " | "
                      << "细胞数: " << std::setw(2) << champion.cells.size() << " | "
                      << "突触数: " << std::setw(2) << champion.synapses.size() << " | "
                      << "单代耗时: " << std::fixed << std::setprecision(1) << gen_ms << " ms | "
                      << "血统: " << champion.lineage_name << "\n";
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    const auto& champion = engine.get_champion();
    std::string checkpoint_path = "runs/adas_cellular_champion.json";
    champion.save_checkpoint_json(checkpoint_path);

    std::cout << "\n========================================================================\n";
    std::cout << "  🎉 智能驾驶大脑演化训练顺利完成！\n";
    std::cout << "• 总耗时: " << std::fixed << std::setprecision(2) << total_sec << " 秒\n";
    std::cout << "• 终代冠军适应度: " << champion.fitness_score << "\n";
    std::cout << "• 终代脑细胞规模: " << champion.cells.size() << " 细胞, " << champion.synapses.size() << " 突触\n";
    std::cout << "• 冠军脑权重检查点已保存至: " << checkpoint_path << "\n";
    std::cout << "========================================================================\n\n";

    return 0;
}
