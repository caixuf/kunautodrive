/**
 * physics_world.cpp — Headless 3D rigid-body vehicle physics world (Jolt Physics v5.7.0)
 */

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include "physics_world.h"
#include "road_network.h"
#include "scenario_loader.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

JPH_SUPPRESS_WARNINGS

namespace flowsim {

namespace {

constexpr double kGravity = 9.81;
constexpr double kMaxSubstepSec = 0.01;     // 100 Hz internal integration step
constexpr double kGroundClearanceM = 0.22;  // Clears 0.18m wheel_stop under overhang

// Object layers
namespace Layers {
static constexpr JPH::ObjectLayer ROAD_SURFACE    = 0;
static constexpr JPH::ObjectLayer STATIC_OBSTACLE = 1;
static constexpr JPH::ObjectLayer VEHICLE         = 2;
static constexpr JPH::ObjectLayer WHEEL_RAY       = 3;
static constexpr JPH::uint        NUM_LAYERS      = 4;
}  // namespace Layers

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint            NUM_LAYERS(2);
}  // namespace BroadPhaseLayers

void jolt_trace_impl(const char* fmt, ...) {
    (void)fmt;
}

void ensure_jolt_initialized() {
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jolt_trace_impl;
        if (!JPH::Factory::sInstance) {
            JPH::Factory::sInstance = new JPH::Factory();
        }
        JPH::RegisterTypes();
    });
}

enum class BodyTagKind : uint8_t {
    RoadSurface    = 0,
    StaticObstacle = 1,
    Building       = 2,
    Construction   = 3,
    Vehicle        = 4,
    Gate           = 5,
};

uint64_t pack_body_tag(BodyTagKind kind, int32_t index, uint16_t kind_idx = 0) {
    return (static_cast<uint64_t>(static_cast<uint8_t>(kind)) << 48) |
           (static_cast<uint64_t>(kind_idx) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(index));
}

void unpack_body_tag(uint64_t tag, BodyTagKind& kind, int32_t& index, uint16_t& kind_idx) {
    kind     = static_cast<BodyTagKind>((tag >> 48) & 0xFFu);
    kind_idx = static_cast<uint16_t>((tag >> 32) & 0xFFFFu);
    index    = static_cast<int32_t>(tag & 0xFFFFFFFFu);
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
            case Layers::ROAD_SURFACE:
                return b == Layers::VEHICLE || b == Layers::WHEEL_RAY;
            case Layers::STATIC_OBSTACLE:
                return b == Layers::VEHICLE;
            case Layers::VEHICLE:
                return b == Layers::ROAD_SURFACE || b == Layers::STATIC_OBSTACLE ||
                       b == Layers::VEHICLE;
            case Layers::WHEEL_RAY:
                return b == Layers::ROAD_SURFACE;
            default:
                return false;
        }
    }
};

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        map_[Layers::ROAD_SURFACE]    = BroadPhaseLayers::NON_MOVING;
        map_[Layers::STATIC_OBSTACLE] = BroadPhaseLayers::NON_MOVING;
        map_[Layers::VEHICLE]         = BroadPhaseLayers::MOVING;
        map_[Layers::WHEEL_RAY]       = BroadPhaseLayers::NON_MOVING;
    }

    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return map_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        switch ((JPH::BroadPhaseLayer::Type)layer) {
            case 0: return "NON_MOVING";
            case 1: return "MOVING";
            default: return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer map_[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        switch (a) {
            case Layers::ROAD_SURFACE:
            case Layers::STATIC_OBSTACLE:
            case Layers::WHEEL_RAY:
                return b == BroadPhaseLayers::NON_MOVING;
            case Layers::VEHICLE:
                return true;
            default:
                return false;
        }
    }
};

struct AABB2D {
    double min_x, max_x, min_y, max_y;
};

static double pacejka_eval(double x_rad, double B, double C, double D, double E) {
    double Bx = B * x_rad;
    return D * std::sin(C * std::atan(Bx - E * (Bx - std::atan(Bx))));
}

static void populate_pacejka_tire_curves(JPH::WheelSettingsWV& ws,
                                         double lat_b, double lat_c, double lat_e, double mu_base) {
    // Longitudinal Pacejka curve vs slip ratio kappa [0 .. 1.0]
    ws.mLongitudinalFriction.Clear();
    constexpr int n_long = 17;
    for (int i = 0; i <= n_long; ++i) {
        double k = (i <= 8) ? (0.025 * i) : (0.20 + 0.80 * (double)(i - 8) / (n_long - 8));
        double val = (i == 0) ? 0.0 : std::max(0.0, pacejka_eval(k, 14.0, 1.55, mu_base * 1.12, 0.55));
        ws.mLongitudinalFriction.AddPoint((float)k, (float)val);
    }

    // Lateral Pacejka curve vs slip angle in degrees [0 .. 35 deg]
    ws.mLateralFriction.Clear();
    const double degs[] = {0.0, 1.0, 2.0, 4.0, 6.0, 9.0, 12.0, 16.0, 22.0, 35.0};
    for (double deg : degs) {
        double rad = deg * (M_PI / 180.0);
        double val = (deg == 0.0) ? 0.0 : std::max(0.0, pacejka_eval(rad, lat_b, lat_c, mu_base, lat_e));
        ws.mLateralFriction.AddPoint((float)deg, (float)val);
    }
}

static JPH::Vec3 wrap_enu_to_world_pos(const WorldPos& wp) {
    return JPH::Vec3((float)wp.x, (float)wp.y, (float)wp.z);
}

static void ensure_upward_winding(const JPH::VertexList& verts, JPH::IndexedTriangle& tri) {
    const JPH::Float3& a = verts[tri.mIdx[0]];
    const JPH::Float3& b = verts[tri.mIdx[1]];
    const JPH::Float3& c = verts[tri.mIdx[2]];
    double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    double acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
    double nz = abx * acy - aby * acx;
    (void)abz; (void)acz;
    if (nz < 0.0) {
        std::swap(tri.mIdx[1], tri.mIdx[2]);
    }
}

static double tri_area_xy(const JPH::Float3& a, const JPH::Float3& b, const JPH::Float3& c) {
    return 0.5 * std::fabs((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y));
}

}  // namespace

struct PhysicsWorld::Impl {
    struct VehicleSlot {
        int entity_idx{-1};
        JPH::BodyID body_id;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTesterRay> wheel_tester;

        double mass{1500.0};
        double wheelbase{2.7};
        double cg_height{0.55};
        double drag_coeff{0.3};
        double pacejka_mu{0.95};
        double wheel_radius{0.34};
        double max_steer_rad{0.60};
        double max_brake_torque{950.0};
        bool   abs_enabled{true};

        // Per-step control & state
        double throttle{0.0};
        double brake{0.0};
        double steer_rad{0.0};
        int    gear{1};
        double base_mu_scale{1.0};
        double wheel_mu[4]{1.0, 1.0, 1.0, 1.0};

        double prev_vx_body{0.0};
        double prev_vy_body{0.0};
        double last_ax_body{0.0};
        double last_ay_body{0.0};
        double last_sub_dt{0.01};
        double wheel_fz[4]{0.0, 0.0, 0.0, 0.0};
    };

    struct NpcBodySlot {
        int entity_idx{-1};
        JPH::BodyID body_id;
        bool is_static{false};
        double cg_height{0.55};
        double last_z{0.0};
        double vz{0.0};
        double last_pitch{0.0};
        double last_roll{0.0};
    };

    struct GateSlot {
        JPH::BodyID body_id;
        bool active_in_world{true};
    };

    class WorldContactListener final : public JPH::ContactListener {
    public:
        explicit WorldContactListener(Impl* owner) : owner_(owner) {}

        JPH::ValidateResult OnContactValidate(
            const JPH::Body& inBody1, const JPH::Body& inBody2,
            JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
            (void)inBody1; (void)inBody2;
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
                            const JPH::ContactManifold& inManifold,
                            JPH::ContactSettings& ioSettings) override {
            handle_contact(inBody1, inBody2, inManifold, ioSettings);
        }

    private:
        void handle_contact(const JPH::Body& b1, const JPH::Body& b2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& ioSettings) {
            BodyTagKind k1, k2;
            int32_t idx1 = -1, idx2 = -1;
            uint16_t str1 = 0, str2 = 0;
            unpack_body_tag(b1.GetUserData(), k1, idx1, str1);
            unpack_body_tag(b2.GetUserData(), k2, idx2, str2);

            if (k1 != BodyTagKind::Vehicle && k2 != BodyTagKind::Vehicle) return;

            ioSettings.mCombinedRestitution = 0.06f;
            ioSettings.mCombinedFriction    = 0.65f;

            JPH::RVec3 cp1 = manifold.GetWorldSpaceContactPointOn1(0);
            JPH::RVec3 cp2 = manifold.GetWorldSpaceContactPointOn2(0);
            JPH::Vec3 v1 = b1.GetPointVelocity(cp1);
            JPH::Vec3 v2 = b2.GetPointVelocity(cp2);
            double rel_n = std::fabs((double)(v1 - v2).Dot(manifold.mWorldSpaceNormal));

            // Ensure vehicle is in slot A when possible, prioritizing ego (idx 0)
            bool swap = false;
            if (k1 != BodyTagKind::Vehicle) {
                swap = true;
            } else if (k2 == BodyTagKind::Vehicle && idx2 == 0 && idx1 != 0) {
                swap = true;
            }

            const BodyTagKind ka = swap ? k2 : k1;
            const int32_t idxa   = swap ? idx2 : idx1;
            const BodyTagKind kb = swap ? k1 : k2;
            const int32_t idxb   = swap ? idx1 : idx2;
            const uint16_t strb  = swap ? str1 : str2;

            (void)ka;
            // Suppress benign low-speed tire/underbody scrapes on road deck
            if (kb == BodyTagKind::RoadSurface && rel_n < 3.5) return;

            PhysicsContactEvent ev;
            ev.entity_a = idxa;
            ev.entity_b = (kb == BodyTagKind::Vehicle) ? idxb : -1;
            ev.obstacle_index = (kb != BodyTagKind::Vehicle) ? idxb : -1;
            ev.impact_speed = rel_n;
            ev.contact_x = 0.5 * ((double)cp1.GetX() + (double)cp2.GetX());
            ev.contact_y = 0.5 * ((double)cp1.GetY() + (double)cp2.GetY());
            ev.contact_z = 0.5 * ((double)cp1.GetZ() + (double)cp2.GetZ());

            if (kb == BodyTagKind::Vehicle) {
                ev.kind_b = "vehicle";
            } else if (kb == BodyTagKind::RoadSurface) {
                ev.kind_b = "ground_impact";
            } else {
                std::lock_guard<std::mutex> lk(owner_->tag_mutex);
                ev.kind_b = (strb < owner_->kind_strings.size())
                            ? owner_->kind_strings[strb] : "obstacle";
            }

            std::lock_guard<std::mutex> lk(owner_->contact_mutex);
            if (owner_->pending_contacts.size() < 256) {
                owner_->pending_contacts.push_back(std::move(ev));
            }
        }

