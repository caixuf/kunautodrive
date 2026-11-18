#include "route_join_policy.h"
#include "road_network.h"
#include "route.h"

#include <cjson/cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s\n", msg);                           \
            ++g_fails;                                                        \
        }                                                                     \
    } while (0)

static std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static cJSON* edge_by_id(cJSON* edges, int id) {
    cJSON* e = nullptr;
    cJSON_ArrayForEach(e, edges) {
        cJSON* jid = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (cJSON_IsNumber(jid) && (int)jid->valuedouble == id) return e;
    }
    return nullptr;
}

static bool node_xy(cJSON* nodes, int index, double& x, double& y) {
    int n = cJSON_GetArraySize(nodes);
    if (index < 0) index = n - 1;
    if (index < 0 || index >= n) return false;
    cJSON* p = cJSON_GetArrayItem(nodes, index);
    if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 2) return false;
    x = cJSON_GetArrayItem(p, 0)->valuedouble;
    y = cJSON_GetArrayItem(p, 1)->valuedouble;
    return true;
}

static void test_scenario_and_route(const std::string& root) {
    const std::string scenario = root + "/scenarios/park_to_park_city.json";
    cJSON* doc = cJSON_Parse(read_file(scenario).c_str());
    CHECK(doc != nullptr, "scenario json parses");
    if (!doc) return;
    cJSON* td = cJSON_GetObjectItemCaseSensitive(doc, "traffic_density");
    cJSON* cpk = cJSON_GetObjectItemCaseSensitive(td, "cars_per_km");
    cJSON* cap = cJSON_GetObjectItemCaseSensitive(td, "max_npcs");
    CHECK(cJSON_IsNumber(cpk) && (int)cpk->valuedouble == 10, "cars_per_km is 10");
    CHECK(cJSON_IsNumber(cap) && (int)cap->valuedouble == 15, "max_npcs is 15");

    cJSON* rn = cJSON_GetObjectItemCaseSensitive(doc, "road_network");
    cJSON* edges = cJSON_GetObjectItemCaseSensitive(rn, "edges");
    cJSON* ramp = edge_by_id(edges, 4);
    cJSON* via = edge_by_id(edges, 5);
    CHECK(ramp && via, "edges 4 and 5 exist");
    if (!ramp || !via) {
        cJSON_Delete(doc);
        return;
    }
    cJSON* lanes = cJSON_GetObjectItemCaseSensitive(via, "lanes");
    cJSON* oneway = cJSON_GetObjectItemCaseSensitive(via, "oneway");
    cJSON* accel = cJSON_GetObjectItemCaseSensitive(ramp, "accel_length_m");
    CHECK(cJSON_IsNumber(lanes) && (int)lanes->valuedouble == 8, "viaduct has 8 lanes");
    CHECK(cJSON_IsBool(oneway) && !cJSON_IsTrue(oneway), "viaduct is two-way");
    CHECK(cJSON_IsNumber(accel) && accel->valuedouble >= 140.0 && accel->valuedouble <= 160.0,
          "on-ramp taper is about 150 m");

    cJSON* rn_nodes = cJSON_GetObjectItemCaseSensitive(ramp, "nodes");
    cJSON* vn_nodes = cJSON_GetObjectItemCaseSensitive(via, "nodes");
    double rx, ry, px, py, vx, vy, v2x, v2y;
    CHECK(node_xy(rn_nodes, -1, rx, ry) && node_xy(rn_nodes, -2, px, py), "ramp end");
    CHECK(node_xy(vn_nodes, 0, vx, vy) && node_xy(vn_nodes, 1, v2x, v2y), "viaduct start");
    const double gap = std::hypot(rx - vx, ry - vy);
    const double rh = std::atan2(ry - py, rx - px);
    const double vh = std::atan2(v2y - vy, v2x - vx);
    const double tx = std::cos(vh), ty = std::sin(vh);
    const double along = (rx - vx) * tx + (ry - vy) * ty;
    const double dh = flowengine::heading_delta_abs(rh, vh);
    std::printf("ramp end (%.2f,%.2f) viaduct start (%.2f,%.2f) gap=%.2f along=%.2f dh=%.3f\n",
                rx, ry, vx, vy, gap, along, dh);
    CHECK(ry < vy, "ramp end is south of the viaduct centerline");
    CHECK(flowengine::beside_accel_join(gap, dh, along),
          "east on-ramp join is a beside-lane join");

    const std::string xodr = root + "/build/park_to_park_join.xodr";
    const std::string cmd = "python3 " + root + "/tools/json_to_xodr.py " + scenario +
                            " -o " + xodr;
    CHECK(std::system(cmd.c_str()) == 0, "json_to_xodr writes the scenario");

    flowsim::FlowRoadNetwork roads;
    CHECK(roads.load(xodr), "esmini loads the generated xodr");
    if (roads.loaded()) {
        int ids[2] = {4, 5};
        flowsim::Route route;
        CHECK(route.build_from_chain(roads, ids, 2), "route builds across the on-ramp");
        int i4 = route.index_of(4);
        int i5 = route.index_of(5);
        CHECK(i4 >= 0 && i5 == i4 + 1, "viaduct follows the ramp with no segment between");
        if (i4 >= 0 && i5 == i4 + 1) {
            CHECK(!route.seg(i4).is_virtual && !route.seg(i5).is_virtual,
                  "neither road segment is a virtual bridge");
        }
        bool virtual_between = false;
        for (int i = 0; i < route.count(); ++i) {
            if (route.seg(i).is_virtual) virtual_between = true;
        }
        CHECK(!virtual_between, "no virtual bridge cuts from the ramp end to the centerline");
        CHECK(roads.drivable_lane_count(5, 1.0) == 8, "loaded viaduct has 8 drivable lanes");
        const auto ramp_lanes = roads.drivable_lane_ids(4, 1.0);
        bool ramp_pos = false, ramp_neg = false;
        for (int lid : ramp_lanes) {
            if (lid > 0) ramp_pos = true;
            if (lid < 0) ramp_neg = true;
        }
        CHECK(ramp_pos && ramp_neg, "on-ramp keeps an opposing lane for the return");
        const double via_v = roads.speed_limit(5, -1, 1.0, 0.0);
        const double ramp_v = roads.speed_limit(4, -1, 1.0, 0.0);
        std::printf("posted ramp=%.2f viaduct=%.2f\n", ramp_v, via_v);
        CHECK(std::fabs(via_v - 15.5) < 0.2, "viaduct honors the authored 15.5 m/s");
        CHECK(std::fabs(ramp_v - 15.0) < 0.2, "ramp honors the authored 15.0 m/s");
    }
    cJSON_Delete(doc);
}

