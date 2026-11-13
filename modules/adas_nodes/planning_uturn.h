#ifndef PLANNING_UTURN_H
#define PLANNING_UTURN_H

#include "adas_msgs_gen.h"

#include <cstring>

/* 段感知下采样：512 点细轨迹 → 消息容量 64 点。
 * 必须保留 v 符号翻转边界点（D/R 换挡点），否则倒车段被稀释后
 * control 的 gear 推导（最近点向前扫符号）会跳段错挡。 */
static inline int downsample_uturn(const TrajectoryPoint* src, int sn,
                                   TrajectoryPoint* dst, int cap) {
    if (sn <= cap) {
        std::memcpy(dst, src, sizeof(TrajectoryPoint) * (size_t)sn);
        return sn;
    }
    static bool keep[512];
    std::memset(keep, 0, sizeof(keep));
    keep[0] = true;
    keep[sn - 1] = true;
    int forced = 2;
    for (int i = 1; i < sn; i++) {
        bool a = src[i - 1].v < -0.05f;
        bool b = src[i].v < -0.05f;
        if (a != b) {
            if (!keep[i - 1]) { keep[i - 1] = true; forced++; }
            if (!keep[i])     { keep[i] = true;     forced++; }
        }
    }
    int remain = cap - forced;
    double stride = (double)sn / (double)(remain > 0 ? remain + 1 : 1);
    for (int k = 1; k <= remain; k++) {
        int idx = (int)(k * stride);
        if (idx < 1) idx = 1;
        if (idx > sn - 1) idx = sn - 1;
        while (idx < sn && keep[idx]) idx++;
        if (idx < sn) keep[idx] = true;
    }
    int n = 0;
    for (int i = 0; i < sn && n < cap; i++) {
        if (keep[i]) dst[n++] = src[i];
    }
    return n;
}

#endif  // PLANNING_UTURN_H

