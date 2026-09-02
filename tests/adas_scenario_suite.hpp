#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

#include "kun/cellular/adas_cellular_adapter.hpp"

namespace kun::adas_test {

struct ScenarioPerturbation {
    double speed_scale{1.0};      // 初速缩放 (如 0.90 ~ 1.15)
    double lead_dist_scale{1.0};  // 前车时距/加塞距离缩放 (0.85 ~ 1.15)
    double curve_amp_scale{1.0};  // 弯道幅度/曲率缩放 (0.85 ~ 1.20)
};

struct ScenarioLatency {
    double mean_us{0.0};
    double max_us{0.0};
    size_t samples{0};
    double total_us{0.0};

    void reset() {
        mean_us = 0.0;
        max_us = 0.0;
        samples = 0;
        total_us = 0.0;
    }

    void record(const std::chrono::high_resolution_clock::time_point& start,
                const std::chrono::high_resolution_clock::time_point& finish) {
        const double elapsed =
            std::chrono::duration<double, std::micro>(finish - start).count();
        total_us += elapsed;
        max_us = std::max(max_us, elapsed);
        ++samples;
        mean_us = total_us / static_cast<double>(samples);
    }
};

struct KinematicBicycleVehicle {
    double x{0.0};
    double y{0.0};
    double psi{0.0};
    double v{12.0};
    double steer{0.0};

    static constexpr double WHEELBASE = 2.7;
    static constexpr double DT = 0.05;
    static constexpr double MAX_STEER = 0.60;
    static constexpr double MAX_ACCEL = 3.5;
    static constexpr double MAX_DECEL = -6.0;

    void reset(double start_x = 0.0, double start_y = 0.0,
               double start_psi = 0.0, double start_v = 12.0) {
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

inline bool run_scenario_curve_tracking(AdasCellularAdapter& adas,
                                         double& max_lat_err,
                                         double& mean_lat_err,
                                         ScenarioLatency& latency,
                                         const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, 0.0, 0.0, 15.0 * perturb.speed_scale);
    const int steps = 300;
    std::vector<double> lat_errs;
    lat_errs.reserve(steps);
    latency.reset();

    for (int k = 0; k < steps; ++k) {
        double s = ego.x;
        double amp = 1.5 * perturb.curve_amp_scale;
        double target_y = amp * std::sin(0.02 * s);
        double target_psi = std::atan(amp * 0.02 * std::cos(0.02 * s));
        double kappa = (-amp * 0.0004 * std::sin(0.02 * s)) /
                       std::pow(1.0 + std::pow(amp * 0.02 * std::cos(0.02 * s), 2), 1.5);
        double lane_offset = ego.y - target_y;
        double heading_err = ego.psi - target_psi;

        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(100.0, 0.0, lane_offset, 99.0);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);

        double steer_cmd = ctl.steering_curvature - heading_err * 0.85 +
                           std::atan(kappa * KinematicBicycleVehicle::WHEELBASE);
        ego.step(steer_cmd, 0.0);
        lat_errs.push_back(std::abs(lane_offset));
    }

    max_lat_err = *std::max_element(lat_errs.begin(), lat_errs.end());
    mean_lat_err = 0.0;
    for (double error : lat_errs) mean_lat_err += error;
    mean_lat_err /= static_cast<double>(lat_errs.size());
    return max_lat_err < (0.15 * perturb.curve_amp_scale + 0.02);
}

inline bool run_scenario_emergency_cutin_aeb(AdasCellularAdapter& adas,
                                             double& min_safety_dist,
                                             bool& aeb_triggered,
                                             ScenarioLatency& latency,
                                             const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, 0.0, 0.0, 16.0 * perturb.speed_scale);
    double lead_x = 30.0 * perturb.lead_dist_scale;
    double lead_v = 16.0 * perturb.speed_scale;
    min_safety_dist = 999.0;
    aeb_triggered = false;
    latency.reset();

    for (int k = 0; k < 200; ++k) {
        if (k == 30) {
            lead_x = ego.x + 24.0 * perturb.lead_dist_scale;
            lead_v = 10.0 * perturb.speed_scale;
        }
        if (k > 30) {
            lead_v = std::max(0.0, lead_v - 5.5 * KinematicBicycleVehicle::DT);
            lead_x += lead_v * KinematicBicycleVehicle::DT;
        } else {
            lead_x += lead_v * KinematicBicycleVehicle::DT;
        }

        double dist_to_lead = lead_x - ego.x;
        double rel_v = lead_v - ego.v;
        double ttc = (rel_v < -0.1) ? (dist_to_lead / (-rel_v)) : 99.0;
        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(dist_to_lead, rel_v, 0.0, ttc);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);
        if (ctl.is_aeb_triggered) aeb_triggered = true;
        ego.step(0.0, ctl.target_accel_mps2);
        min_safety_dist = std::min(min_safety_dist, dist_to_lead);
    }
    return aeb_triggered && min_safety_dist > 1.8;
}