        Impl* owner_;
    };

    Impl() : contact_listener(this) {
        ensure_jolt_initialized();
        temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
        job_system     = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        init_system();
    }

    ~Impl() {
        clear_all();
    }

    void init_system() {
        physics_system = std::make_unique<JPH::PhysicsSystem>();
        constexpr JPH::uint cMaxBodies             = 8192;
        constexpr JPH::uint cNumBodyMutexes        = 0;
        constexpr JPH::uint cMaxBodyPairs          = 16384;
        constexpr JPH::uint cMaxContactConstraints = 8192;
        physics_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                             bp_layer_interface, obj_vs_bp_filter, obj_layer_pair_filter);
        physics_system->SetGravity(JPH::Vec3(0.0f, 0.0f, -(float)kGravity));

        JPH::PhysicsSettings ps = physics_system->GetPhysicsSettings();
        ps.mNumVelocitySteps = 12;
        ps.mNumPositionSteps = 3;
        ps.mAllowSleeping    = true;
        physics_system->SetPhysicsSettings(ps);
        physics_system->SetContactListener(&contact_listener);
        is_ready = true;
    }

    void clear_all() {
        if (!physics_system) return;
        JPH::BodyInterface& bi = physics_system->GetBodyInterface();

        for (auto& kv : vehicles) {
            if (kv.second.constraint) {
                physics_system->RemoveStepListener(kv.second.constraint);
                physics_system->RemoveConstraint(kv.second.constraint);
            }
            if (!kv.second.body_id.IsInvalid()) {
                bi.RemoveBody(kv.second.body_id);
                bi.DestroyBody(kv.second.body_id);
            }
        }
        vehicles.clear();

        for (auto& kv : npcs) {
            if (!kv.second.body_id.IsInvalid()) {
                bi.RemoveBody(kv.second.body_id);
                bi.DestroyBody(kv.second.body_id);
            }
        }
        npcs.clear();

        for (auto& g : gates) {
            if (!g.body_id.IsInvalid()) {
                if (g.active_in_world) bi.RemoveBody(g.body_id);
                bi.DestroyBody(g.body_id);
            }
        }
        gates.clear();

        for (const JPH::BodyID& bid : static_bodies) {
            if (!bid.IsInvalid()) {
                bi.RemoveBody(bid);
                bi.DestroyBody(bid);
            }
        }
        static_bodies.clear();

        {
            std::lock_guard<std::mutex> lk(contact_mutex);
            pending_contacts.clear();
        }
        {
            std::lock_guard<std::mutex> lk(tag_mutex);
            kind_strings.clear();
            kind_lookup.clear();
        }
        friction_patches.clear();
    }

    uint16_t intern_kind(const std::string& kind) {
        std::lock_guard<std::mutex> lk(tag_mutex);
        auto it = kind_lookup.find(kind);
        if (it != kind_lookup.end()) return it->second;
        uint16_t id = (uint16_t)kind_strings.size();
        kind_strings.push_back(kind);
        kind_lookup[kind] = id;
        return id;
    }

    double sample_friction_at(double x, double y, double fallback_scale) const {
        for (const auto& fp : friction_patches) {
            if (x >= fp.x_min && x <= fp.x_max && y >= fp.y_min && y <= fp.y_max) {
                return std::max(0.05, fp.mu_scale);
            }
        }
        return std::max(0.05, fallback_scale);
    }

    void add_static_box_body(double cx, double cy, double cz,
                             double hx, double hy, double hz,
                             const JPH::Quat& rot,
                             JPH::ObjectLayer layer,
                             uint64_t user_data,
                             double friction = 1.0) {
        if (hx < 1e-3 || hy < 1e-3 || hz < 1e-3) return;
        float convex_r = (float)std::min({0.02, hx * 0.25, hy * 0.25, hz * 0.25});
        JPH::BoxShapeSettings bss(JPH::Vec3((float)hx, (float)hy, (float)hz), convex_r);
        JPH::ShapeSettings::ShapeResult res = bss.Create();
        if (res.HasError()) return;

        JPH::BodyCreationSettings bcs(res.Get(), JPH::RVec3(cx, cy, cz), rot,
                                      JPH::EMotionType::Static, layer);
        bcs.mFriction = (float)friction;
        bcs.mRestitution = 0.05f;
        bcs.mUserData = user_data;
        JPH::Body* b = physics_system->GetBodyInterface().CreateBody(bcs);
        if (b) {
            physics_system->GetBodyInterface().AddBody(b->GetID(), JPH::EActivation::DontActivate);
            static_bodies.push_back(b->GetID());
        }
    }

    void add_static_mesh_centered(const std::vector<JPH::Vec3>& world_verts,
                                  const std::vector<JPH::IndexedTriangle>& tris,
                                  JPH::ObjectLayer layer,
                                  uint64_t user_data,
                                  double friction = 1.0) {
        if (world_verts.empty() || tris.empty()) return;

        double sx = 0.0, sy = 0.0, sz = 0.0;
        for (const auto& v : world_verts) {
            sx += v.GetX();
            sy += v.GetY();
            sz += v.GetZ();
        }
        double inv = 1.0 / (double)world_verts.size();
        double cx = sx * inv, cy = sy * inv, cz = sz * inv;

        JPH::VertexList local_verts;
        local_verts.reserve(world_verts.size());
        for (const auto& v : world_verts) {
            local_verts.push_back(JPH::Float3(
                (float)(v.GetX() - cx),
                (float)(v.GetY() - cy),
                (float)(v.GetZ() - cz)));
        }

        JPH::IndexedTriangleList clean_tris;
        clean_tris.reserve(tris.size());
        for (JPH::IndexedTriangle t : tris) {
            if (t.mIdx[0] == t.mIdx[1] || t.mIdx[1] == t.mIdx[2] || t.mIdx[0] == t.mIdx[2]) continue;
            if (tri_area_xy(local_verts[t.mIdx[0]], local_verts[t.mIdx[1]], local_verts[t.mIdx[2]]) < 1e-4) {
                continue;
            }
            ensure_upward_winding(local_verts, t);
            clean_tris.push_back(t);
        }
        if (clean_tris.empty()) return;

        JPH::MeshShapeSettings mss(std::move(local_verts), std::move(clean_tris));
        mss.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
        JPH::ShapeSettings::ShapeResult res = mss.Create();
        if (res.HasError()) return;

        JPH::BodyCreationSettings bcs(res.Get(), JPH::RVec3(cx, cy, cz), JPH::Quat::sIdentity(),
                                      JPH::EMotionType::Static, layer);
        bcs.mFriction = (float)friction;
        bcs.mRestitution = 0.05f;
        bcs.mUserData = user_data;
        JPH::Body* b = physics_system->GetBodyInterface().CreateBody(bcs);
        if (b) {
            physics_system->GetBodyInterface().AddBody(b->GetID(), JPH::EActivation::DontActivate);
            static_bodies.push_back(b->GetID());
        }
    }

    void add_building_collider(const BuildingOBB& b, int index, const FlowRoadNetwork* /*roads*/) {
        double h = (b.height > 0.5) ? b.height : 12.0;
        const uint16_t kidx = intern_kind("building");
        const uint64_t tag  = pack_body_tag(BodyTagKind::Building, index, kidx);

        // Prefer extruded concave/polygon footprint when detailed vertices exist
        if (b.poly.size() >= 3) {
            std::vector<std::pair<double, double>> ring = b.poly;
            if (ring.size() > 3) {
                const auto& f = ring.front();
                const auto& l = ring.back();
                if (std::hypot(f.first - l.first, f.second - l.second) < 0.05) {
                    ring.pop_back();
                }
            }
            if (ring.size() >= 3) {
                // Guarantee CCW winding in XY so outward wall normals point outside
                double area2 = 0.0;
                for (size_t i = 0; i < ring.size(); ++i) {
                    const auto& p = ring[i];
                    const auto& q = ring[(i + 1) % ring.size()];
                    area2 += p.first * q.second - q.first * p.second;
                }
                if (area2 < 0.0) {
                    std::reverse(ring.begin(), ring.end());
                }

                double cx = b.x, cy = b.y;
                double z_bot = -0.3;
                double z_top = h + 0.5;
                JPH::VertexList verts;
                JPH::IndexedTriangleList tris;
                const size_t n = ring.size();
                verts.reserve(n * 2);
                for (size_t i = 0; i < n; ++i) {
                    verts.push_back(JPH::Float3((float)(ring[i].first - cx),
                                                (float)(ring[i].second - cy), (float)z_bot));
                    verts.push_back(JPH::Float3((float)(ring[i].first - cx),
                                                (float)(ring[i].second - cy), (float)z_top));
                }
                for (size_t i = 0; i < n; ++i) {
                    size_t j = (i + 1) % n;
                    JPH::uint32 b0 = (JPH::uint32)(2 * i);
                    JPH::uint32 t0 = (JPH::uint32)(2 * i + 1);
                    JPH::uint32 b1 = (JPH::uint32)(2 * j);
                    JPH::uint32 t1 = (JPH::uint32)(2 * j + 1);
                    // Edge p[i]->p[j] is CCW => outward normal = (p[j]-p[i]) x +Z
                    tris.push_back(JPH::IndexedTriangle(b0, b1, t1));
                    tris.push_back(JPH::IndexedTriangle(b0, t1, t0));
                }

                JPH::MeshShapeSettings mss(std::move(verts), std::move(tris));
                mss.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
                JPH::ShapeSettings::ShapeResult res = mss.Create();
                if (!res.HasError()) {
                    JPH::BodyCreationSettings bcs(res.Get(), JPH::RVec3(cx, cy, 0.0),
                                                  JPH::Quat::sIdentity(),
                                                  JPH::EMotionType::Static,
                                                  Layers::STATIC_OBSTACLE);
                    bcs.mFriction = 0.6f;
                    bcs.mUserData = tag;
                    JPH::Body* body = physics_system->GetBodyInterface().CreateBody(bcs);
                    if (body) {
                        physics_system->GetBodyInterface().AddBody(body->GetID(),
                                                                   JPH::EActivation::DontActivate);
                        static_bodies.push_back(body->GetID());
                        return;
                    }
                }
            }
        }

        // Box fallback when polygon missing
        double cz = 0.5 * h - 0.15;
        add_static_box_body(b.x, b.y, cz,
                            std::max(0.5, b.len * 0.5),
                            std::max(0.5, b.wid * 0.5),
                            std::max(0.5, h * 0.5 + 0.15),
                            JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)b.heading),
                            Layers::STATIC_OBSTACLE, tag, 0.6);
    }

    void add_road_network_meshes(FlowRoadNetwork& roads,
                                 std::vector<AABB2D>& subsurface_cutouts) {
        const int n_roads = roads.road_count();
        for (int ri = 0; ri < n_roads; ++ri) {
            RoadInfo info;
            if (!roads.road_info(ri, info) || info.length < 0.2) continue;

            const double ds = 2.0;
            const int steps = std::max(2, (int)std::ceil(info.length / ds));
            std::vector<JPH::Vec3> verts;
            std::vector<JPH::IndexedTriangle> tris;
            verts.reserve((size_t)(steps + 1) * 3);
            tris.reserve((size_t)steps * 4);

            bool has_subsurface = false;
            double sub_min_x = 1e12, sub_max_x = -1e12, sub_min_y = 1e12, sub_max_y = -1e12;

            const double shoulder = 0.85;
            for (int k = 0; k <= steps; ++k) {
                double s = (k == steps) ? info.length : (info.length * (double)k / (double)steps);
                std::vector<int> lids = roads.drivable_lane_ids(info.id, s);

                int max_pos = 0;
                int min_neg = 0;
                for (int lid : lids) {
                    if (lid > max_pos) max_pos = lid;
                    if (lid < min_neg) min_neg = lid;
                }

                WorldPos wp_left{}, wp_mid{}, wp_right{};
                bool ok_l = false, ok_m = false, ok_r = false;

                ok_m = roads.frenet_to_world(info.id, 0, s, 0.0, wp_mid);
                if (max_pos > 0) {
                    double w = roads.lane_width(info.id, max_pos, s);
                    if (w < 1.0) w = 3.5;
                    ok_l = roads.frenet_to_world(info.id, max_pos, s, 0.5 * w + shoulder, wp_left);
                } else {
                    ok_l = roads.frenet_to_world(info.id, 0, s, shoulder, wp_left);
                }

                if (min_neg < 0) {
                    double w = roads.lane_width(info.id, min_neg, s);
                    if (w < 1.0) w = 3.5;
                    ok_r = roads.frenet_to_world(info.id, min_neg, s, -(0.5 * w + shoulder), wp_right);
                } else {
                    ok_r = roads.frenet_to_world(info.id, 0, s, -shoulder, wp_right);
                }

                if (!ok_l || !ok_r) break;
                if (!ok_m) {
                    wp_mid.x = 0.5 * (wp_left.x + wp_right.x);
                    wp_mid.y = 0.5 * (wp_left.y + wp_right.y);
                    wp_mid.z = 0.5 * (wp_left.z + wp_right.z);
                }

                if (wp_mid.z < -0.35) {
                    has_subsurface = true;
                    sub_min_x = std::min({sub_min_x, wp_left.x, wp_right.x});
                    sub_max_x = std::max({sub_max_x, wp_left.x, wp_right.x});
                    sub_min_y = std::min({sub_min_y, wp_left.y, wp_right.y});
                    sub_max_y = std::max({sub_max_y, wp_left.y, wp_right.y});
                }

                JPH::uint32 base = (JPH::uint32)verts.size();
                verts.push_back(wrap_enu_to_world_pos(wp_right));
                verts.push_back(wrap_enu_to_world_pos(wp_mid));
                verts.push_back(wrap_enu_to_world_pos(wp_left));

                if (k > 0) {
                    JPH::uint32 prev = base - 3;
                    // Right strip quad (R0, R1, M1, M0) and Left strip quad (M0, M1, L1, L0)
                    tris.push_back(JPH::IndexedTriangle(prev + 0, base + 0, base + 1));
                    tris.push_back(JPH::IndexedTriangle(prev + 0, base + 1, prev + 1));
                    tris.push_back(JPH::IndexedTriangle(prev + 1, base + 1, base + 2));
                    tris.push_back(JPH::IndexedTriangle(prev + 1, base + 2, prev + 2));
                }
            }

            if (!tris.empty()) {
                add_static_mesh_centered(verts, tris, Layers::ROAD_SURFACE,
                                         pack_body_tag(BodyTagKind::RoadSurface, (int32_t)info.id), 1.0);
            }
            if (has_subsurface) {
                subsurface_cutouts.push_back({sub_min_x - 3.0, sub_max_x + 3.0,
                                              sub_min_y - 3.0, sub_max_y + 3.0});
            }
        }

        // Junction intersection discs cover small corner seams at multi-way intersections
        std::vector<RoadEndpoint> eps = roads.road_endpoints();
        std::vector<JunctionInfo> juncs = roads.detect_junctions(15.0);
        for (const auto& j : juncs) {
            double z_sum = 0.0;
            int    z_cnt = 0;
            for (const auto& ep : eps) {
                if (std::hypot(ep.x - j.x, ep.y - j.y) <= 20.0) {
                    WorldPos ewp{};
                    RoadInfo rinf;
                    if (roads.road_info(ep.road_id, rinf) &&
                        roads.frenet_to_world(rinf.id, 0, ep.is_start ? 0.0 : rinf.length, 0.0, ewp)) {
                        z_sum += ewp.z;
                        z_cnt++;
                    }
                }
            }
            double jz = (z_cnt > 0) ? (z_sum / z_cnt) : 0.0;
            double r  = std::max(12.0, j.radius + 4.0);
            constexpr int n_seg = 12;
            std::vector<JPH::Vec3> jv;
            std::vector<JPH::IndexedTriangle> jt;
            jv.reserve(n_seg + 1);
            jv.push_back(JPH::Vec3((float)j.x, (float)j.y, (float)jz));
            for (int k = 0; k < n_seg; ++k) {
                double a = (2.0 * M_PI * k) / n_seg;
                jv.push_back(JPH::Vec3((float)(j.x + r * std::cos(a)),
                                       (float)(j.y + r * std::sin(a)), (float)jz));
            }
            for (int k = 0; k < n_seg; ++k) {
                JPH::uint32 i1 = (JPH::uint32)(1 + k);
                JPH::uint32 i2 = (JPH::uint32)(1 + ((k + 1) % n_seg));
                jt.push_back(JPH::IndexedTriangle(0, i1, i2));
            }
            add_static_mesh_centered(jv, jt, Layers::ROAD_SURFACE,
                                     pack_body_tag(BodyTagKind::RoadSurface, j.id), 1.0);
        }
    }

    void add_scenario_parking_and_ground(const ScenarioConfig* scenario,
                                         std::vector<AABB2D>& subsurface_cutouts) {
        if (scenario && scenario->road_network_json && scenario->road_network_json[0]) {
            cJSON* rn = cJSON_Parse(scenario->road_network_json);
            if (rn) {
                cJSON* earr = cJSON_GetObjectItemCaseSensitive(rn, "edges");
                if (cJSON_IsArray(earr)) {
                    cJSON* edge = nullptr;
                    cJSON_ArrayForEach(edge, earr) {
                        cJSON* jtype  = cJSON_GetObjectItemCaseSensitive(edge, "type");
                        cJSON* jnodes = cJSON_GetObjectItemCaseSensitive(edge, "nodes");
                        if (!jtype || !cJSON_IsString(jtype) ||
                            !cJSON_IsArray(jnodes) || cJSON_GetArraySize(jnodes) < 2) {
                            continue;
                        }
                        const char* etype = jtype->valuestring;
                        bool is_garage = (strcmp(etype, "garage_hall") == 0);
                        bool is_ramp   = (strcmp(etype, "open_ramp") == 0);
                        if (!is_garage && !is_ramp) continue;

                        double minx = 1e12, maxx = -1e12, miny = 1e12, maxy = -1e12, zsum = 0.0;
                        int n = cJSON_GetArraySize(jnodes);
                        for (int i = 0; i < n; ++i) {
                            cJSON* pt = cJSON_GetArrayItem(jnodes, i);
                            double x = cJSON_GetArrayItem(pt, 0)->valuedouble;
                            double y = cJSON_GetArrayItem(pt, 1)->valuedouble;
                            double z = cJSON_GetArraySize(pt) > 2 ? cJSON_GetArrayItem(pt, 2)->valuedouble : 0.0;
                            minx = std::min(minx, x);
                            maxx = std::max(maxx, x);
                            miny = std::min(miny, y);
                            maxy = std::max(maxy, y);
                            zsum += z;
                        }
                        double zavg = zsum / n;
                        if (is_garage) {
                            double hall_hw = 12.2;
                            add_static_box_body(0.5 * (minx + maxx), 0.5 * (miny + maxy), zavg - 0.5,
                                                0.5 * (maxx - minx) + 4.0, hall_hw, 0.5,
                                                JPH::Quat::sIdentity(), Layers::ROAD_SURFACE,
                                                pack_body_tag(BodyTagKind::RoadSurface, -1), 1.0);
                            subsurface_cutouts.push_back({minx - 6.0, maxx + 6.0,
                                                          miny - hall_hw - 2.0, maxy + hall_hw + 2.0});
                        } else if (is_ramp) {
                            subsurface_cutouts.push_back({minx - 8.0, maxx + 8.0,
                                                          miny - 8.0, maxy + 8.0});
                        }
                    }
                }
                cJSON_Delete(rn);
            }
        }

        // Parking lot / park-out approach apron slabs
        if (scenario) {
            auto add_pad = [&](double x, double y, double z, double rx, double ry) {
                add_static_box_body(x, y, z - 0.5, rx, ry, 0.5,
                                    JPH::Quat::sIdentity(), Layers::ROAD_SURFACE,
                                    pack_body_tag(BodyTagKind::RoadSurface, -2), 1.0);
            };
            if (scenario->park_out.enabled) {
                add_pad(0.5 * (scenario->park_out.slot_x + scenario->park_out.exit_x),
                        0.5 * (scenario->park_out.slot_y + scenario->park_out.exit_y),
                        scenario->park_out.slot_z, 24.0, 18.0);
            }
            if (scenario->parking_slot_count > 0 || scenario->parking_goal.enabled) {
                double sminx = 1e12, smaxx = -1e12, sminy = 1e12, smaxy = -1e12;
                double sz = scenario->parking_goal.enabled ? scenario->parking_goal.z : 0.0;
                bool any = false;
                if (scenario->parking_goal.enabled && std::fabs(scenario->parking_goal.z) < 1.5) {
                    sminx = smaxx = scenario->parking_goal.x;
                    sminy = smaxy = scenario->parking_goal.y;
                    sz = scenario->parking_goal.z;
                    any = true;
                }
                for (int si = 0; si < scenario->parking_slot_count; ++si) {
                    const auto& ps = scenario->parking_slots[si];
                    if (std::fabs(ps.z) < 1.5) {
                        sminx = any ? std::min(sminx, ps.x) : ps.x;
                        smaxx = any ? std::max(smaxx, ps.x) : ps.x;
                        sminy = any ? std::min(sminy, ps.y) : ps.y;
                        smaxy = any ? std::max(smaxy, ps.y) : ps.y;
                        sz = ps.z;
                        any = true;
                    }
                }
                if (any) {
                    add_pad(0.5 * (sminx + smaxx), 0.5 * (sminy + smaxy), sz,
                            0.5 * (smaxx - sminx) + 28.0, 0.5 * (smaxy - sminy) + 22.0);
                }
            }
        }

        // Ground datum slab around [-25km, +25km] excluding any underground/open_ramp cutout
        constexpr double W = 25000.0;
        if (subsurface_cutouts.empty()) {
            add_static_box_body(0.0, 0.0, -0.5, W, W, 0.5,
                                JPH::Quat::sIdentity(), Layers::ROAD_SURFACE,
                                pack_body_tag(BodyTagKind::RoadSurface, -3), 1.0);
        } else {
            AABB2D u = subsurface_cutouts[0];
            for (size_t i = 1; i < subsurface_cutouts.size(); ++i) {
                u.min_x = std::min(u.min_x, subsurface_cutouts[i].min_x);
                u.max_x = std::max(u.max_x, subsurface_cutouts[i].max_x);
                u.min_y = std::min(u.min_y, subsurface_cutouts[i].min_y);
                u.max_y = std::max(u.max_y, subsurface_cutouts[i].max_y);
            }
            // Partition z=0 earth plane into 4 non-overlapping slabs around subsurface pit [u.min..u.max]
            auto add_rect = [&](double x0, double x1, double y0, double y1) {
                if (x1 - x0 < 0.5 || y1 - y0 < 0.5) return;
                add_static_box_body(0.5 * (x0 + x1), 0.5 * (y0 + y1), -0.5,
                                    0.5 * (x1 - x0), 0.5 * (y1 - y0), 0.5,
                                    JPH::Quat::sIdentity(), Layers::ROAD_SURFACE,
                                    pack_body_tag(BodyTagKind::RoadSurface, -4), 1.0);
            };
            add_rect(-W, u.min_x, -W, W);
            add_rect(u.max_x, W, -W, W);
            add_rect(u.min_x, u.max_x, -W, u.min_y);
            add_rect(u.min_x, u.max_x, u.max_y, W);
        }
    }

    bool raycast_road(double x, double y, double z_top, double max_dist,
                      double& out_z, double& out_nx, double& out_ny, double& out_nz) const {
        if (!physics_system) return false;
        JPH::RRayCast ray(JPH::RVec3(x, y, z_top), JPH::Vec3(0.0f, 0.0f, -(float)max_dist));
        JPH::RayCastResult hit;
        JPH::SpecifiedBroadPhaseLayerFilter bp_filter(BroadPhaseLayers::NON_MOVING);
        JPH::SpecifiedObjectLayerFilter obj_filter(Layers::ROAD_SURFACE);
        if (!physics_system->GetNarrowPhaseQuery().CastRay(ray, hit, bp_filter, obj_filter)) {
            return false;
        }
        JPH::RVec3 hp = ray.GetPointOnRay(hit.mFraction);
        out_z = (double)hp.GetZ();
        JPH::BodyLockRead lock(physics_system->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hp);
            out_nx = (double)n.GetX();
            out_ny = (double)n.GetY();
            out_nz = (double)n.GetZ();
        } else {
            out_nx = 0.0; out_ny = 0.0; out_nz = 1.0;
        }
        return true;
    }

    JPH::BodyID create_npc_body(int entity_idx, const Entity& e) {
        double h = (e.height > 0.4) ? e.height : 1.5;
        double hx = std::max(0.4, 0.5 * e.length);
        double hy = std::max(0.4, 0.5 * e.width);
        double hz = std::max(0.3, 0.5 * (h - kGroundClearanceM));
        double z_box_center = kGroundClearanceM + hz;

        JPH::Ref<JPH::BoxShapeSettings> bss = new JPH::BoxShapeSettings(
            JPH::Vec3((float)hx, (float)hy, (float)hz), 0.04f);
        JPH::Ref<JPH::RotatedTranslatedShapeSettings> rtss =
            new JPH::RotatedTranslatedShapeSettings(
                JPH::Vec3(0.0f, 0.0f, (float)z_box_center), JPH::Quat::sIdentity(), bss);
        JPH::ShapeSettings::ShapeResult res = rtss->Create();
        if (res.HasError()) return JPH::BodyID();

        bool is_static = e.parked_fixed || e.type == EntityType::UnknownStatic;
        JPH::EMotionType mt = is_static ? JPH::EMotionType::Static : JPH::EMotionType::Kinematic;
        JPH::ObjectLayer layer = (e.type == EntityType::UnknownStatic)
                                 ? Layers::STATIC_OBSTACLE : Layers::VEHICLE;

        JPH::Quat q = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)e.heading);
        JPH::BodyCreationSettings bcs(res.Get(), JPH::RVec3(e.x, e.y, e.z), q, mt, layer);
        bcs.mFriction = 0.6f;
        bcs.mRestitution = 0.05f;
        bcs.mUserData = (e.type == EntityType::UnknownStatic)
                        ? pack_body_tag(BodyTagKind::StaticObstacle, entity_idx, intern_kind("unknown_static"))
                        : pack_body_tag(BodyTagKind::Vehicle, entity_idx);
        JPH::Body* b = physics_system->GetBodyInterface().CreateBody(bcs);
        if (!b) return JPH::BodyID();
        physics_system->GetBodyInterface().AddBody(
            b->GetID(), is_static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        return b->GetID();
    }

    double last_step_ms{0.0};
    bool is_ready{false};

    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemSingleThreaded> job_system;
    BPLayerInterfaceImpl bp_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl obj_vs_bp_filter;
    ObjectLayerPairFilterImpl obj_layer_pair_filter;
    std::unique_ptr<JPH::PhysicsSystem> physics_system;
    WorldContactListener contact_listener;

    std::vector<JPH::BodyID> static_bodies;
    std::vector<GateSlot> gates;
    std::vector<FrictionPatch> friction_patches;
    std::unordered_map<int, VehicleSlot> vehicles;
    std::unordered_map<int, NpcBodySlot> npcs;

    std::mutex contact_mutex;
    std::vector<PhysicsContactEvent> pending_contacts;

    std::mutex tag_mutex;
    std::vector<std::string> kind_strings;
    std::unordered_map<std::string, uint16_t> kind_lookup;
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

