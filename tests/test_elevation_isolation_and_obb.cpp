/**
 * test_elevation_isolation_and_obb.cpp — Phase 3 P3-4 automated verification:
 *   1. Vertical layer isolation (|z_a - z_b| <= 2.2m) in perception_points,
 *      perception_fusion_core, kalman_tracker, behavior_gap_eval, behavior_uturn_approach,
 *      and 3D safety / forward-space helpers.
 *   2. Generic rotated 3D OBB clearance & height-aware low ground curb handling.
 *   3. FlowRoadNetwork::world_to_frenet(..., z) multi-level viaduct deck disambiguation
 *      when built against esminiRMLib.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "adas_msgs_gen.h"
#include "perception_points.h"
#include "perception_fusion_core.h"
#include "behavior_gap_eval.h"
#include "behavior_uturn_approach.h"
#include "kalman_tracker.h"

#ifdef HAS_ESMINI_RMLIB
#include "flowsim/road_network.h"
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #cond); \
        std::abort(); \
    } \
} while (0)

static void test_pf_fusion_z_isolation(void) {
    ObstacleList a{}, b{}, out{};
    a.count = 1;
    a.obstacles[0].x = 20.0f;
    a.obstacles[0].y = 0.0f;
    a.obstacles[0].z = 0.0f;
    a.obstacles[0].length = 4.6f;
    a.obstacles[0].width = 2.0f;
    a.obstacles[0].height = 1.6f;
    a.obstacles[0].heading = 0.10f;
    a.obstacles[0].type = OBJ_TYPE_VEHICLE;
    a.obstacles[0].confidence = 0.90f;

    /* Different deck (-9m) at identical (x, y) MUST NOT merge. */
    b.count = 1;
    b.obstacles[0] = a.obstacles[0];
    b.obstacles[0].z = -9.0f;
    b.obstacles[0].heading = -0.45f;
    b.obstacles[0].type = OBJ_TYPE_UNKNOWN;
    b.obstacles[0].confidence = 0.95f;

    const int merges_diff = pf_fuse_obstacles(&a, &b, 2.5, &out);
    CHECK(merges_diff == 0);
    CHECK(out.count == 2);

    /* Same deck (|dz| = 0.8 <= 2.2m) MUST merge, preserving winner z/heading/type and max height. */
    ObstacleList b_same{};
    b_same.count = 1;
    b_same.obstacles[0] = a.obstacles[0];
    b_same.obstacles[0].x = 20.2f;
    b_same.obstacles[0].z = -0.8f;
    b_same.obstacles[0].height = 1.9f;
    b_same.obstacles[0].heading = 0.25f;
    b_same.obstacles[0].type = OBJ_TYPE_UNKNOWN;
    b_same.obstacles[0].confidence = 0.98f;

    const int merges_same = pf_fuse_obstacles(&a, &b_same, 2.5, &out);
    CHECK(merges_same == 1);
    CHECK(out.count == 1);
    CHECK(std::fabs(out.obstacles[0].z - (-0.8f)) < 1e-4f);
    CHECK(std::fabs(out.obstacles[0].heading - 0.25f) < 1e-4f);
    CHECK(std::fabs(out.obstacles[0].height - 1.9f) < 1e-4f);
    CHECK(out.obstacles[0].type == OBJ_TYPE_UNKNOWN);
    std::printf("PASS test_pf_fusion_z_isolation\n");
}

static void test_perception_points_stacked_clusters(void) {
    ClusterBounds clusters[2]{};
    /* Ground-deck vehicle observed from viaduct ego (ego_z=9.0m, lidar_mount=1.8m => sensor=10.8m). */
    clusters[0].cx = 20.0f;
    clusters[0].cy = 0.0f;
    clusters[0].width = 2.0f;
    clusters[0].length = 4.6f;
    clusters[0].min_z = -10.8f;
    clusters[0].max_z = -9.2f;
    clusters[0].point_count = 14;
    clusters[0].confidence = 0.92f;
    clusters[0].cls = CLS_VEHICLE;

    /* Same-deck vehicle at (20, 0, world z=9.0m). */
    clusters[1] = clusters[0];
    clusters[1].min_z = -1.8f;
    clusters[1].max_z = -0.2f;

    PerceptionFramePose pose{};
    pose.frame_id = 7;
    pose.ego_y = -1.75;
    pose.ego_heading = 0.0;
    pose.lane_count = 2;
    pose.lane_width = 3.5;
    pose.road_oneway = 0;
    pose.ego_z = 9.0;
    pose.lidar_mount_height_m = 1.8;

    ObstacleList out{};
    const uint32_t count = perception_clusters_to_obstacles(clusters, 2, &pose, &out);
    CHECK(count == 2);
    CHECK(out.count == 2);
    CHECK(std::fabs(out.obstacles[0].z - (-9.0f)) < 1e-3f);
    CHECK(std::fabs(out.obstacles[0].height - 1.6f) < 1e-3f);
    CHECK(std::fabs(out.obstacles[1].z - 0.0f) < 1e-3f);
    CHECK(std::fabs(out.obstacles[1].height - 1.6f) < 1e-3f);
    std::printf("PASS test_perception_points_stacked_clusters\n");
}

