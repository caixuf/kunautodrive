/**
 * physics_world.h — Headless 3D rigid-body vehicle physics world (Jolt Physics backend)
 *
 * Implements Phase 6 (P6-1 .. P6-5):
 *   - 6-DOF sprung chassis rigid body with true moments of inertia & CG height
 *   - 4-wheel independent suspension (spring/damper + anti-roll bars + travel limits)
 *   - Combined longitudinal/lateral Pacejka tire forces coupled with dynamic normal
 *     loads F_z and spatial surface friction zones (mu_scale, split-mu, ABS)
 *   - Static 3D collision world built from FlowRoadNetwork elevation/lanes,
 *     junction pads, garage slabs, parking lots, OSM building extrusions,
 *     and facility obstacles (trees, light poles, pillars, walls, bollards, gates)
 *   - True gravity and wheel-raycast support without forced z=wp.z snapping
 */

#ifndef FLOWSIM_PHYSICS_WORLD_H
#define FLOWSIM_PHYSICS_WORLD_H

#include "building.h"
#include "entity.h"
#include "scenario_loader.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace flowsim {

class FlowRoadNetwork;

struct WheelTelemetry {
    bool   grounded{false};
    double contact_x{0.0};
    double contact_y{0.0};
    double contact_z{0.0};
    double normal_force{0.0};       /**< Dynamic tire normal load F_z (N) */
    double suspension_length{0.0};  /**< Current spring length (m) */
    double longitudinal_slip{0.0};
    double lateral_slip{0.0};
    double mu_effective{1.0};
};

struct VehiclePhysicsTelemetry {
    double x{0.0}, y{0.0}, z{0.0};
    double heading{0.0}, pitch{0.0}, roll{0.0};
    double vx{0.0}, vy{0.0}, vz{0.0}, speed{0.0};
    double v_x_body{0.0}, v_y_body{0.0};
    double yaw_rate{0.0}, pitch_rate{0.0}, roll_rate{0.0};
    double a_x_body{0.0}, a_y_body{0.0};
    double F_zf{0.0}, F_zr{0.0};
    WheelTelemetry wheels[4]{};     /**< 0=FL, 1=FR, 2=RL, 3=RR */
};

struct StaticBoxObstacle {
    double x{0.0};
    double y{0.0};
    double z{0.0};                  /**< Base / bottom elevation (ENU m) */
    double len{0.5};
    double wid{0.5};
    double height{1.5};
    double heading{0.0};
    std::string kind{"obstacle"};
    bool is_cylinder{false};
    std::vector<std::pair<double, double>> footprint;
};

struct FrictionPatch {
    double x_min{0.0}, x_max{0.0};
    double y_min{0.0}, y_max{0.0};
    double mu_scale{1.0};
};

struct PhysicsContactEvent {
    int         entity_a{-1};       /**< Pool index of vehicle A */
    int         entity_b{-1};       /**< Pool index of vehicle B, or -1 if static world */
    std::string kind_b;             /**< "vehicle", "building", "pole", "tree", "pillar", etc. */
    int         obstacle_index{-1}; /**< Index into facility/building list when entity_b < 0 */
    double      impact_speed{0.0};  /**< Relative normal speed at contact (m/s) */
    double      contact_x{0.0};
    double      contact_y{0.0};
    double      contact_z{0.0};
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    /** Clears all bodies, meshes, vehicles, and state for a fresh world. */
    void reset();

    /** Returns true if Jolt backend is initialized and ready for stepping. */
    bool ready() const;

    /**
     * Populates static 3D terrain/road meshes and static obstacle colliders from
     * OpenDRIVE road network, scenario geometry (garage/parking slabs/cutouts/
     * construction zones/friction patches), OSM buildings, and roadside facilities.
     */
    bool build_static_world(FlowRoadNetwork* roads,
                            const ScenarioConfig* scenario,
                            const std::vector<BuildingOBB>& buildings,
                            const std::vector<StaticBoxObstacle>& facilities);

    /** Standalone geometry builders for deterministic CTest physics scenes. */
    void add_ground_slab(double cx, double cy, double top_z,
                         double half_x, double half_y, double friction = 1.0);
    void add_ramp_surface(double x0, double y0, double z0,
                          double x1, double y1, double z1,
                          double width, double friction = 1.0);
    void add_mesh_triangles(const std::vector<double>& xyz_triplets,
                            double friction = 1.0);
    void add_static_obstacle(const StaticBoxObstacle& obs, int obstacle_index = -1);
    void set_friction_zones(std::vector<FrictionPatch> patches);

    /** Registers a movable/removable gate boom barrier and toggles its blocking state. */
    int  add_gate_barrier(double x, double y, double z,
                          double len, double wid, double height,
                          double heading, int pool_entity_idx = -1);
    void set_gate_open(int gate_handle, bool open);

    /** Registers a full 6-DOF 4-wheel suspended vehicle body bound to an Entity pool index. */
    bool register_vehicle(int entity_idx, const Entity& e, bool abs_enabled = true);
    bool register_ego(const Entity& e, bool abs_enabled = true) {
        return register_vehicle(0, e, abs_enabled);
    }

    /** Controls throttle [0..1], brake [0..1], steer [rad], gear (1=D, -1=R), and mu scale. */
    void set_vehicle_input(int entity_idx, double throttle, double brake,
                           double steer_rad, int gear, double mu_scale = 1.0);

    /** Teleports / resets a registered vehicle body to an exact 3D pose & forward speed. */
    void set_vehicle_pose(int entity_idx, double x, double y, double z,
                          double heading, double speed = 0.0);

    /**
     * Synchronizes non-ego vehicles (moving NPCs, parked vehicles, UnknownStatic) from
     * EntityPool into Jolt rigid bodies with raycast ground/normal support on roads.
     */
    void sync_npcs_from_pool(EntityPool& pool, double dt);

    /** Advances physics by dt seconds using stable <=10ms internal substeps. */
    void step(double dt);

    /** Reads full 3D chassis & per-wheel telemetry for a registered 6-DOF vehicle. */
    bool get_vehicle_telemetry(int entity_idx, VehiclePhysicsTelemetry& out) const;

    /** Copies solved 6-DOF physics state (pose, velocity, pitch, roll, axle loads) to Entity. */
    bool sync_entity_from_vehicle(int entity_idx, Entity& e) const;

    /** Drains collision events observed since the previous call. */
    std::vector<PhysicsContactEvent> consume_contacts();

    /** Downward single raycast against drivable LAYER_ROAD_SURFACE bodies. */
    bool raycast_ground(double x, double y, double z_top, double max_dist,
                        double& out_z, double& out_nx, double& out_ny, double& out_nz) const;

    /** Wall-clock compute time of the last step(dt) call in milliseconds. */
    double last_step_time_ms() const;

    /** Returns count of static & dynamic bodies currently in the Jolt PhysicsSystem. */
    int body_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace flowsim

#endif  // FLOWSIM_PHYSICS_WORLD_H