inline bool run_scenario_lane_change(AdasCellularAdapter& adas,
                                     double& settle_time_s,
                                     double& overshoot_m,
                                     ScenarioLatency& latency,
                                     const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, 0.0, 0.0, 16.0 * perturb.speed_scale);
    settle_time_s = -1.0;
    double max_y = 0.0;
    latency.reset();

    for (int k = 0; k < 200; ++k) {
        double t = k * KinematicBicycleVehicle::DT;
        double tau = std::clamp(t / 3.0, 0.0, 1.0);
        double target_y = 3.5 * (10.0 * std::pow(tau, 3) -
                                 15.0 * std::pow(tau, 4) +
                                 6.0 * std::pow(tau, 5));
        double target_dy = (3.5 / 3.0) * (30.0 * std::pow(tau, 2) -
                                          60.0 * std::pow(tau, 3) +
                                          30.0 * std::pow(tau, 4));
        double target_psi = std::atan2(target_dy, ego.v);
        double lane_offset = ego.y - target_y;
        double heading_err = ego.psi - target_psi;
        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(100.0, 0.0, lane_offset, 99.0);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);
        ego.step(ctl.steering_curvature - heading_err * 0.85, 0.0);
        max_y = std::max(max_y, ego.y);
        if (std::abs(ego.y - 3.5) < 0.06 &&
            settle_time_s < 0.0 && t > 2.4) {
            settle_time_s = t;
        }
    }
    overshoot_m = std::max(0.0, max_y - 3.5);
    return settle_time_s > 0.0 && overshoot_m < 0.15;
}

inline bool run_scenario_stop_and_go(AdasCellularAdapter& adas,
                                     double& max_gap_err,
                                     ScenarioLatency& latency,
                                     const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, 0.0, 0.0, 8.0 * perturb.speed_scale);
    double lead_x = 23.0 * perturb.lead_dist_scale;
    double lead_v = 8.0 * perturb.speed_scale;
    max_gap_err = 0.0;
    latency.reset();

    for (int k = 0; k < 250; ++k) {
        lead_v = std::clamp((8.0 + 2.5 * std::sin(k * 0.05)) * perturb.speed_scale, 2.5, 14.0);
        lead_x += lead_v * KinematicBicycleVehicle::DT;
        double dist_to_lead = lead_x - ego.x;
        double rel_v = lead_v - ego.v;
        double ttc = (rel_v < -0.1) ? (dist_to_lead / (-rel_v)) : 99.0;
        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(dist_to_lead, rel_v, 0.0, ttc);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);
        ego.step(0.0, ctl.target_accel_mps2);
        if (k > 50) {
            max_gap_err = std::max(max_gap_err,
                                   std::abs(dist_to_lead - 15.0));
        }
    }
    return max_gap_err < 8.5;
}

inline bool run_scenario_ramp_merge(AdasCellularAdapter& adas,
                                    double& merge_speed_mps,
                                    ScenarioLatency& latency,
                                    const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, -3.5, 0.0, 10.0 * perturb.speed_scale);
    latency.reset();
    for (int k = 0; k < 160; ++k) {
        double lane_offset = ego.y;
        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(100.0, 0.0, lane_offset, 99.0);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);
        ego.step(ctl.steering_curvature - ego.psi * 1.35,
                 ctl.target_accel_mps2);
    }
    merge_speed_mps = ego.v;
    return std::abs(ego.y) < 0.10 && merge_speed_mps >= 13.5;
}

inline bool run_scenario_obstacle_swerve(AdasCellularAdapter& adas,
                                         double& min_obs_clearance,
                                         ScenarioLatency& latency,
                                         const ScenarioPerturbation& perturb = {}) {
    KinematicBicycleVehicle ego;
    ego.reset(0.0, 0.0, 0.0, 12.0 * perturb.speed_scale);
    const double obs_x = 70.0 * perturb.lead_dist_scale;
    const double obs_y = 0.0;
    min_obs_clearance = 999.0;
    latency.reset();

    for (int k = 0; k < 200; ++k) {
        double dist_to_obs = obs_x - ego.x;
        double target_y = 0.0;
        if (ego.x >= 30.0 && ego.x < (obs_x + 20.0)) target_y = 2.5;
        double lane_offset = ego.y - target_y;
        auto start = std::chrono::high_resolution_clock::now();
        auto ctl = adas.process_perception(dist_to_obs, 0.0, lane_offset, 99.0);
        auto finish = std::chrono::high_resolution_clock::now();
        latency.record(start, finish);
        ego.step(ctl.steering_curvature - ego.psi * 1.35, 0.0);
        if (std::abs(ego.x - obs_x) < 4.0) {
            min_obs_clearance = std::min(min_obs_clearance,
                                         std::abs(ego.y - obs_y));
        }
    }
    return min_obs_clearance > 1.8 && std::abs(ego.y) < 0.18;
}

