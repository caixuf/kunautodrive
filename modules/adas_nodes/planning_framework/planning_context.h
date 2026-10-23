#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string_view>
#include <type_traits>
#include <variant>

#include "adas_msgs_gen.h"

namespace flowengine::planning {

/**
 * @brief Zero-dynamic-allocation type-safe Blackboard for Task-to-Task data sharing.
 *
 * Implements a fast, fixed-capacity key-value store suitable for automotive
 * real-time planning loops without heap memory allocation.
 */
class Blackboard {
public:
    static constexpr size_t kMaxEntries = 32;
    static constexpr size_t kMaxKeyLen = 32;
    static constexpr size_t kMaxStringValLen = 32;

    using ValueVariant = std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        void*,
        const void*,
        std::array<char, kMaxStringValLen>
    >;

    struct Entry {
        std::array<char, kMaxKeyLen> key{};
        ValueVariant value{};
        bool in_use{false};
    };

    Blackboard() = default;

    void Clear() noexcept {
        for (size_t i = 0; i < count_; ++i) {
            entries_[i].in_use = false;
            entries_[i].key.fill('\0');
            entries_[i].value = std::monostate{};
        }
        count_ = 0;
    }

    size_t Size() const noexcept { return count_; }
    static constexpr size_t Capacity() noexcept { return kMaxEntries; }

    bool Has(std::string_view key) const noexcept {
        return FindEntry(key) != nullptr;
    }

    // Set bool
    bool Set(std::string_view key, bool val) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        e->value = val;
        return true;
    }

    // Set signed and unsigned integrals (except bool)
    template<typename T>
    requires (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    bool Set(std::string_view key, T val) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        e->value = static_cast<int64_t>(val);
        return true;
    }

    // Set floating point
    template<typename T>
    requires std::is_floating_point_v<T>
    bool Set(std::string_view key, T val) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        e->value = static_cast<double>(val);
        return true;
    }

    // Set string_view
    bool Set(std::string_view key, std::string_view val) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        std::array<char, kMaxStringValLen> buf{};
        size_t len = std::min(val.size(), kMaxStringValLen - 1);
        std::memcpy(buf.data(), val.data(), len);
        buf[len] = '\0';
        e->value = buf;
        return true;
    }

    bool Set(std::string_view key, const char* val) noexcept {
        return Set(key, std::string_view(val ? val : ""));
    }

    // Set pointers
    bool Set(std::string_view key, void* ptr) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        e->value = ptr;
        return true;
    }

    bool Set(std::string_view key, const void* ptr) noexcept {
        Entry* e = GetOrCreateEntry(key);
        if (!e) return false;
        e->value = ptr;
        return true;
    }

    // Get bool
    bool Get(std::string_view key, bool& out_val) const noexcept {
        const Entry* e = FindEntry(key);
        if (!e || !std::holds_alternative<bool>(e->value)) return false;
        out_val = std::get<bool>(e->value);
        return true;
    }

    // Get integral
    template<typename T>
    requires (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    bool Get(std::string_view key, T& out_val) const noexcept {
        const Entry* e = FindEntry(key);
        if (!e) return false;
        if (std::holds_alternative<int64_t>(e->value)) {
            out_val = static_cast<T>(std::get<int64_t>(e->value));
            return true;
        }
        return false;
    }

    // Get floating point
    template<typename T>
    requires std::is_floating_point_v<T>
    bool Get(std::string_view key, T& out_val) const noexcept {
        const Entry* e = FindEntry(key);
        if (!e) return false;
        if (std::holds_alternative<double>(e->value)) {
            out_val = static_cast<T>(std::get<double>(e->value));
            return true;
        } else if (std::holds_alternative<int64_t>(e->value)) {
            out_val = static_cast<T>(std::get<int64_t>(e->value));
            return true;
        }
        return false;
    }

    // Get string_view
    bool Get(std::string_view key, std::string_view& out_val) const noexcept {
        const Entry* e = FindEntry(key);
        if (!e || !std::holds_alternative<std::array<char, kMaxStringValLen>>(e->value)) return false;
        const auto& buf = std::get<std::array<char, kMaxStringValLen>>(e->value);
        out_val = std::string_view(buf.data());
        return true;
    }

    // Get pointer
    template<typename T>
    requires std::is_pointer_v<T>
    bool Get(std::string_view key, T& out_val) const noexcept {
        const Entry* e = FindEntry(key);
        if (!e) return false;
        if constexpr (std::is_const_v<std::remove_pointer_t<T>>) {
            if (std::holds_alternative<const void*>(e->value)) {
                out_val = static_cast<T>(std::get<const void*>(e->value));
                return true;
            } else if (std::holds_alternative<void*>(e->value)) {
                out_val = static_cast<T>(std::get<void*>(e->value));
                return true;
            }
        } else {
            if (std::holds_alternative<void*>(e->value)) {
                out_val = static_cast<T>(std::get<void*>(e->value));
                return true;
            }
        }
        return false;
    }

    template<typename T>
    T GetOrDefault(std::string_view key, const T& default_val) const noexcept {
        T result = default_val;
        if (Get(key, result)) {
            return result;
        }
        return default_val;
    }

private:
    std::array<Entry, kMaxEntries> entries_{};
    size_t count_{0};

    const Entry* FindEntry(std::string_view key) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].in_use && std::string_view(entries_[i].key.data()) == key) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    Entry* FindEntry(std::string_view key) noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].in_use && std::string_view(entries_[i].key.data()) == key) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    Entry* GetOrCreateEntry(std::string_view key) noexcept {
        Entry* existing = FindEntry(key);
        if (existing) return existing;
        if (count_ >= kMaxEntries) return nullptr;
        Entry* slot = &entries_[count_++];
        slot->in_use = true;
        size_t len = std::min(key.size(), kMaxKeyLen - 1);
        std::memcpy(slot->key.data(), key.data(), len);
        slot->key[len] = '\0';
        return slot;
    }
};