static void test_align_ref_path_step() {
    double x1[6] = {1605.0, 1612.5, 1620.0, 1620.0, 1628.0, 1636.0};
    double y1[6] = {237.65, 237.70, 237.75, 250.00, 250.00, 250.00};
    CHECK(flowengine::align_ref_path_step(x1, y1, 6, 1610.0, 236.0),
          "aligns parallel step when ego is on pre-jump ramp");
    CHECK(std::fabs(y1[3] - 237.75) < 1e-6 && std::fabs(y1[5] - 237.75) < 1e-6,
          "post-jump points shifted down to pre-jump ramp height");

    double x2[6] = {1605.0, 1612.5, 1620.0, 1620.0, 1628.0, 1636.0};
    double y2[6] = {237.65, 237.70, 237.75, 250.00, 250.00, 250.00};
    CHECK(flowengine::align_ref_path_step(x2, y2, 6, 1625.0, 237.75),
          "aligns parallel step when ego is on post-jump viaduct");
    CHECK(std::fabs(y2[0] - 249.90) < 1e-6 && std::fabs(y2[2] - 250.00) < 1e-6,
          "pre-jump points shifted up to viaduct centerline when ego is past the join");

    /* Parking-slot -> aisle hop is ~6.5m; it must not be treated as a
     * beside-lane join. The later 12.25m viaduct join is the real step. */
    double x3[8] = {100.0, 95.0, 95.0, 90.0, 85.0, 85.0, 80.0, 75.0};
    double y3[8] = {0.0, 0.0, 6.5, 6.5, 6.5, 18.75, 18.75, 18.75};
    CHECK(flowengine::align_ref_path_step(x3, y3, 8, 100.0, 0.0),
          "finds the real 12.25m join while ignoring the 6.5m slot hop");
    CHECK(std::fabs(y3[0]) < 1e-6 && std::fabs(y3[2] - 6.5) < 1e-6,
          "near side / slot hop is left untouched");
    CHECK(std::fabs(y3[5] - 6.5) < 1e-6 && std::fabs(y3[7] - 6.5) < 1e-6,
          "far side of the 12.25m join is shifted onto the near side");

    /* Return leg: the far side is aligned at the join, then blended back to
     * the real far-side lane over subsequent points instead of staying on
     * the entry carriageway (wrong-way) forever. */
    double x4[8] = {100.0, 95.0, 95.0, 90.0, 85.0, 85.0, 80.0, 75.0};
    double y4[8] = {0.0, 0.0, 6.5, 6.5, 6.5, 18.75, 18.75, 18.75};
    CHECK(flowengine::align_ref_path_step(x4, y4, 8, 100.0, 0.0, true),
          "return-leg join aligns and tapers");
    CHECK(std::fabs(y4[5] - 6.5) < 1e-6,
          "first far-side point is on the entry carriageway");
    CHECK(y4[6] > 6.5 && y4[6] < 18.75,
          "second far-side point blends toward the real lane");

    /* Return route: keep the far side aligned even after the ego crosses,
     * otherwise the path snaps back to the opposite carriageway. */
    double x5[6] = {1605.0, 1612.5, 1620.0, 1620.0, 1628.0, 1636.0};
    double y5[6] = {237.65, 237.70, 237.75, 250.00, 250.00, 250.00};
    CHECK(flowengine::align_ref_path_step(x5, y5, 6, 1625.0, 237.75,
                                          /*taper_far_side=*/false,
                                          /*force_far_side=*/true),
          "return-leg force-far alignment applies after the join");
    CHECK(std::fabs(y5[0] - 237.65) < 1e-6 && std::fabs(y5[2] - 237.75) < 1e-6 &&
          std::fabs(y5[5] - 237.75) < 1e-6,
          "force-far keeps the entry carriageway instead of snapping back");
}

int main() {
    test_scenario_and_route(FLOW_SOURCE_DIR);
    test_align_ref_path_step();
    if (g_fails) {
        std::fprintf(stderr, "FAIL viaduct join (%d)\n", g_fails);
        return 1;
    }
    std::printf("PASS viaduct join\n");
    return 0;
}