void PhysicsWorld::reset() {
    if (impl_) impl_->clear_all();
}

bool PhysicsWorld::ready() const {
    return impl_ && impl_->is_ready && impl_->physics_system;
}

bool PhysicsWorld::build_static_world(FlowRoadNetwork* roads,
                                      const ScenarioConfig* scenario,
                                      const std::vector<BuildingOBB>& buildings,
                                      const std::vector<StaticBoxObstacle>& facilities) {
    if (!ready()) return false;
    impl_->clear_all();

    if (scenario && scenario->friction_zone_count > 0) {
        std::vector<FrictionPatch> fps;
        fps.reserve((size_t)scenario->friction_zone_count);
        for (int i = 0; i < scenario->friction_zone_count; ++i) {
            const auto& fz = scenario->friction_zones[i];
            fps.push_back({fz.x_min, fz.x_max, fz.y_min, fz.y_max, fz.mu_scale});
        }
        impl_->friction_patches = std::move(fps);
    }

    std::vector<AABB2D> subsurface_cutouts;
    if (roads && roads->loaded()) {
        impl_->add_road_network_meshes(*roads, subsurface_cutouts);
    }
    impl_->add_scenario_parking_and_ground(scenario, subsurface_cutouts);

    for (size_t i = 0; i < buildings.size(); ++i) {
        impl_->add_building_collider(buildings[i], (int)i, roads);
    }
    for (size_t i = 0; i < facilities.size(); ++i) {
        add_static_obstacle(facilities[i], (int)i);
    }
    if (scenario) {
        const uint16_t kcz = impl_->intern_kind("construction");
        for (int i = 0; i < scenario->construction_zone_count; ++i) {
            const auto& cz = scenario->construction_zones[i];
            impl_->add_static_box_body(
                cz.x, cz.y, 0.55,
                std::max(0.5, cz.length * 0.5),
                std::max(0.5, cz.width * 0.5),
                0.55,
                JPH::Quat::sIdentity(), Layers::STATIC_OBSTACLE,
                pack_body_tag(BodyTagKind::Construction, i, kcz), 0.7);
        }
    }

    impl_->physics_system->OptimizeBroadPhase();
    return true;
}

