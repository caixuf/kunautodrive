#include <iomanip>
#include <iostream>
#include <string>

#include "adas_scenario_suite.hpp"
#include "kun/cellular/cellular_genome.hpp"

using namespace kun;
using namespace kun::adas_test;

static bool require_check(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << "\n";
    return false;
}

static bool valid_latency(const ScenarioLatency& latency) {
    return latency.samples > 0 && latency.mean_us > 0.0 &&
           latency.max_us >= latency.mean_us;
}

int main() {
    std::cout << "\n======================================================================\n";
    std::cout << " ADAS cellular brain: six deterministic control scenarios\n";
    std::cout << "======================================================================\n\n";

    {
        auto blank = CellularOrganism::create_disconnected_embryo(1);
        AdasCellularAdapter blank_adas(blank);
        auto ctl = blank_adas.process_perception(8.0, -4.0, 0.0, 0.8);
        if (!require_check(!ctl.is_aeb_triggered,
                           "blank embryo unexpectedly triggered AEB")) {
            return 1;
        }
        std::cout << "[Step 0] blank embryo has no adapter AEB bypass\n";
    }

    auto seed = CellularOrganism::create_adas_seed_organism(1);
    seed.compile();
    AdasCellularAdapter adas_brain(seed);
    std::cout << "[Step 1] compiled ADAS progenitor: cells=" << seed.cells.size()
              << ", synapses=" << seed.synapses.size() << "\n\n";

    std::cout << "[Step 2] six deterministic control scenarios:\n";
    std::cout << "----------------------------------------------------------------------\n";

    double s1_max = 0.0, s1_mean = 0.0;
    ScenarioLatency s1_latency;
    bool s1_ok = run_scenario_curve_tracking(adas_brain, s1_max, s1_mean,
                                              s1_latency);
    std::cout << "  [1] curve tracking: " << (s1_ok ? "PASS" : "FAIL")
              << " | mean=" << std::fixed << std::setprecision(3) << s1_mean
              << "m, max=" << s1_max << "m, latency=" << std::setprecision(2)
              << s1_latency.mean_us << " us, max latency="
              << s1_latency.max_us << " us (" << s1_latency.samples
              << " samples)\n";
    if (!require_check(s1_ok && valid_latency(s1_latency),
                       "curve tracking or latency gate failed")) return 1;

    double s2_dist = 0.0;
    bool s2_aeb = false;
    ScenarioLatency s2_latency;
    bool s2_ok = run_scenario_emergency_cutin_aeb(
        adas_brain, s2_dist, s2_aeb, s2_latency);
    std::cout << "  [2] emergency cut-in AEB: " << (s2_ok ? "PASS" : "FAIL")
              << " | triggered=" << (s2_aeb ? "YES" : "NO")
              << ", min distance=" << s2_dist << "m, mean latency="
              << s2_latency.mean_us << " us, max latency="
              << s2_latency.max_us << " us (" << s2_latency.samples
              << " samples)\n";
    if (!require_check(s2_ok && valid_latency(s2_latency),
                       "emergency cut-in or latency gate failed")) return 1;

    double s3_settle = 0.0, s3_overshoot = 0.0;
    ScenarioLatency s3_latency;
    bool s3_ok = run_scenario_lane_change(
        adas_brain, s3_settle, s3_overshoot, s3_latency);
    std::cout << "  [3] lane change: " << (s3_ok ? "PASS" : "FAIL")
              << " | settle=" << s3_settle << "s, overshoot=" << s3_overshoot
              << "m, mean latency=" << s3_latency.mean_us
              << " us, max latency=" << s3_latency.max_us << " us ("
              << s3_latency.samples << " samples)\n";
    if (!require_check(s3_ok && valid_latency(s3_latency),
                       "lane change or latency gate failed")) return 1;

    double s4_gap_error = 0.0;
    ScenarioLatency s4_latency;
    bool s4_ok = run_scenario_stop_and_go(
        adas_brain, s4_gap_error, s4_latency);
    std::cout << "  [4] stop and go: " << (s4_ok ? "PASS" : "FAIL")
              << " | max gap error=" << s4_gap_error << "m, mean latency="
              << s4_latency.mean_us << " us, max latency="
              << s4_latency.max_us << " us (" << s4_latency.samples
              << " samples)\n";
    if (!require_check(s4_ok && valid_latency(s4_latency),
                       "stop-and-go or latency gate failed")) return 1;

    double s5_speed = 0.0;
    ScenarioLatency s5_latency;
    bool s5_ok = run_scenario_ramp_merge(
        adas_brain, s5_speed, s5_latency);
    std::cout << "  [5] ramp merge: " << (s5_ok ? "PASS" : "FAIL")
              << " | terminal speed=" << s5_speed << "m/s, mean latency="
              << s5_latency.mean_us << " us, max latency="
              << s5_latency.max_us << " us (" << s5_latency.samples
              << " samples)\n";
    if (!require_check(s5_ok && valid_latency(s5_latency),
                       "ramp merge or latency gate failed")) return 1;

    double s6_clearance = 0.0;
    ScenarioLatency s6_latency;
    bool s6_ok = run_scenario_obstacle_swerve(
        adas_brain, s6_clearance, s6_latency);
    std::cout << "  [6] obstacle swerve: " << (s6_ok ? "PASS" : "FAIL")
              << " | clearance=" << s6_clearance << "m, mean latency="
              << s6_latency.mean_us << " us, max latency="
              << s6_latency.max_us << " us (" << s6_latency.samples
              << " samples)\n";
    if (!require_check(s6_ok && valid_latency(s6_latency),
                       "obstacle swerve or latency gate failed")) return 1;

    std::cout << "----------------------------------------------------------------------\n";
    std::cout << " all six deterministic control scenarios passed\n";
    return 0;
}