struct AdasScenarioFitness {
    std::array<bool, 6> passed{};
    std::array<double, 6> metric{};
    std::array<double, 6> latency_mean_ns{};
    std::array<double, 6> latency_max_ns{};
    double latency_ns{0.0};
    double cost{0.0};
    double score{-1000.0};

    bool all_passed() const {
        return std::all_of(passed.begin(), passed.end(),
                           [](bool value) { return value; });
    }
};

inline AdasScenarioFitness evaluate_adas_fitness(CellularOrganism& organism,
                                                const ScenarioPerturbation& perturb = {}) {
    AdasScenarioFitness result;
    if (!organism.evaluate_adas_contract().valid()) return result;
    AdasCellularAdapter adapter(organism);

    adapter.get_organism().reset_state(true);
    double max_err = 0.0, mean_err = 0.0;
    ScenarioLatency latency;
    result.passed[0] = run_scenario_curve_tracking(adapter, max_err,
                                                    mean_err, latency, perturb);
    result.metric[0] = max_err;
    result.latency_mean_ns[0] = latency.mean_us * 1000.0;
    result.latency_max_ns[0] = latency.max_us * 1000.0;

    adapter.get_organism().reset_state(true);
    double safety_dist = 0.0;
    bool aeb_triggered = false;
    latency.reset();
    result.passed[1] = run_scenario_emergency_cutin_aeb(
        adapter, safety_dist, aeb_triggered, latency, perturb);
    result.metric[1] = safety_dist;
    result.latency_mean_ns[1] = latency.mean_us * 1000.0;
    result.latency_max_ns[1] = latency.max_us * 1000.0;

    adapter.get_organism().reset_state(true);
    double settle_time = 0.0, overshoot = 0.0;
    latency.reset();
    result.passed[2] = run_scenario_lane_change(
        adapter, settle_time, overshoot, latency, perturb);
    result.metric[2] = overshoot;
    result.latency_mean_ns[2] = latency.mean_us * 1000.0;
    result.latency_max_ns[2] = latency.max_us * 1000.0;

    adapter.get_organism().reset_state(true);
    double gap_error = 0.0;
    latency.reset();
    result.passed[3] = run_scenario_stop_and_go(
        adapter, gap_error, latency, perturb);
    result.metric[3] = gap_error;
    result.latency_mean_ns[3] = latency.mean_us * 1000.0;
    result.latency_max_ns[3] = latency.max_us * 1000.0;

    adapter.get_organism().reset_state(true);
    double merge_speed = 0.0;
    latency.reset();
    result.passed[4] = run_scenario_ramp_merge(
        adapter, merge_speed, latency, perturb);
    result.metric[4] = merge_speed;
    result.latency_mean_ns[4] = latency.mean_us * 1000.0;
    result.latency_max_ns[4] = latency.max_us * 1000.0;

    adapter.get_organism().reset_state(true);
    double clearance = 0.0;
    latency.reset();
    result.passed[5] = run_scenario_obstacle_swerve(
        adapter, clearance, latency, perturb);
    result.metric[5] = clearance;
    result.latency_mean_ns[5] = latency.mean_us * 1000.0;
    result.latency_max_ns[5] = latency.max_us * 1000.0;

    result.latency_ns = 0.0;
    for (double value : result.latency_mean_ns) result.latency_ns += value;
    result.latency_ns /= static_cast<double>(result.latency_mean_ns.size());

    size_t active_synapses = 0;
    for (const auto& synapse : organism.synapses) {
        if (synapse.is_active) ++active_synapses;
    }
    result.cost = result.latency_ns * 0.0001 +
                  static_cast<double>(organism.cells.size()) * 0.01 +
                  static_cast<double>(active_synapses) * 0.002;
    result.score = result.all_passed() ? 600.0 - result.cost
                                       : -600.0 - result.cost;
    return result;
}

} // namespace kun::adas_test