void PhysicsWorld::add_ground_slab(double cx, double cy, double top_z,
                                   double half_x, double half_y, double friction) {
    if (!ready()) return;
    impl_->add_static_box_body(cx, cy, top_z - 0.5, half_x, half_y, 0.5,
                               JPH::Quat::sIdentity(), Layers::ROAD_SURFACE,
                               pack_body_tag(BodyTagKind::RoadSurface, -1), friction);
}

void PhysicsWorld::add_ramp_surface(double x0, double y0, double z0,
                                    double x1, double y1, double z1,
                                    double width, double friction) {
    if (!ready()) return;
    double dx = x1 - x0, dy = y1 - y0;
    double len = std::hypot(dx, dy);
    if (len < 0.1 || width < 0.2) return;
    double nx = -dy / len * (0.5 * width);
    double ny = dx / len * (0.5 * width);
    std::vector<JPH::Vec3> verts = {
        JPH::Vec3((float)(x0 - nx), (float)(y0 - ny), (float)z0),
        JPH::Vec3((float)(x0 + nx), (float)(y0 + ny), (float)z0),
        JPH::Vec3((float)(x1 - nx), (float)(y1 - ny), (float)z1),
        JPH::Vec3((float)(x1 + nx), (float)(y1 + ny), (float)z1),
    };
    std::vector<JPH::IndexedTriangle> tris = {
        JPH::IndexedTriangle(0, 2, 3),
        JPH::IndexedTriangle(0, 3, 1),
    };
    impl_->add_static_mesh_centered(verts, tris, Layers::ROAD_SURFACE,
                                    pack_body_tag(BodyTagKind::RoadSurface, -5), friction);
}

