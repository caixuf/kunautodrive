#include "road_speed_policy.h"

#include <cmath>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg);    \
            ++g_fails;                                                           \
        }                                                                        \
    } while (0)

static void test_posted_table() {
    const double urban = flowengine::posted_speed_mps("urban", 14.0);
    CHECK(std::fabs(urban - 14.0) < 1e-9, "urban 14 m/s stays in the 40-60 band");
    const double urban_default = flowengine::posted_speed_mps("urban", 30.0);
    CHECK(std::fabs(urban_default - 50.0 / 3.6) < 1e-9, "urban outside the band falls to 50 km/h");
    const double via = flowengine::posted_speed_mps("viaduct_highway", 15.5);
    CHECK(std::fabs(via - 15.5) < 1e-9, "authored viaduct speed is honored");
    const double via_default = flowengine::posted_speed_mps("viaduct_highway", 0.0);
    CHECK(std::fabs(via_default - 100.0 / 3.6) < 1e-6,
          "un-authored viaduct defaults to 100 km/h");
    const double ramp = flowengine::posted_speed_mps("ramp_curve", 14.0);
    CHECK(std::fabs(ramp - 14.0) < 1e-9, "authored ramp speed is honored");
    const double ramp_default = flowengine::posted_speed_mps("ramp_curve", 0.0);
    CHECK(std::fabs(ramp_default - 60.0 / 3.6) < 1e-6,
          "un-authored ramp defaults to 60 km/h");
    const double garage = flowengine::posted_speed_mps("garage_hall", 6.5);
    CHECK(std::fabs(garage - 6.5) < 1e-9, "garage keeps its authored low limit");
}

static void test_overtake_caps() {
    const double posted = 100.0 / 3.6;
    const double camera = flowengine::command_cap_mps(posted, true, true, 0.0);
    CHECK(camera <= posted * 1.2 + 1e-9, "camera overtake stays within 20%");
    CHECK(camera > posted, "camera overtake is above the posted limit");
    const double free = flowengine::command_cap_mps(posted, true, false, 0.0);
    CHECK(free > posted * 1.2, "no camera on a straight can exceed 20%");
    CHECK(free <= flowengine::kSkillMaxMps + 1e-9, "skill max holds");
    const double urban_free = flowengine::command_cap_mps(15.0, true, false, 0.0);
    CHECK(urban_free <= 15.0 * flowengine::kNoCameraOvertakeMult + 1e-9,
          "urban overtake stays within 1.5x posted (not 130 km/h)");
    const double hairpin = flowengine::command_cap_mps(posted, false, false, 0.05);
    CHECK(hairpin < posted, "a sharp curve holds the car under the posted limit");
    const double cruise = flowengine::command_cap_mps(14.0, false, false, 0.0);
    CHECK(std::fabs(cruise - 14.0) < 1e-9, "urban cruise stays on the posted limit");
}

int main() {
    test_posted_table();
    test_overtake_caps();
    if (g_fails) {
        std::fprintf(stderr, "FAIL road speed policy (%d)\n", g_fails);
        return 1;
    }
    std::printf("PASS road speed policy\n");
    return 0;
}
