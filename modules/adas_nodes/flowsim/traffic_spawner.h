#ifndef FLOWSIM_TRAFFIC_SPAWNER_H
#define FLOWSIM_TRAFFIC_SPAWNER_H

#include <cstdint>
#include <vector>

#include "flowsim/entity.h"
#include "flowsim/road_network.h"
#include "flowsim/route.h"
#include "flowsim/sim_rng.h"
#include "scenario_loader.h"

namespace flowsim {

struct TrafficSpawnerContext {
    const ScenarioConfig* scenario{nullptr};
    bool                  roads_loaded{false};
    FlowRoadNetwork*      roads{nullptr};
    Route*                route{nullptr};
    std::vector<Route>*   npc_routes{nullptr};
    EntityPool*           pool{nullptr};
    SimRng*               ped_rng{nullptr};
    SimRng*               sim_rng{nullptr};
    int*                  npc_route_spawned{nullptr};

    bool                  enable_poisson_traffic{false};
    double                poisson_rate{0.25};
    int                   poisson_max_vehicles{24};
    double                target_speed{12.0};
    uint32_t              cycle{0};
};

void build_npc_routes_from_scenario(TrafficSpawnerContext& ctx);

int auto_populate_traffic_density(TrafficSpawnerContext& ctx,
                                  const ScenarioConfig* sc,
                                  bool* pool_full_out);

void step_poisson_traffic(TrafficSpawnerContext& ctx,
                          double dt, double ego_route_s, double sim_time_s);

}  // namespace flowsim

#endif  // FLOWSIM_TRAFFIC_SPAWNER_H