void PhysicsWorld::add_mesh_triangles(const std::vector<double>& xyz_triplets, double friction) {
    if (!ready() || xyz_triplets.size() < 9) return;
    size_t n_pts = xyz_triplets.size() / 3;
    size_t n_tri = n_pts / 3;
    std::vector<JPH::Vec3> verts;
    std::vector<JPH::IndexedTriangle> tris;
    verts.reserve(n_tri * 3);
    tris.reserve(n_tri);
    for (size_t t = 0; t < n_tri; ++t) {
        size_t o = t * 9;
        JPH::uint32 base = (JPH::uint32)verts.size();
        verts.emplace_back((float)xyz_triplets[o + 0], (float)xyz_triplets[o + 1], (float)xyz_triplets[o + 2]);
        verts.emplace_back((float)xyz_triplets[o + 3], (float)xyz_triplets[o + 4], (float)xyz_triplets[o + 5]);
        verts.emplace_back((float)xyz_triplets[o + 6], (float)xyz_triplets[o + 7], (float)xyz_triplets[o + 8]);
        tris.emplace_back(base, base + 1, base + 2);
    }
    impl_->add_static_mesh_centered(verts, tris, Layers::ROAD_SURFACE,
                                    pack_body_tag(BodyTagKind::RoadSurface, -6), friction);
}

void PhysicsWorld::add_static_obstacle(const StaticBoxObstacle& obs, int obstacle_index) {
    if (!ready()) return;
    double h = (obs.height > 0.05) ? obs.height : 1.5;
    uint16_t kidx = impl_->intern_kind(obs.kind.empty() ? "facility" : obs.kind);
    uint64_t tag = pack_body_tag(BodyTagKind::StaticObstacle, obstacle_index, kidx);

    bool use_cyl = obs.is_cylinder || obs.kind == "pole" || obs.kind == "tree";
    if (use_cyl) {
        float half_h = (float)(0.5 * h);
        float r = (float)std::max(0.12, 0.25 * (obs.len + obs.wid));
        JPH::CylinderShapeSettings css(half_h, r, 0.02f);
        JPH::ShapeSettings::ShapeResult res = css.Create();
        if (res.HasError()) return;
        // Align Jolt Y-axis cylinder vertically along ENU +Z
        JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI);
        JPH::BodyCreationSettings bcs(res.Get(), JPH::RVec3(obs.x, obs.y, obs.z + half_h), rot,
                                      JPH::EMotionType::Static, Layers::STATIC_OBSTACLE);
        bcs.mFriction = 0.65f;
        bcs.mUserData = tag;
        JPH::Body* b = impl_->physics_system->GetBodyInterface().CreateBody(bcs);
        if (b) {
            impl_->physics_system->GetBodyInterface().AddBody(b->GetID(), JPH::EActivation::DontActivate);
            impl_->static_bodies.push_back(b->GetID());
        }
        return;
    }

    impl_->add_static_box_body(
        obs.x, obs.y, obs.z + 0.5 * h,
        std::max(0.08, 0.5 * obs.len),
        std::max(0.08, 0.5 * obs.wid),
        0.5 * h,
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)obs.heading),
        Layers::STATIC_OBSTACLE, tag, 0.65);
}

void PhysicsWorld::set_friction_zones(std::vector<FrictionPatch> patches) {
    if (impl_) impl_->friction_patches = std::move(patches);
}

int PhysicsWorld::add_gate_barrier(double x, double y, double z,
                                   double len, double wid, double height,
                                   double heading, int pool_entity_idx) {
    if (!ready()) return -1;
    JPH::BoxShapeSettings bss(
        JPH::Vec3((float)std::max(0.15, 0.5 * len),
                  (float)std::max(0.5, 0.5 * wid),
                  (float)std::max(0.2, 0.5 * height)),
        0.02f);
    JPH::ShapeSettings::ShapeResult res = bss.Create();
    if (res.HasError()) return -1;

    uint16_t kidx = impl_->intern_kind("gate");
    JPH::BodyCreationSettings bcs(
        res.Get(), JPH::RVec3(x, y, z + 0.5 * height),
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)heading),
        JPH::EMotionType::Static, Layers::STATIC_OBSTACLE);
    bcs.mFriction = 0.6f;
    bcs.mUserData = pack_body_tag(BodyTagKind::Gate, pool_entity_idx, kidx);
    JPH::Body* b = impl_->physics_system->GetBodyInterface().CreateBody(bcs);
    if (!b) return -1;

    impl_->physics_system->GetBodyInterface().AddBody(b->GetID(), JPH::EActivation::DontActivate);
    impl_->gates.push_back({b->GetID(), true});
    return (int)impl_->gates.size() - 1;
}

void PhysicsWorld::set_gate_open(int gate_handle, bool open) {
    if (!ready() || gate_handle < 0 || gate_handle >= (int)impl_->gates.size()) return;
    auto& g = impl_->gates[gate_handle];
    JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();
    if (open && g.active_in_world) {
        bi.RemoveBody(g.body_id);
        g.active_in_world = false;
    } else if (!open && !g.active_in_world) {
        bi.AddBody(g.body_id, JPH::EActivation::DontActivate);
        g.active_in_world = true;
    }
}

