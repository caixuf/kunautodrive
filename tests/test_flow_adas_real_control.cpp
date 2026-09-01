#include <cassert>
#include <iomanip>
#include <iostream>

#include "adas_scenario_suite.hpp"
#include "kun/cellular/cellular_genome.hpp"

using namespace kun;
using namespace kun::adas_test;

int main() {
    std::cout << "\n======================================================================\n";
    std::cout << " ADAS cellular brain: six deterministic control scenarios\n";
    std::cout << "======================================================================\n\n";

    {
        auto blank = CellularOrganism::create_disconnected_embryo(1);
        AdasCellularAdapter blank_adas(blank);
        auto ctl = blank_adas.process_perception(8.0, -4.0, 0.0, 0.8);
        assert(!ctl.is_aeb_triggered);
        std::cout << "[Step 0] blank embryo has no adapter AEB bypass\n";
    }

    auto seed = CellularOrganism::create_adas_seed_organism(1);
    seed.compile();
    AdasCellularAdapter adas_brain(seed);
    std::cout << "[Step 1] compiled ADAS progenitor: cells=" << seed.cells.size()
              << ", synapses=" << seed.synapses.size() << "\n\n";

    std::cout << "[Step 2] six deterministic control scenarios:\n";
    std::cout << "----------------------------------------------------------------------\n";

    double s1_max = 0.0, s1_mean = 0.0, s1_latency = 0.0;
    bool s1_ok = run_scenario_curve_tracking(adas_brain, s1_max, s1_mean,
                                              s1_latency);
    std::cout << "  [1] curve tracking: " << (s1_ok ? "PASS" : "FAIL")
              << " | mean=" << std::fixed << std::setprecision(3) << s1_mean
              << "m, max=" << s1_max << "m, latency=" << std::setprecision(2)
              << s1_latency << " us\n";
    if (!s1_ok) return 1;

    double s2_dist = 0.0;
    bool s2_aeb = false;
    bool s2_ok = run_scenario_emergency_cutin_aeb(adas_brain, s2_dist, s2_aeb);
    std::cout << "  [2] emergency cut-in AEB: " << (s2_ok ? "PASS" : "FAIL")
              << " | triggered=" << (s2_aeb ? "YES" : "NO")
              << ", min distance=" << s2_dist << "m\n";
    if (!s2_ok) return 1;

    double s3_settle = 0.0, s3_overshoot = 0.0;
    bool s3_ok = run_scenario_lane_change(adas_brain, s3_settle, s3_overshoot);
    std::cout << "  [3] lane change: " << (s3_ok ? "PASS" : "FAIL")
              << " | settle=" << s3_settle << "s, overshoot=" << s3_overshoot
              << "m\n";
    if (!s3_ok) return 1;

    double s4_gap_error = 0.0;
    bool s4_ok = run_scenario_stop_and_go(adas_brain, s4_gap_error);
    std::cout << "  [4] stop and go: " << (s4_ok ? "PASS" : "FAIL")
              << " | max gap error=" << s4_gap_error << "m\n";
    if (!s4_ok) return 1;

    double s5_speed = 0.0;
    bool s5_ok = run_scenario_ramp_merge(adas_brain, s5_speed);
    std::cout << "  [5] ramp merge: " << (s5_ok ? "PASS" : "FAIL")
              << " | terminal speed=" << s5_speed << "m/s\n";
    if (!s5_ok) return 1;

    double s6_clearance = 0.0;
    bool s6_ok = run_scenario_obstacle_swerve(adas_brain, s6_clearance);
    std::cout << "  [6] obstacle swerve: " << (s6_ok ? "PASS" : "FAIL")
              << " | clearance=" << s6_clearance << "m\n";
    if (!s6_ok) return 1;

    std::cout << "----------------------------------------------------------------------\n";
    std::cout << " all six deterministic control scenarios passed\n";
    return 0;
}
