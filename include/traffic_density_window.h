#ifndef TRAFFIC_DENSITY_WINDOW_H
#define TRAFFIC_DENSITY_WINDOW_H

/*
 * 时段 / 区域密度窗（L3 方向三 D3-3）—— 场景契约与 spawn 纯算法**共用一份**定义。
 *
 * 为什么放在 include/：`ScenarioTrafficDensity`（core 场景加载）与
 * `traffic_density_spawn.h`（模块纯算法）都要用它。此前两边各定义一份同义同序
 * 结构 + 一个逐字段复制的适配函数，字段一改就会静默漏拷（2026-11-07 code-review
 * 指出）。收敛到单一头文件后，适配层可以直接删掉。
 *
 * 语义：半开区间 [start, end) + 倍率；多窗首个命中者生效，窗外 = 基线 1.0。
 *   - 时段窗：t = 仿真时间（s）
 *   - 区域窗：t = route 累计弧长 s（m）
 */

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double start;
    double end;
    double multiplier;
} TrafficDensityWindow;

/** 窗口合法性**唯一判据**：场景解析器与运行时 scale 共用，避免
 * "加载时接受、运行时按另一套规则忽略"（负倍率曾一处丢弃、一处钳到 0）。 */
static inline int traffic_density_window_valid(const TrafficDensityWindow* w) {
    if (!w) return 0;
    if (!isfinite(w->start) || !isfinite(w->end) || !isfinite(w->multiplier))
        return 0;
    if (w->end < w->start) return 0;
    if (w->multiplier < 0.0) return 0;
    return 1;
}

static inline double traffic_density_window_scale(
    double t, const TrafficDensityWindow* windows, int n_windows
) {
    if (!windows || n_windows <= 0) return 1.0;
    for (int i = 0; i < n_windows; i++) {
        const TrafficDensityWindow* w = &windows[i];
        if (!traffic_density_window_valid(w)) continue;
        if (t >= w->start && t < w->end) return w->multiplier;
    }
    return 1.0;
}

/** 时段窗 × 区域窗的组合倍率。启动期 spawn 与运行期泊松必须共用这一处，
 * 否则两条路径的"窗外=1.0 / 首个命中"口径会再次分叉。 */
static inline double traffic_density_combined_scale(
    const TrafficDensityWindow* time_windows, int n_time, double t,
    const TrafficDensityWindow* zones, int n_zones, double s
) {
    return traffic_density_window_scale(t, time_windows, n_time)
         * traffic_density_window_scale(s, zones, n_zones);
}

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_DENSITY_WINDOW_H */