bool PhysicsWorld::register_vehicle(int entity_idx, const Entity& e, bool abs_enabled) {
    if (!ready()) return false;
    JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();

    // Remove previous binding for same entity index if re-registering
    auto existing = impl_->vehicles.find(entity_idx);
    if (existing != impl_->vehicles.end()) {
        if (existing->second.constraint) {
            impl_->physics_system->RemoveStepListener(existing->second.constraint);
            impl_->physics_system->RemoveConstraint(existing->second.constraint);
        }
        if (!existing->second.body_id.IsInvalid()) {
            bi.RemoveBody(existing->second.body_id);
            bi.DestroyBody(existing->second.body_id);
        }
        impl_->vehicles.erase(existing);
    }

    const double len   = (e.length > 1.0) ? e.length : 4.6;
    const double wid   = (e.width  > 0.8) ? e.width  : 2.0;
    const double hgt   = (e.height > 0.6) ? e.height : 1.5;
    const double wb    = (e.wheelbase > 0.8) ? e.wheelbase : 2.7;
    const double mass  = (e.mass > 100.0) ? e.mass : 1500.0;
    const double cg_h  = (e.cg_height > 0.2) ? e.cg_height : 0.55;
    const double iz    = (e.yaw_inertia > 100.0) ? e.yaw_inertia : 2250.0;

    // Ground-datum local frame: z=0 sits at flat resting tire contact patch, CoM at z=cg_h
    const double clearance = (e.type == EntityType::Truck) ? 0.28 : kGroundClearanceM;
    const double box_h = std::max(0.45, hgt - clearance);
    const double box_z_center = clearance + 0.5 * box_h;

    JPH::Ref<JPH::BoxShapeSettings> chassis_box = new JPH::BoxShapeSettings(
        JPH::Vec3((float)(0.5 * (len - 0.10)),
                  (float)(0.5 * (wid - 0.08)),
                  (float)(0.5 * box_h)),
        0.04f);
    JPH::Ref<JPH::RotatedTranslatedShapeSettings> positioned_box =
        new JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0.0f, 0.0f, (float)box_z_center), JPH::Quat::sIdentity(), chassis_box);
    JPH::Ref<JPH::OffsetCenterOfMassShapeSettings> com_shape =
        new JPH::OffsetCenterOfMassShapeSettings(
            JPH::Vec3(0.0f, 0.0f, (float)(cg_h - box_z_center)), positioned_box);
    JPH::ShapeSettings::ShapeResult shape_res = com_shape->Create();
    if (shape_res.HasError()) return false;

    double Ixx = mass * (wid * wid + box_h * box_h) / 12.0;
    double Iyy = mass * (len * len + box_h * box_h) / 12.0;
    JPH::MassProperties mp;
    mp.mMass = (float)mass;
    mp.mInertia = JPH::Mat44::sScale(JPH::Vec3((float)Ixx, (float)Iyy, (float)iz));

    JPH::Quat q0 = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)e.heading);
    JPH::BodyCreationSettings bcs(shape_res.Get(), JPH::RVec3(e.x, e.y, e.z), q0,
                                  JPH::EMotionType::Dynamic, Layers::VEHICLE);
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    bcs.mMassPropertiesOverride = mp;
    bcs.mAllowSleeping = false;
    bcs.mLinearDamping = 0.0f;
    bcs.mAngularDamping = 0.01f;
    bcs.mUserData = pack_body_tag(BodyTagKind::Vehicle, entity_idx);

    JPH::Body* car_body = bi.CreateBody(bcs);
    if (!car_body) return false;

    const double wheel_r = (e.type == EntityType::Truck) ? 0.48 :
                           (e.type == EntityType::SUV   ? 0.37 : 0.34);
    const double wheel_w = (e.type == EntityType::Truck) ? 0.28 : 0.22;
    const double front_x = +0.45 * wb;
    const double rear_x  = -0.55 * wb;
    const double track_y = +0.42 * wid;

    // Neutral loaded wheel center at z = wheel_r; static 1g sag ~ g/(2*pi*f)^2 ~ 0.097m
    const float f_susp = (e.type == EntityType::Truck) ? 1.45f : 1.65f;
    const double static_sag = kGravity / (4.0 * M_PI * M_PI * f_susp * f_susp);
    const double susp_rest_len = 0.14;
    const double susp_min = 0.04;
    const double susp_max = susp_rest_len + static_sag;
    const float attach_z = (float)(wheel_r + susp_rest_len);

    const double lat_b  = (e.pacejka_b > 0.5) ? e.pacejka_b : 10.0;
    const double lat_c  = (e.pacejka_c > 0.5) ? e.pacejka_c : 1.9;
    const double lat_e  = e.pacejka_e;
    const double mu_nom = (e.pacejka_mu > 0.1) ? e.pacejka_mu : 0.95;

    const double mass_scale = mass / 1500.0;
    const float max_brake_tq = (float)(950.0 * mass_scale);
    const float wheel_inertia = (e.type == EntityType::Truck) ? 3.2f : 1.0f;

    JPH::VehicleConstraintSettings vcs;
    vcs.mUp = JPH::Vec3(0.0f, 0.0f, 1.0f);
    vcs.mForward = JPH::Vec3(1.0f, 0.0f, 0.0f);
    vcs.mMaxPitchRollAngle = JPH::DegreesToRadians(75.0f);

    const struct { double x; double y; bool steerable; } w_defs[4] = {
        {front_x, +track_y, true},   // 0: FL
        {front_x, -track_y, true},   // 1: FR
        {rear_x,  +track_y, false},  // 2: RL
        {rear_x,  -track_y, false},  // 3: RR
    };

    for (int i = 0; i < 4; ++i) {
        auto* ws = new JPH::WheelSettingsWV();
        ws->mPosition = JPH::Vec3((float)w_defs[i].x, (float)w_defs[i].y, attach_z);
        ws->mSuspensionDirection = JPH::Vec3(0.0f, 0.0f, -1.0f);
        ws->mSteeringAxis = JPH::Vec3(0.0f, 0.0f, 1.0f);
        ws->mWheelUp = JPH::Vec3(0.0f, 0.0f, 1.0f);
        ws->mWheelForward = JPH::Vec3(1.0f, 0.0f, 0.0f);
        ws->mSuspensionMinLength = (float)susp_min;
        ws->mSuspensionMaxLength = (float)susp_max;
        ws->mSuspensionSpring = JPH::SpringSettings(
            JPH::ESpringMode::FrequencyAndDamping, f_susp, 0.65f);
        ws->mRadius = (float)wheel_r;
        ws->mWidth = (float)wheel_w;
        ws->mInertia = wheel_inertia;
        ws->mAngularDamping = 0.02f;
        ws->mMaxSteerAngle = w_defs[i].steerable ? 0.60f : 0.0f;
        ws->mMaxBrakeTorque = max_brake_tq;
        ws->mMaxHandBrakeTorque = w_defs[i].steerable ? 0.0f : (max_brake_tq * 2.5f);
        ws->mEnableSuspensionForcePoint = true;
        ws->mSuspensionForcePoint = JPH::Vec3(
            (float)w_defs[i].x, (float)w_defs[i].y, (float)(wheel_r * 0.5));
        populate_pacejka_tire_curves(*ws, lat_b, lat_c, lat_e, mu_nom);
        vcs.mWheels.push_back(ws);
    }

    // Anti-roll bars balance realistic lateral body lean with rollover resistance
    float arb_k = (float)(2200.0 * mass_scale);
    vcs.mAntiRollBars.resize(2);
    vcs.mAntiRollBars[0].mLeftWheel = 0;
    vcs.mAntiRollBars[0].mRightWheel = 1;
    vcs.mAntiRollBars[0].mStiffness = arb_k * 1.25f;
    vcs.mAntiRollBars[1].mLeftWheel = 2;
    vcs.mAntiRollBars[1].mRightWheel = 3;
    vcs.mAntiRollBars[1].mStiffness = arb_k * 0.85f;

    // EV instant-torque controller calibrated to FlowEngine throttle/brake force range
    auto* ctrl_settings = new JPH::WheeledVehicleControllerSettings();
    ctrl_settings->mEngine.mMinRPM = 0.0f;
    ctrl_settings->mEngine.mMaxRPM = 12000.0f;
    ctrl_settings->mEngine.mMaxTorque = (float)(380.0 * (wheel_r / 0.34) * mass_scale);
    ctrl_settings->mEngine.mInertia = 0.25f;
    ctrl_settings->mEngine.mAngularDamping = 0.02f;
    ctrl_settings->mEngine.mNormalizedTorque.Clear();
    ctrl_settings->mEngine.mNormalizedTorque.AddPoint(0.0f, 1.0f);
    ctrl_settings->mEngine.mNormalizedTorque.AddPoint(0.55f, 1.0f);
    ctrl_settings->mEngine.mNormalizedTorque.AddPoint(1.0f, 0.72f);

    ctrl_settings->mTransmission.mMode = JPH::ETransmissionMode::Manual;
    ctrl_settings->mTransmission.mGearRatios = { 4.5f };
    ctrl_settings->mTransmission.mReverseGearRatios = { -4.5f };
    ctrl_settings->mTransmission.mSwitchTime = 0.0f;
    ctrl_settings->mTransmission.mClutchReleaseTime = 0.0f;
    ctrl_settings->mTransmission.mClutchStrength = (float)(20.0 * mass_scale);

    // AWD 40/60 split keeps climbing traction stable on steep parking/viaduct ramps
    ctrl_settings->mDifferentials.resize(2);
    ctrl_settings->mDifferentials[0].mLeftWheel = 0;
    ctrl_settings->mDifferentials[0].mRightWheel = 1;
    ctrl_settings->mDifferentials[0].mDifferentialRatio = 1.0f;
    ctrl_settings->mDifferentials[0].mEngineTorqueRatio = 0.4f;
    ctrl_settings->mDifferentials[1].mLeftWheel = 2;
    ctrl_settings->mDifferentials[1].mRightWheel = 3;
    ctrl_settings->mDifferentials[1].mDifferentialRatio = 1.0f;
    ctrl_settings->mDifferentials[1].mEngineTorqueRatio = 0.6f;
    vcs.mController = ctrl_settings;

    JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*car_body, vcs);
    JPH::Ref<JPH::VehicleCollisionTesterRay> tester = new JPH::VehicleCollisionTesterRay(
        Layers::WHEEL_RAY, JPH::Vec3(0.0f, 0.0f, 1.0f), JPH::DegreesToRadians(75.0f));
    constraint->SetVehicleCollisionTester(tester);

    Impl::VehicleSlot& slot = impl_->vehicles[entity_idx];
    slot.entity_idx       = entity_idx;
    slot.body_id          = car_body->GetID();
    slot.constraint       = constraint;
    slot.wheel_tester     = tester;
    slot.mass             = mass;
    slot.wheelbase        = wb;
    slot.cg_height        = cg_h;
    slot.drag_coeff       = (e.drag_coeff > 0.0) ? e.drag_coeff : 0.3;
    slot.pacejka_mu       = mu_nom;
    slot.wheel_radius     = wheel_r;
    slot.max_steer_rad    = 0.60;
    slot.max_brake_torque = max_brake_tq;
    slot.abs_enabled      = abs_enabled;
    slot.base_mu_scale    = (e.surface_mu_scale > 0.05) ? e.surface_mu_scale : 1.0;

    // Spatial friction & hill-hold combined-slip Pacejka tire callback
    constraint->SetCombineFriction(
        [impl_ptr = impl_.get(), entity_idx](
            JPH::uint wheel_idx, float& io_long_mu, float& io_lat_mu,
            const JPH::Body& ground_body, const JPH::SubShapeID&) {
            auto it = impl_ptr->vehicles.find(entity_idx);
            if (it == impl_ptr->vehicles.end() || wheel_idx >= 4) return;
            Impl::VehicleSlot& vs = it->second;
            const JPH::Wheel* w = vs.constraint->GetWheel(wheel_idx);
            double mu_patch = vs.base_mu_scale;
            if (w && w->HasContact()) {
                JPH::RVec3 cp = w->GetContactPosition();
                mu_patch = impl_ptr->sample_friction_at((double)cp.GetX(), (double)cp.GetY(),
                                                        vs.base_mu_scale);
            }
            float surface_mu = ground_body.GetFriction() * (float)mu_patch;
            vs.wheel_mu[wheel_idx] = (double)surface_mu;
            io_long_mu *= surface_mu;
            io_lat_mu  *= surface_mu;
        });

    auto* controller = static_cast<JPH::WheeledVehicleController*>(constraint->GetController());
    controller->SetTireMaxImpulseCallback(
        [impl_ptr = impl_.get(), entity_idx](
            JPH::uint wheel_idx, float& out_long_imp, float& out_lat_imp,
            float susp_imp, float long_fric, float lat_fric,
            float long_slip, float /*lat_slip*/, float /*dt*/) {
            auto it = impl_ptr->vehicles.find(entity_idx);
            float mu_eff = 1.0f;
            bool braking = false;
            if (it != impl_ptr->vehicles.end() && wheel_idx < 4) {
                mu_eff = (float)(it->second.pacejka_mu * it->second.wheel_mu[wheel_idx]);
                braking = (it->second.brake > 0.05);
            }
            // Static brake stiction prevents slow numerical creep when held stopped on steep slopes
            float eff_long = braking ? std::max(long_fric, 0.92f * mu_eff) : long_fric;
            // Friction ellipse couples longitudinal slip utilization to remaining lateral capacity
            float slip_norm = std::min(1.0f, std::fabs(long_slip) / 0.35f);
            float lat_couple = std::sqrt(std::max(0.18f, 1.0f - 0.72f * slip_norm * slip_norm));
            out_long_imp = eff_long * susp_imp;
            out_lat_imp  = lat_fric * lat_couple * susp_imp;
        });

    bi.AddBody(car_body->GetID(), JPH::EActivation::Activate);
    impl_->physics_system->AddConstraint(constraint);
    impl_->physics_system->AddStepListener(constraint);

    if (std::fabs(e.speed) > 1e-3) {
        set_vehicle_pose(entity_idx, e.x, e.y, e.z, e.heading, e.speed);
    }
    return true;
}