/**
 * @brief Vehicle dynamic and kinematic state in global coordinates.
 */
struct VehicleState {
    double x{0.0};              // World X (m)
    double y{0.0};              // World Y (m)
    double z{0.0};              // World Z (m)
    double v{0.0};              // Longitudinal speed (m/s)
    double a{0.0};              // Longitudinal acceleration (m/s^2)
    double heading{0.0};        // Yaw / heading angle (rad)
    double yaw_rate{0.0};       // Yaw rate (rad/s)
    double steering{0.0};       // Steering angle (rad)
    double kappa{0.0};          // Curvature (1/m)
    double wheelbase{2.7};      // Wheelbase (m)
    int8_t gear{1};             // Gear: 1=DRIVE, 0=NEUTRAL, -1=REVERSE
};

/**
 * @brief Static road geometry and topological layout context.
 */
struct RoadGeometryContext {
    int lane_count{2};
    double lane_width{3.5};
    bool road_oneway{false};
    double curve_start_x{0.0};
    double curve_length_m{0.0};
    double curve_offset_m{0.0};
    double road_end_x{1e9};
    int current_lane_idx{0};
    int target_lane_idx{0};

    /**
     * @brief Center Y position for a given lane index (right-hand drive convention).
     */
    double LaneCenterY(int lane_idx) const noexcept {
        if (lane_count <= 1) return 0.0;
        return -(lane_idx - (lane_count - 1) * 0.5) * lane_width;
    }
};

/**
 * @brief L3 Autonomous Driving and Functional Safety status.
 */
struct L3SafetyState {
    int32_t degrade_level{0};        // 0=L0, 1=L1, 2=L2 (MRM), 3=L3 (Emergency Stop)
    int32_t degrade_reason{0};       // Reason code from degrade_ladder
    bool mrm_requested{false};       // Minimum Risk Maneuver requested by safety arbiter
    bool emergency_stop_requested{false}; // Critical emergency stop requested
    bool collision_risk{false};      // Forward collision danger detected
    double min_ttc_s{100.0};         // Minimum Time-To-Collision (s)
    double lead_gap_m{1e9};          // Bumper-to-bumper distance to lead obstacle (m)
    uint32_t lead_obs_id{0};         // Track ID of lead obstacle
};

/**
 * @brief Planning Frame Context per cycle (zero dynamic allocation, passed by reference).
 *
 * Encapsulates vehicle state, perception obstacles, road layout, L3 safety status,
 * and the inter-task shared Blackboard.
 */
struct FrameContext {
    uint32_t seq{0};
    uint64_t timestamp_us{0};

    VehicleState vehicle_state{};
    ObstacleList obstacles{};
    RoadGeometryContext road_geometry{};
    L3SafetyState l3_state{};
    Blackboard blackboard{};

    void Reset() noexcept {
        seq = 0;
        timestamp_us = 0;
        vehicle_state = VehicleState{};
        obstacles.count = 0;
        road_geometry = RoadGeometryContext{};
        l3_state = L3SafetyState{};
        blackboard.Clear();
    }

    size_t ObstacleCount() const noexcept {
        return obstacles.count;
    }

    const Obstacle& GetObstacle(size_t index) const noexcept {
        return obstacles.obstacles[index];
    }

    Obstacle& GetObstacle(size_t index) noexcept {
        return obstacles.obstacles[index];
    }

    bool AddObstacle(const Obstacle& obs) noexcept {
        if (obstacles.count >= 128) {
            return false;
        }
        obstacles.obstacles[obstacles.count++] = obs;
        return true;
    }

    void ClearObstacles() noexcept {
        obstacles.count = 0;
    }
};

} // namespace flowengine::planning