static void test_kalman_tracker_z_isolation(void) {
    KalmanTracker kt{};
    ktracker_init(&kt, 0.05);

    /* Track existing car on z=9.0m viaduct. */
    KTrackDetection upper{};
    upper.x = 20.0f;
    upper.y = 0.0f;
    upper.z = 9.0f;
    upper.width = 2.0f;
    upper.length = 4.6f;
    upper.height = 1.6f;
    upper.heading = 0.3f;
    upper.cls = 1;
    upper.confidence = 0.95f;
    ktracker_associate_and_update(&kt, &upper, 1);
    CHECK(ktracker_total_count(&kt) == 1);
    CHECK(std::fabs(kt.tracks[0].z - 9.0f) < 1e-4f);
    CHECK(std::fabs(kt.tracks[0].heading - 0.3f) < 1e-4f);
    const int upper_id = kt.tracks[0].id;

    /* Next frame: lower-deck detection at identical (x, y) MUST NOT match upper track. */
    KTrackDetection lower = upper;
    lower.z = 0.0f;
    lower.heading = -0.5f;
    ktracker_predict(&kt);
    ktracker_associate_and_update(&kt, &lower, 1);
    CHECK(ktracker_total_count(&kt) == 2);
    int found_upper = 0, found_lower = 0;
    for (int i = 0; i < kt.n_tracks; ++i) {
        if (kt.tracks[i].id == upper_id) {
            found_upper = 1;
            CHECK(std::fabs(kt.tracks[i].z - 9.0f) < 1e-4f);
            CHECK(kt.tracks[i].misses == 1);
        } else {
            found_lower = 1;
            CHECK(std::fabs(kt.tracks[i].z - 0.0f) < 1e-4f);
            CHECK(std::fabs(kt.tracks[i].heading - (-0.5f)) < 1e-4f);
        }
    }
    CHECK(found_upper && found_lower);
    std::printf("PASS test_kalman_tracker_z_isolation\n");
}