void PhysicsWorld::set_vehicle_input(int entity_idx, double throttle, double brake,
                                     double steer_rad, int gear, double mu_scale) {
    if (!ready()) return;
    auto it = impl_->vehicles.find(entity_idx);
    if (it == impl_->vehicles.end()) return;
    Impl::VehicleSlot& vs = it->second;
    vs.throttle      = std::clamp(throttle, 0.0, 1.0);
    vs.brake         = std::clamp(brake, 0.0, 1.0);
    vs.steer_rad     = std::clamp(steer_rad, -vs.max_steer_rad, vs.max_steer_rad);
    vs.gear          = (gear < 0) ? -1 : 1;
    vs.base_mu_scale = std::max(0.05, mu_scale);
}

void PhysicsWorld::set_vehicle_pose(int entity_idx, double x, double y, double z,
                                    double heading, double speed) {
    if (!ready()) return;
    auto it = impl_->vehicles.find(entity_idx);
    if (it == impl_->vehicles.end()) return;
    Impl::VehicleSlot& vs = it->second;
    JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();
    JPH::Quat q = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)heading);
    bi.SetPositionAndRotation(vs.body_id, JPH::RVec3(x, y, z), q, JPH::EActivation::Activate);

    JPH::Vec3 fwd = q.RotateAxisX();
    bi.SetLinearVelocity(vs.body_id, fwd * (float)speed);
    bi.SetAngularVelocity(vs.body_id, JPH::Vec3::sZero());

    float omega = (float)(speed / std::max(0.15, vs.wheel_radius));
    for (JPH::Wheel* w : vs.constraint->GetWheels()) {
        w->SetAngularVelocity(omega);
    }
    vs.prev_vx_body = speed;
    vs.prev_vy_body = 0.0;
    vs.last_ax_body = 0.0;
    vs.last_ay_body = 0.0;
}

void PhysicsWorld::sync_npcs_from_pool(EntityPool& pool, double dt) {
    if (!ready()) return;
    JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();

    // Remove NPC bodies for entities that disappeared or became inactive
    for (auto it = impl_->npcs.begin(); it != impl_->npcs.end(); ) {
        int idx = it->first;
        bool valid = (idx > 0 && idx < pool.size() && pool[idx].active &&
                      (pool[idx].is_vehicle() || pool[idx].type == EntityType::UnknownStatic));
        if (!valid) {
            bi.RemoveBody(it->second.body_id);
            bi.DestroyBody(it->second.body_id);
            it = impl_->npcs.erase(it);
        } else {
            ++it;
        }
    }

    for (int idx = 1; idx < pool.size(); ++idx) {
        Entity& e = pool[idx];
        if (!e.active) continue;

        if (e.type == EntityType::ETCGate) {
            // Sync gate barrier if registered via gate_id tracking, handled cleanly in flowsim
            continue;
        }
        if (!e.is_vehicle() && e.type != EntityType::UnknownStatic) continue;

        auto it = impl_->npcs.find(idx);
        if (it == impl_->npcs.end()) {
            JPH::BodyID bid = impl_->create_npc_body(idx, e);
            if (!bid.IsInvalid()) {
                Impl::NpcBodySlot nbs;
                nbs.entity_idx = idx;
                nbs.body_id    = bid;
                nbs.is_static  = e.parked_fixed || e.type == EntityType::UnknownStatic;
                nbs.cg_height  = e.cg_height;
                nbs.last_z     = e.z;
                impl_->npcs[idx] = nbs;
            }
            continue;
        }

        Impl::NpcBodySlot& slot = it->second;
        if (slot.is_static) continue;

        // Ground-raycast onto road deck near NPC current Z so bridges overhead don't grab ground NPCs
        double rz = 0.0, nx = 0.0, ny = 0.0, nz = 1.0;
        double pitch = 0.0, roll = 0.0;
        if (impl_->raycast_road(e.x, e.y, e.z + 1.6, 3.2, rz, nx, ny, nz)) {
            e.z = rz;
            slot.vz = 0.0;
            double ch = std::cos(e.heading), sh = std::sin(e.heading);
            double lx = -sh, ly = ch;
            double n_fwd  = nx * ch + ny * sh;
            double n_left = nx * lx + ny * ly;
            pitch = std::asin(std::clamp(n_fwd, -0.35, 0.35));
            roll  = -std::asin(std::clamp(n_left, -0.35, 0.35));
        } else if (dt > 1e-4) {
            // Off road deck & unsupported: integrate real free-fall gravity
            double sub_rz = 0.0;
            slot.vz -= kGravity * dt;
            double z_next = e.z + slot.vz * dt;
            if (impl_->raycast_road(e.x, e.y, e.z + 0.5, 50.0, sub_rz, nx, ny, nz) && z_next <= sub_rz) {
                z_next = sub_rz;
                slot.vz = 0.0;
            }
            e.z = z_next;
        }
        e.vz    = slot.vz;
        e.pitch = pitch;
        e.roll  = roll;
        slot.last_z     = e.z;
        slot.last_pitch = pitch;
        slot.last_roll  = roll;

        JPH::Quat q_yaw   = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), (float)e.heading);
        JPH::Quat q_pitch = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), (float)pitch);
        JPH::Quat q_roll  = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), (float)roll);
        JPH::Quat q_world = q_yaw * q_pitch * q_roll;

        JPH::RVec3 cur_pos = bi.GetPosition(slot.body_id);
        double d_xy = std::hypot(e.x - (double)cur_pos.GetX(), e.y - (double)cur_pos.GetY());
        double d_z  = std::fabs(e.z - (double)cur_pos.GetZ());
        if (dt <= 1e-4 || d_xy > 8.0 || d_z > 3.0) {
            bi.SetPositionAndRotation(slot.body_id, JPH::RVec3(e.x, e.y, e.z), q_world,
                                      JPH::EActivation::Activate);
        } else {
            bi.MoveKinematic(slot.body_id, JPH::RVec3(e.x, e.y, e.z), q_world, (float)dt);
        }
    }
}

