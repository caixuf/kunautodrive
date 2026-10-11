/*
 * 定位完整性交叉校验实现 — 见 include/localization_integrity.h。
 *
 * 纯函数、无全局状态、无时钟依赖（调用方传龄期），故可被单测完全覆盖，
 * 也可在 fusion_node 的每帧热路径上廉价调用。
 */

#include "localization_integrity.h"

#include <math.h>
#include <string.h>

static double loc_angle_diff(double a, double b) {
    /* 归一化到 (-pi, pi] */
    double d = fmod(a - b, 2.0 * M_PI);
    if (d > M_PI) d -= 2.0 * M_PI;
    if (d <= -M_PI) d += 2.0 * M_PI;
    return d;
}

bool localization_check_speed(double lidar_x, double lidar_y,
                              double prev_x, double prev_y, double dt,
                              double other_speed_mps, bool* ok,
                              double* out_lidar_speed) {
    if (!ok || dt <= 0.0) return false;
    /* 任一源缺失 → 无法判定（不是"通过"） */
    if (!isfinite(prev_x) || !isfinite(prev_y) || !isfinite(other_speed_mps))
        return false;

    const double other = fabs(other_speed_mps);
    const double lidar_speed = hypot(lidar_x - prev_x, lidar_y - prev_y) / dt;
    /* 低速时位置差分被噪声主导，比了没意义 */
    if (fmax(lidar_speed, other) < LOC_MIN_SPEED_FOR_CHECK_MPS) return false;

    const double diff = fabs(lidar_speed - other);
    const double denom = fmax(fmax(lidar_speed, other), 1e-6);
    const double rel = diff / denom;
    *ok = (diff <= LOC_SPEED_AGREE_TOL_MPS) || (rel <= LOC_SPEED_AGREE_REL);
    if (out_lidar_speed) *out_lidar_speed = lidar_speed;
    return true;
}

bool localization_check_heading(double lidar_x, double lidar_y,
                                double prev_x, double prev_y,
                                double other_heading_rad, bool* ok,
                                double* out_diff_rad) {
    if (!ok) return false;
    if (!isfinite(prev_x) || !isfinite(prev_y) || !isfinite(other_heading_rad))
        return false;

    const double dx = lidar_x - prev_x;
    const double dy = lidar_y - prev_y;
    /* 位移过小，方向不可信 */
    if (hypot(dx, dy) < 0.2) return false;

    const double lidar_heading = atan2(dy, dx);
    const double diff = fabs(loc_angle_diff(lidar_heading, other_heading_rad));
    *ok = (diff <= LOC_HEADING_AGREE_TOL_RAD);
    if (out_diff_rad) *out_diff_rad = diff;
    return true;
}

void localization_assess(LocalizationIntegrity* out) {
    if (!out) return;

    out->reason_count = 0;
    out->critical = false;
    out->reasons[0] = LOC_REASON_OK;

    const bool gps_ok   = (out->gps_age_ms   >= 0.0 &&
                           out->gps_age_ms   <= LOC_SOURCE_STALE_MS);
    const bool lidar_ok = (out->lidar_age_ms >= 0.0 &&
                           out->lidar_age_ms <= LOC_SOURCE_STALE_MS);

    /* 原因收集顺序 = 优先级：源全丢 > 源不一致 > 滤波器不健康 */
#define LOC_ADD_REASON(r)                                                    \
    do {                                                                     \
        if (out->reason_count < LOC_MAX_REASONS)                             \
            out->reasons[out->reason_count++] = (r);                         \
    } while (0)

    if (!gps_ok && !lidar_ok) {
        LOC_ADD_REASON(LOC_REASON_ALL_SOURCES_LOST);
        out->critical = true;
    } else {
        if (!gps_ok)   LOC_ADD_REASON(LOC_REASON_GPS_STALE);
        if (!lidar_ok) LOC_ADD_REASON(LOC_REASON_LIDAR_STALE);
    }

    /* 交叉校验只在两源都可用时才有意义（单源无从互校） */
    if (gps_ok && lidar_ok) {
        if (out->has_speed_check && !out->speed_ok) {
            LOC_ADD_REASON(LOC_REASON_SPEED_MISMATCH);
            out->critical = true;
        }
        if (out->has_heading_check && !out->heading_ok) {
            LOC_ADD_REASON(LOC_REASON_HEADING_MISMATCH);
            out->critical = true;
        }
    }

    if (out->has_gated_ratio && out->gated_ratio > LOC_GATED_RATIO_MAX) {
        LOC_ADD_REASON(LOC_REASON_FILTER_GATED);
        out->critical = true;
    }
    if (out->has_cov_trace && out->cov_trace > LOC_COV_TRACE_MAX) {
        LOC_ADD_REASON(LOC_REASON_COVARIANCE_GROWN);
        out->critical = true;
    }

#undef LOC_ADD_REASON

    /* trustworthy = 定位结果仍可用于驾驶（单源陈旧不算失败：冗余正在生效）*/
    out->trustworthy = !out->critical;

    if (out->reason_count == 0) {
        out->reasons[0] = LOC_REASON_OK;
        out->reason_count = 1;
    }
}

bool localization_should_escalate(double disagree_s) {
    return disagree_s >= LOC_DISAGREE_DWELL_S;
}
