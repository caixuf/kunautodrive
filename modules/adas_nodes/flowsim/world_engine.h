#ifndef FLOWSIM_WORLD_ENGINE_H
#define FLOWSIM_WORLD_ENGINE_H

#include "flowsim/building.h"
#include <cstdint>
#include <string>
#include <vector>

namespace flowsim {

enum : uint32_t {
    WORLD_MASK_NONE      = 0u,
    WORLD_MASK_POLE      = 1u << 0,
    WORLD_MASK_TREE      = 1u << 1,
    WORLD_MASK_BARRIER   = 1u << 2,
    WORLD_MASK_PIER      = 1u << 3,
    WORLD_MASK_SIGN      = 1u << 4,
    WORLD_MASK_FURNITURE = 1u << 5,
    WORLD_MASK_BUILDING  = 1u << 6,
    WORLD_MASK_PROP      = 1u << 7,
    WORLD_MASK_ALL       = 0xFFu,
};

struct WorldExclusionZone {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double radius{0.0};
    double min_x{0.0};
    double max_x{0.0};
    double min_y{0.0};
    double max_y{0.0};
    bool is_box{false};
    bool check_z{false};
    uint32_t kind_mask{WORLD_MASK_ALL};
};

struct WorldEdgeOverride {
    std::string match;
    bool has_poles{false}, poles{true};
    bool has_trees{false}, trees{true};
    bool has_barriers{false}, barriers{false};
    bool has_signs{false}, signs{true};
    bool has_furniture{false}, furniture{true};
    double pole_spacing_m{0.0};
    double tree_spacing_m{0.0};
    double speed_limit_kph{0.0};
};

struct WorldCustomPropSpec {
    std::string id;
    std::string kind;        /* welcome_gantry | arch_landmark | speed_sign | stop_sign | bus_shelter | ev_charger | bollard | planter | landmark_tower */
    std::string label;
    std::string placement;   /* "road_s" | "world" */
    std::string road;        /* edge name or edge id string when placement == "road_s" */
    double s{0.0};
    double d{0.0};           /* ENU lateral offset (+ left, - right) */
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double z_offset{0.0};
    double heading{0.0};
    double heading_offset{0.0};
    double length{1.0};
    double width{1.0};
    double height{2.5};
    bool auto_z{true};
    bool has_collider{false};
    bool collider_explicit{false};
};

struct WorldGenConfig {
    std::string preset{"urban_arterial"}; /* modern_cbd | urban_arterial | suburban_parkway | highway_corridor */
    uint32_t seed{42};
    bool auto_poles{true};
    bool auto_trees{true};
    bool auto_barriers{true};
    bool auto_piers{true};
    bool auto_signs{true};
    bool auto_furniture{true};
    bool auto_buildings{true};
    double pole_spacing_m{40.0};
    double tree_spacing_m{30.0};
    double barrier_spacing_m{4.0};
    double pier_spacing_m{22.0};
    double tree_density{1.0};
    std::string tree_species{"broadleaf"};
    std::vector<WorldExclusionZone> exclude_zones;
    std::vector<WorldEdgeOverride> edge_overrides;
    std::vector<WorldCustomPropSpec> custom_props;
};

struct WorldGateInput {
    double x{0.0}, y{0.0}, z{0.0}, heading{0.0};
};

struct WorldLightInput {
    int id{0};
    double x{0.0}, y{0.0}, z{0.0}, heading{0.0};
};

struct WorldParkingSlotInput {
    int id{0};
    double x{0.0}, y{0.0}, z{0.0}, heading{0.0};
    double width{2.8}, length{5.8};
};

struct WorldParkOutInput {
    bool enabled{false};
    double slot_x{0.0}, slot_y{0.0}, slot_z{0.0}, slot_heading{0.0};
    double exit_x{0.0}, exit_y{0.0}, exit_z{0.0}, exit_heading{0.0};
    double corridor_width{6.0};
};

struct WorldEngineInput {
    const char* road_network_json{nullptr};
    const char* world_gen_json{nullptr};     /* Optional JSON text (either raw world_gen obj or full scenario JSON) */
    const char* scenario_file_path{nullptr}; /* Fallback to read top-level "world_gen" if world_gen_json is null */
    uint32_t random_seed{42};
    std::vector<WorldGateInput> etc_gates;
    std::vector<WorldLightInput> traffic_lights;
    std::vector<WorldParkingSlotInput> parking_slots;
    WorldParkingSlotInput parking_goal;
    bool has_parking_goal{false};
    WorldParkOutInput park_out;
    std::vector<BuildingOBB> manual_buildings;
};

struct WorldPole {
    double x{0.0}, y{0.0}, z{0.0};
    double nx{0.0}, ny{0.0}; /* unit lateral normal in ENU */
    double heading{0.0};
    int side{1};
    bool is_double_arm{false};
    std::string edge_id;
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldTree {
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0};
    double variant{0.5};
    double height_scale{1.0};
    double crown_scale{1.0};
    int side{1};
    std::string species{"broadleaf"};
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldBarrierPost {
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0};
};

struct WorldBarrierSpan {
    double ax{0.0}, ay{0.0}, az{0.0};
    double bx{0.0}, by{0.0}, bz{0.0};
    double mid_x{0.0}, mid_y{0.0}, mid_z{0.0};
    double len{0.0}, heading{0.0};
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldPier {
    double x{0.0}, y{0.0};
    double z_ground{0.0}, z_top{0.0};
    double heading{0.0};
    double half_w{3.5};
    int columns{1};
    double col_offset{0.0};
    double radius{0.75};
    double cap_len{4.0}, cap_w{1.6}, cap_h{0.7};
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldSign {
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0};
    std::string kind;   /* stop_sign | speed_limit | crosswalk_warn | curve_warn | slope_warn | exit_sign */
    std::string label;
    int speed_kph{60};
    double length{0.55};
    double width{0.85};
    double height{2.60};
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldFurniture {
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0};
    double length{1.0}, width{1.0}, height{1.5};
    std::string kind;
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldBuildingSlot {
    double x{0.0}, y{0.0}, z{0.0};
    double width{14.0}, depth{12.0}, height{20.0};
    double heading{0.0};
    int layer{0};
    double seed{0.5};
    bool has_collider{true};
    bool sensor_visible{true};
};

struct WorldProp {
    std::string id;
    std::string kind;
    std::string label;
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0};
    double length{1.0}, width{1.0}, height{2.5};
    bool has_collider{true};
    bool sensor_visible{true};
};

enum class ColliderRole {
    StructuralObstacle = 0, // 桥墩/实体墙/柱/禁行桩/路内可撞障碍：物理碰撞 + 感知障碍物
    RoadsideBarrier    = 1, // 护栏段：仅 Jolt 3D 物理碰撞，不作为感知障碍物
    OffroadProp        = 2, // 路灯/树木/标志/路侧家具/止轮器/道闸等：不作为感知障碍物
};

struct WorldCollider {
    std::string kind;
    double x{0.0}, y{0.0}, z{0.0};
    double len{0.5}, wid{0.5}, heading{0.0}, height{1.5};
    bool is_cylinder{false};
    bool sensor_visible{true};
    std::string subkind{};
    ColliderRole role{ColliderRole::StructuralObstacle};
};

inline bool is_perception_facility_collider(const WorldCollider& c) {
    return c.sensor_visible && c.role == ColliderRole::StructuralObstacle;
}

inline bool is_jolt_static_collider(const WorldCollider& c) {
    return c.role != ColliderRole::OffroadProp;
}

struct WorldSanityReport {
    int poles{0};
    int trees{0};
    int barrier_posts{0};
    int barrier_spans{0};
    int piers{0};
    int signs{0};
    int furniture{0};
    int buildings{0};
    int props{0};
    int colliders{0};
    int pruned_asphalt{0};
    int pruned_junction{0};
    int pruned_cutout{0};
    int pruned_excluded{0};
    int remaining_asphalt_intrusions{0};
    int floating_count{0};
    double compile_ms{0.0};
};

struct WorldLayout {
    WorldGenConfig config;
    std::string preset{"urban_arterial"};
    std::string buildings_source{"world_engine"}; /* "map_buildings" | "world_engine" | "none" */
    std::string world_hash;
    std::vector<WorldPole> poles;
    std::vector<WorldTree> trees;
    std::vector<WorldBarrierPost> barrier_posts;
    std::vector<WorldBarrierSpan> barrier_spans;
    std::vector<WorldPier> piers;
    std::vector<WorldSign> signs;
    std::vector<WorldFurniture> furniture;
    std::vector<WorldBuildingSlot> buildings;
    std::vector<WorldProp> props;
    std::vector<WorldCollider> colliders;
    WorldSanityReport sanity;
};

/** Parse "world_gen" block (accepts either the world_gen object itself or full scenario JSON). */
WorldGenConfig parse_world_gen_config(const char* scenario_or_wg_json, uint32_t default_seed = 42u);

/** Compile complete 3D/Physics/Sensor world layout deterministically from roads + refinements. */
WorldLayout compile_world(const WorldEngineInput& input);

/** Serialize compiled layout to compact JSON (consumed by FlowBoard /api/world/layout). */
std::string serialize_world_layout_json(const WorldLayout& layout);

/** Atomically write compiled layout JSON to disk (temp file + rename). */
bool write_world_layout_json(const WorldLayout& layout, const char* path);

}  // namespace flowsim

#endif  // FLOWSIM_WORLD_ENGINE_H