void PhysicsWorld::step(double dt) {
    if (!ready() || dt <= 0.0) return;

    const auto t0 = std::chrono::steady_clock::now();
    JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();

    int n_sub = std::max(1, (int)std::ceil(dt / kMaxSubstepSec));
    double sub_dt = dt / (double)n_sub;

    for (int s = 0; s < n_sub; ++s) {
        for (auto& kv : impl_->vehicles) {
            Impl::VehicleSlot& vs = kv.second;
            auto* ctrl = static_cast<JPH::WheeledVehicleController*>(vs.constraint->GetController());

            bi.ActivateBody(vs.body_id);
            JPH::Vec3 vel = bi.GetLinearVelocity(vs.body_id);
            double v_mag = (double)vel.Length();

            // ABS slip-ratio modulation under hard braking above walking speed
            double eff_brake = vs.brake;
            if (vs.abs_enabled && eff_brake > 0.25 && v_mag > 2.5) {
                double max_slip = 0.0;
                for (const JPH::Wheel* wb : vs.constraint->GetWheels()) {
                    const auto* wv = static_cast<const JPH::WheelWV*>(wb);
                    if (wv->HasContact()) {
                        max_slip = std::max(max_slip, (double)wv->mLongitudinalSlip);
                    }
                }
                if (max_slip > 0.18) {
                    double relief = std::clamp((max_slip - 0.18) * 1.4, 0.0, 0.55);
                    eff_brake *= (1.0 - relief);
                }
            }

            float clutch = (vs.throttle > 0.01) ? 1.0f : 0.0f;
            ctrl->GetTransmission().Set(vs.gear, clutch);

            // Jolt WheeledVehicleController uses inRight > 0 for right turn (negative ENU +Z steer)
            float right_input = (float)(-vs.steer_rad / vs.max_steer_rad);
            ctrl->SetDriverInput((float)vs.throttle, right_input, (float)eff_brake, 0.0f);

            // Aerodynamic drag + tire rolling resistance opposing world velocity
            if (v_mag > 1e-3) {
                double f_drag = vs.drag_coeff * v_mag * v_mag + 0.012 * vs.mass * kGravity;
                JPH::Vec3 dir = vel / (float)v_mag;
                bi.AddForce(vs.body_id, dir * (float)(-f_drag));
            }
        }

        impl_->physics_system->Update((float)sub_dt, 1,
                                      impl_->temp_allocator.get(), impl_->job_system.get());

        // Capture last substep tire normal loads & body-frame acceleration
        for (auto& kv : impl_->vehicles) {
            Impl::VehicleSlot& vs = kv.second;
            vs.last_sub_dt = sub_dt;
            for (int wi = 0; wi < 4; ++wi) {
                const JPH::Wheel* w = vs.constraint->GetWheel((JPH::uint)wi);
                vs.wheel_fz[wi] = (w && w->HasContact())
                                  ? std::max(0.0, (double)w->GetSuspensionLambda() / sub_dt)
                                  : 0.0;
            }
            JPH::Quat q = bi.GetRotation(vs.body_id);
            JPH::Vec3 vel = bi.GetLinearVelocity(vs.body_id);
            double vx_b = (double)vel.Dot(q.RotateAxisX());
            double vy_b = (double)vel.Dot(q.RotateAxisY());
            vs.last_ax_body = (vx_b - vs.prev_vx_body) / sub_dt;
            vs.last_ay_body = (vy_b - vs.prev_vy_body) / sub_dt;
            vs.prev_vx_body = vx_b;
            vs.prev_vy_body = vy_b;
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    impl_->last_step_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

bool PhysicsWorld::get_vehicle_telemetry(int entity_idx, VehiclePhysicsTelemetry& out) const {
    if (!ready()) return false;
    auto it = impl_->vehicles.find(entity_idx);
    if (it == impl_->vehicles.end()) return false;
    const Impl::VehicleSlot& vs = it->second;

    const JPH::BodyInterface& bi = impl_->physics_system->GetBodyInterface();
    JPH::RVec3 pos = bi.GetPosition(vs.body_id);
    JPH::Quat q    = bi.GetRotation(vs.body_id);
    JPH::Vec3 vel  = bi.GetLinearVelocity(vs.body_id);
    JPH::Vec3 avel = bi.GetAngularVelocity(vs.body_id);

    JPH::Vec3 fwd  = q.RotateAxisX();
    JPH::Vec3 left = q.RotateAxisY();
    JPH::Vec3 up   = q.RotateAxisZ();

    // Estimate ground datum height from loaded wheel contact patches when grounded
    double ground_z_sum = 0.0;
    int grounded_cnt = 0;
    for (int wi = 0; wi < 4; ++wi) {
        const auto* wv = static_cast<const JPH::WheelWV*>(vs.constraint->GetWheel((JPH::uint)wi));
        if (wv) {
            out.wheels[wi].grounded          = wv->HasContact();
            out.wheels[wi].suspension_length = (double)wv->GetSuspensionLength();
            out.wheels[wi].normal_force      = vs.wheel_fz[wi];
            out.wheels[wi].longitudinal_slip = (double)wv->mLongitudinalSlip;
            out.wheels[wi].lateral_slip      = (double)wv->mLateralSlip;
            out.wheels[wi].mu_effective      = vs.wheel_mu[wi];
            if (wv->HasContact()) {
                JPH::RVec3 cp = wv->GetContactPosition();
                out.wheels[wi].contact_x = (double)cp.GetX();
                out.wheels[wi].contact_y = (double)cp.GetY();
                out.wheels[wi].contact_z = (double)cp.GetZ();
                ground_z_sum += (double)cp.GetZ();
                grounded_cnt++;
            }
        }
    }
    (void)ground_z_sum;

    out.x       = (double)pos.GetX();
    out.y       = (double)pos.GetY();
    out.z       = (grounded_cnt > 0) ? (ground_z_sum / grounded_cnt) : (double)pos.GetZ();
    out.heading = std::atan2((double)fwd.GetY(), (double)fwd.GetX());
    out.pitch   = -std::asin(std::clamp((double)fwd.GetZ(), -1.0, 1.0));
    out.roll    = std::atan2((double)left.GetZ(), std::max(1e-4, (double)up.GetZ()));

    out.vx       = (double)vel.GetX();
    out.vy       = (double)vel.GetY();
    out.vz       = (double)vel.GetZ();
    out.v_x_body = (double)vel.Dot(fwd);
    out.v_y_body = (double)vel.Dot(left);
    out.speed    = out.v_x_body;

    out.yaw_rate   = (double)avel.Dot(up);
    out.pitch_rate = (double)avel.Dot(left);
    out.roll_rate  = (double)avel.Dot(fwd);

    out.a_x_body = vs.last_ax_body;
    out.a_y_body = vs.last_ay_body;
    out.F_zf     = vs.wheel_fz[0] + vs.wheel_fz[1];
    out.F_zr     = vs.wheel_fz[2] + vs.wheel_fz[3];
    return true;
}

bool PhysicsWorld::sync_entity_from_vehicle(int entity_idx, Entity& e) const {
    VehiclePhysicsTelemetry tel;
    if (!get_vehicle_telemetry(entity_idx, tel)) return false;

    e.x          = tel.x;
    e.y          = tel.y;
    e.z          = tel.z;
    e.heading    = tel.heading;
    e.pitch      = tel.pitch;
    e.roll       = tel.roll;
    e.vx         = tel.vx;
    e.vy         = tel.vy;
    e.vz         = tel.vz;
    e.v_x_body   = tel.v_x_body;
    e.v_y_body   = tel.v_y_body;
    e.speed      = tel.speed;
    e.yaw_rate   = tel.yaw_rate;
    e.pitch_rate = tel.pitch_rate;
    e.roll_rate  = tel.roll_rate;
    e.a_x_body   = tel.a_x_body;
    e.a_y_body   = tel.a_y_body;
    e.F_zf       = tel.F_zf;
    e.F_zr       = tel.F_zr;

    double f_y_front = 0.0, f_y_rear = 0.0;
    for (int wi = 0; wi < 4; ++wi) {
        const JPH::Wheel* w = impl_->vehicles[entity_idx].constraint->GetWheel((JPH::uint)wi);
        if (w && w->HasContact() && impl_->vehicles[entity_idx].last_sub_dt > 0.0) {
            double fy = (double)w->GetLateralLambda() / impl_->vehicles[entity_idx].last_sub_dt;
            if (wi < 2) f_y_front += fy;
            else        f_y_rear  += fy;
        }
    }
    e.F_yf = f_y_front;
    e.F_yr = f_y_rear;
    return true;
}

std::vector<PhysicsContactEvent> PhysicsWorld::consume_contacts() {
    std::vector<PhysicsContactEvent> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lk(impl_->contact_mutex);
    out.swap(impl_->pending_contacts);
    return out;
}

bool PhysicsWorld::raycast_ground(double x, double y, double z_top, double max_dist,
                                  double& out_z, double& out_nx, double& out_ny, double& out_nz) const {
    return impl_ && impl_->raycast_road(x, y, z_top, max_dist, out_z, out_nx, out_ny, out_nz);
}

double PhysicsWorld::last_step_time_ms() const {
    return impl_ ? impl_->last_step_ms : 0.0;
}

int PhysicsWorld::body_count() const {
    return (ready()) ? (int)impl_->physics_system->GetNumBodies() : 0;
}

}  // namespace flowsim
