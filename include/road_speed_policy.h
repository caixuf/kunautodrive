#pragma once

#include <cmath>
#include <cstring>

namespace flowengine {

/** 与 tools/json_to_xodr.py posted_speed_mps 同一张表。改一边必须改另一边。 */
inline constexpr double kUrbanKph = 50.0;
inline constexpr double kViaductKph = 100.0;
inline constexpr double kRampKph = 60.0;
inline constexpr double kSkillAy = 2.0;       /* 车技能稳住的侧向加速度 m/s² */
inline constexpr double kSkillMaxMps = 36.0;  /* 车技/车辆绝对上限，约 130 km/h */
inline constexpr double kVehicleMaxMps = 36.0;
inline constexpr double kNoCameraOvertakeMult = 1.5; /* 无摄像头超车最多高于限速 50% */

inline double kph_to_mps(double kph) { return kph / 3.6; }

inline bool road_type_is(const char* type, const char* name) {
    return type && name && std::strcmp(type, name) == 0;
}

/** 道路类型的巡航限速（m/s）。authored 是场景里写的值。 */
inline double posted_speed_mps(const char* type, double authored) {
    /* Authored limits win. The park_to_park_city viaduct is authored at
     * 15.5 m/s but the old table forced every viaduct to 100 km/h (27.8 m/s);
     * ego then entered the 12m-offset exit ramp at 22 m/s, missed the turn and
     * wedged on the elevated deck. Scenarios are scaled worlds, not real
     * highways — honor the number the scenario author wrote. */
    if (road_type_is(type, "viaduct_highway") || road_type_is(type, "highway")) {
        if (authored > 0.5) return authored;
        return kph_to_mps(kViaductKph);
    }
    if (road_type_is(type, "ramp_curve")) {
        if (authored > 0.5) return authored;
        return kph_to_mps(kRampKph);
    }
    if (road_type_is(type, "urban")) {
        if (authored >= 11.0 && authored <= 16.7) return authored;
        return kph_to_mps(kUrbanKph);
    }
    if (authored > 0.5) return authored;
    return kph_to_mps(kUrbanKph);
}

inline double skill_speed_mps(double kappa_abs) {
    if (!(kappa_abs > 1e-4)) return kSkillMaxMps;
    const double v = std::sqrt(kSkillAy / kappa_abs);
    return v < kSkillMaxMps ? v : kSkillMaxMps;
}

/**
 * 本帧速度上限。
 * 不超车：该路限速，急弯再被车技按住。
 * 超车且有摄像头：最多 1.2 倍，仍不超车技。
 * 超车且没有摄像头：最多 1.5 倍。旧实现直接给车技上限（36 m/s），
 * 在城市 15 m/s 道路上会把 behavior 目标拉到 130 km/h；虽然 planning
 * 的曲率/限速还会再压，target_speed 本身已经失真。
 */
inline double command_cap_mps(double posted, bool overtaking, bool speed_camera,
                              double kappa_abs) {
    const double skill = skill_speed_mps(kappa_abs);
    double cap = posted > 0.5 ? posted : kph_to_mps(kUrbanKph);
    if (overtaking) {
        cap = speed_camera ? cap * 1.2 : cap * kNoCameraOvertakeMult;
    }
    if (cap > skill) cap = skill;
    if (cap > kVehicleMaxMps) cap = kVehicleMaxMps;
    return cap;
}

/** 前方 horizon_m 内折线的最大 |kappa|。点不够或是直道时返回 0。 */
inline double polyline_max_kappa(const double* xs, const double* ys, int n,
                                 double horizon_m) {
    constexpr double kPi = 3.14159265358979323846;
    if (!xs || !ys || n < 3 || horizon_m <= 0.0) return 0.0;
    double acc = 0.0;
    double max_k = 0.0;
    for (int i = 1; i + 1 < n; ++i) {
        const double ds0 = std::hypot(xs[i] - xs[i - 1], ys[i] - ys[i - 1]);
        const double ds1 = std::hypot(xs[i + 1] - xs[i], ys[i + 1] - ys[i]);
        acc += ds0;
        if (acc > horizon_m) break;
        double h0 = std::atan2(ys[i] - ys[i - 1], xs[i] - xs[i - 1]);
        double h1 = std::atan2(ys[i + 1] - ys[i], xs[i + 1] - xs[i]);
        double dh = h1 - h0;
        while (dh > kPi) dh -= 2.0 * kPi;
        while (dh < -kPi) dh += 2.0 * kPi;
        const double ds = ds0 + ds1;
        if (ds < 0.5) continue;
        const double k = std::fabs(dh) / ds;
        if (k > max_k) max_k = k;
    }
    return max_k;
}

}  // namespace flowengine