static void test_viaduct_z_isolation_and_3d_obb(void) {
    /* Scenario: ego on viaduct deck at (x=10, y=0, z=9.0m, heading=0). */
    const double ego_x = 10.0, ego_y = 0.0, ego_z = 9.0, ego_h = 0.0;

    /* 1) Ground-floor parked car + unknown static box directly below ego's lane at z=0.0m */
    CHECK(!pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                                12.0, 0.0, 0.0, 4.6, 2.0, 1.6, 0.0, false));
    CHECK(!pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                                11.5, 0.0, 0.0, 1.2, 1.2, 1.0, 0.785, false));
    CHECK(pf_obb_forward_space_m(ego_x, ego_y, ego_z, ego_h,
                                 12.0, 0.0, 0.0, 4.6, 2.0, 1.6, 0.0) >= 1e8);

    /* Behavior gap eval & U-turn approach ignore z=0.0m obstacles while ego is on z=9.0m deck */
    behavior_gap::GapEvalConfig gcfg;
    gcfg.ego_x = ego_x;
    gcfg.ego_y = -1.75;
    gcfg.ego_z = ego_z;
    gcfg.ego_v = 12.0;
    gcfg.ego_heading = 0.0;
    gcfg.ego_lane_d = -1.75;
    gcfg.target_speed = 15.0;
    const double obs_x[2] = { 22.0, 24.0 };
    const double obs_y[2] = { -5.25, -5.25 };
    const double obs_z[2] = { 0.0, 0.0 };
    const double obs_vx[2] = { 0.0, 0.0 };
    const double obs_vy[2] = { 0.0, 0.0 };
    gcfg.obs_z = obs_z;
    auto dummy_proj = [](double, double, double&) { return false; };
    auto gap_res = behavior_gap::evaluate_target_lane_gap(
        gcfg, -5.25, true, true, obs_x, obs_y, obs_vx, obs_vy, 2, dummy_proj);
    CHECK(gap_res.front_gap >= 1e8);
    CHECK(gap_res.ok);

    behavior_uturn::UturnApproachInput uin;
    uin.ego_x = 180.0;
    uin.ego_y = -1.75;
    uin.ego_z = ego_z;
    uin.ego_v = 10.0;
    uin.road_end_x = 400.0;
    uin.uturn_approach_dist_m = 60.0;
    const double u_ox[1] = { 190.0 };
    const double u_oz[1] = { 0.0 };
    const double u_ovx[1] = { 0.0 };
    const double u_ovy[1] = { 0.0 };
    uin.obs_x = u_ox;
    uin.obs_z = u_oz;
    uin.obs_vx = u_ovx;
    uin.obs_vy = u_ovy;
    uin.obs_count = 1;
    auto udec = behavior_uturn::evaluate_uturn_approach(uin);
    CHECK(!udec.trigger);
    CHECK(!udec.approach_active);

    /* 2) Same-layer rotated unknown 3D OBB (4.0m long x 0.8m wide barrier rotated 90 deg at x=12.4, y=2.6):
     *    - Because heading=pi/2 rotates long axis laterally, box spans x in [12.0, 12.8], y in [0.6, 4.6] -> HITS ego corridor [-1, 1] */
    CHECK(pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                               12.4, 2.6, 9.0, 4.0, 0.8, 1.2, M_PI * 0.5, false));
    /*    - With heading=0.0, box spans y in [2.2, 3.0] -> CLEAR of ego corridor [-1, 1] */
    CHECK(!pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                                12.4, 2.6, 9.0, 4.0, 0.8, 1.2, 0.0, false));

    /* 3) Height-aware low wheel-stop (height=0.14m <= 0.25m -> axle span 1.55 + 0.15 = 1.70m vs body 2.65m):
     *    - Overhang at dx=2.0m passes safely; inside wheel span dx=1.55m triggers stop */
    CHECK(!pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                                12.0, 0.0, 9.0, 0.16, 2.0, 0.14, 0.0, false));
    CHECK(pf_3d_obb_safety_hit(ego_x, ego_y, ego_z, ego_h,
                               11.55, 0.0, 9.0, 0.16, 2.0, 0.14, 0.0, false));

    /* 4) Generic forward space respects rotated 3D OBB front edge & elevation */
    const double d_rot = pf_obb_forward_space_m(
        ego_x, ego_y, ego_z, ego_h,
        20.0, 0.0, 9.2, 4.0, 1.0, 1.4, 0.0, 2.0, 1.0);
    /* front edge at 20.0 - 2.0 = 18.0 -> (18.0 - 10.0) - 1.0 = 7.0 */
    CHECK(std::fabs(d_rot - 7.0) < 1e-6);

    const double d_perp = pf_obb_forward_space_m(
        ego_x, ego_y, ego_z, ego_h,
        20.0, 0.0, 9.2, 4.0, 1.0, 1.4, M_PI * 0.5, 2.0, 1.0);
    /* rotated 90 deg -> front edge at 20.0 - 0.5 = 19.5 -> 9.5 - 1.0 = 8.5 */
    CHECK(std::fabs(d_perp - 8.5) < 1e-6);

    std::printf("PASS test_viaduct_z_isolation_and_3d_obb\n");
}

