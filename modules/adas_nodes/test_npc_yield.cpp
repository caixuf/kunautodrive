#include "flowsim/npc_yield.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

namespace {

flowsim::Entity& add_veh(flowsim::EntityPool& pool,
                         double x, double y, double h, double v) {
    flowsim::EntityId id = pool.alloc(flowsim::EntityType::Car);
    flowsim::Entity& e = pool[id];
    e.x = x;
    e.y = y;
    e.heading = h;
    e.speed = v;
    e.target_vx = v;
    return e;
}

void test_crossing_yields_to_ego() {
    flowsim::EntityPool pool;
    // Ego (id=0) heading +X at (0, 0), v=10 m/s -> arrives at (20, 0) in 2.0s
    add_veh(pool, 0.0, 0.0, 0.0, 10.0);
    // NPC (id=1) heading +Y at (20, -20), v=10 m/s -> arrives at (20, 0) in 2.0s
    const auto& npc = add_veh(pool, 20.0, -20.0, 1.57079632679, 10.0);

    auto res = flowsim::evaluate_crossing_conflict_yield(npc, pool);
    CHECK(res.should_yield);
    CHECK(res.yield_to == 0);
    CHECK(res.cpa_dist_m < 0.5);
    CHECK(res.speed_cap < 10.0);
}

void test_parallel_same_lane_does_not_trigger_crossing_yield() {
    flowsim::EntityPool pool;
    add_veh(pool, 20.0, 0.0, 0.0, 10.0);
    const auto& npc = add_veh(pool, 0.0, 0.0, 0.0, 10.0);

    auto res = flowsim::evaluate_crossing_conflict_yield(npc, pool);
    CHECK(!res.should_yield);
}

void test_earlier_npc_does_not_yield_to_later_npc() {
    flowsim::EntityPool pool;
    // id=0 dummy parked far away
    add_veh(pool, 1000.0, 1000.0, 0.0, 0.0);
    // NPC 1 is 14m from intersection (1.4s away), NPC 2 is 17m away (1.7s away)
    const auto& npc1 = add_veh(pool, 0.0, -14.0, 1.57079632679, 10.0);
    const auto& npc2 = add_veh(pool, -17.0, 0.0, 0.0, 10.0);

    auto res1 = flowsim::evaluate_crossing_conflict_yield(npc1, pool);
    auto res2 = flowsim::evaluate_crossing_conflict_yield(npc2, pool);
    CHECK(!res1.should_yield);
    CHECK(res2.should_yield);
    CHECK(res2.yield_to == 1);
}

}  // namespace

int main() {
    test_crossing_yields_to_ego();
    test_parallel_same_lane_does_not_trigger_crossing_yield();
    test_earlier_npc_does_not_yield_to_later_npc();
    std::printf("test_npc_yield: all 3 tests passed\n");
    return 0;
}
