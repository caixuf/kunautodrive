/**
 * slam_math.c — SLAM 纯数学件实现（详见 slam_math.h）
 */

#include "slam_math.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float slam_wrap_pi(float angle) {
    const float two_pi = 2.0f * (float)M_PI;
    float a = fmodf(angle, two_pi);          /* NaN → NaN，不进入循环 */
    if (a > (float)M_PI)  a -= two_pi;
    if (a < -(float)M_PI) a += two_pi;
    return a;
}

void slam_dry_run_pose(uint64_t poses_published, int publish_hz, float radius_m,
                       Pose2D* out) {
    if (!out) return;
    const double hz = (publish_hz > 0) ? (double)publish_hz : 20.0;
    const double t = (double)poses_published / hz;
    memset(out, 0, sizeof(*out));
    out->x         = radius_m * (float)cos(t);
    out->y         = radius_m * (float)sin(t);
    out->heading   = (float)t + (float)(M_PI / 2.0);
    out->cov_xx    = 0.1f;
    out->cov_yy    = 0.1f;
    out->cov_hh    = 0.05f;
    out->converged = true;
    out->source    = SLAM_POSE_SOURCE_SLAM;
}