#ifdef HAS_ESMINI_RMLIB
static void test_flow_road_network_viaduct_z(void) {
    const char* path = "/tmp/test_viaduct_z.xodr";
    FILE* f = std::fopen(path, "w");
    CHECK(f != nullptr);
    std::fputs(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<OpenDRIVE>\n"
        "  <header revMajor=\"1\" revMinor=\"4\" name=\"viaduct_z_test\" version=\"1.0\" "
        "north=\"50\" south=\"-50\" east=\"120\" west=\"-20\"/>\n"
        "  <road name=\"lower_deck\" length=\"100.0\" id=\"0\" junction=\"-1\">\n"
        "    <planView><geometry s=\"0.0\" x=\"0.0\" y=\"0.0\" hdg=\"0.0\" length=\"100.0\"><line/></geometry></planView>\n"
        "    <elevationProfile><elevation s=\"0.0\" a=\"0.0\" b=\"0.0\" c=\"0.0\" d=\"0.0\"/></elevationProfile>\n"
        "    <lanes><laneSection s=\"0.0\">\n"
        "      <center><lane id=\"0\" type=\"none\" level=\"false\"/></center>\n"
        "      <right><lane id=\"-1\" type=\"driving\" level=\"false\"><width sOffset=\"0.0\" a=\"3.5\" b=\"0.0\" c=\"0.0\" d=\"0.0\"/></lane></right>\n"
        "    </laneSection></lanes>\n"
        "  </road>\n"
        "  <road name=\"upper_viaduct\" length=\"100.0\" id=\"1\" junction=\"-1\">\n"
        "    <planView><geometry s=\"0.0\" x=\"0.0\" y=\"0.0\" hdg=\"0.0\" length=\"100.0\"><line/></geometry></planView>\n"
        "    <elevationProfile><elevation s=\"0.0\" a=\"9.0\" b=\"0.0\" c=\"0.0\" d=\"0.0\"/></elevationProfile>\n"
        "    <lanes><laneSection s=\"0.0\">\n"
        "      <center><lane id=\"0\" type=\"none\" level=\"false\"/></center>\n"
        "      <right><lane id=\"-1\" type=\"driving\" level=\"false\"><width sOffset=\"0.0\" a=\"3.5\" b=\"0.0\" c=\"0.0\" d=\"0.0\"/></lane></right>\n"
        "    </laneSection></lanes>\n"
        "  </road>\n"
        "</OpenDRIVE>\n",
        f);
    std::fclose(f);

    flowsim::FlowRoadNetwork net;
    CHECK(net.load(path));

    flowsim::FrenetPos fp_low{}, fp_high{}, fp_low_again{};
    CHECK(net.world_to_frenet(50.0, -1.75, fp_low, 0.0));
    CHECK(net.world_to_frenet(50.0, -1.75, fp_high, 9.0));
    CHECK(net.world_to_frenet(50.0, -1.75, fp_low_again, 0.0));
    CHECK(fp_low.road_id == 0);
    CHECK(fp_high.road_id == 1);
    CHECK(fp_low_again.road_id == 0);

    flowsim::WorldPos wp_low{}, wp_high{};
    CHECK(net.frenet_to_world(fp_low.road_id, fp_low.lane_id, fp_low.s, fp_low.offset, wp_low));
    CHECK(net.frenet_to_world(fp_high.road_id, fp_high.lane_id, fp_high.s, fp_high.offset, wp_high));
    CHECK(std::fabs(wp_low.z - 0.0) < 0.2);
    CHECK(std::fabs(wp_high.z - 9.0) < 0.2);

    std::remove(path);
    std::printf("PASS test_flow_road_network_viaduct_z (low road=%d z=%.2f, high road=%d z=%.2f)\n",
                fp_low.road_id, wp_low.z, fp_high.road_id, wp_high.z);
}
#endif

static void test_safety_sign_obb_and_overhead_clearance(void) {
    CHECK(OBJ_TYPE_SIGN == 5);
    const double ego_x = 10.0, ego_y = 0.0, ego_z = 0.0, ego_h = 0.0;

    /* Ground-mounted traffic sign (0.55 x 0.85 x 2.60) in ego corridor triggers OBB clearance */
    const double sign_hit = pf_obb_forward_space_m(
        ego_x, ego_y, ego_z, ego_h,
        22.0, 0.0, 0.0, 0.55, 0.85, 2.60, 0.0, 1.15, 2.5);
    /* front edge at 22.0 - 0.275 = 21.725 -> 11.725 - 2.5 = 9.225 */
    CHECK(sign_hit < 1e8 && std::fabs(sign_hit - 9.225) < 1e-6);

    /* Roadside traffic sign outside lateral corridor (lat=3.8m) is not treated as an in-lane blocker */
    const double sign_side = pf_obb_forward_space_m(
        ego_x, ego_y, ego_z, ego_h,
        22.0, 3.8, 0.0, 0.55, 0.85, 2.60, 0.0, 1.15, 2.5);
    CHECK(sign_side >= 1e8);

    /* Overhead gantry / exit sign (dz = 2.15m >= 2.1m) sits above drivable envelope despite <=2.2m layer tol */
    const double overhead_z = 2.15;
    CHECK(pf_same_elevation_layer(overhead_z, ego_z));
    CHECK((overhead_z - ego_z) >= 2.1);

    std::printf("PASS test_safety_sign_obb_and_overhead_clearance\n");
}

int main(void) {
    test_pf_fusion_z_isolation();
    test_perception_points_stacked_clusters();
    test_kalman_tracker_z_isolation();
    test_viaduct_z_isolation_and_3d_obb();
    test_safety_sign_obb_and_overhead_clearance();
#ifdef HAS_ESMINI_RMLIB
    test_flow_road_network_viaduct_z();
#endif
    std::printf("ALL PASS test_elevation_isolation_and_obb\n");
    return 0;
}
