#ifndef LOCALIZATION_INTEGRITY_H
#define LOCALIZATION_INTEGRITY_H

/*
 * 定位完整性交叉校验 — L3-P2 冗余（方向六）。
 *
 * 为什么存在：融合节点有 GPS + LiDAR/SLAM 双源，但两者之间**没有互校** ——
 * 只有一个 `diverged` 单标志（χ² 连续失败 >10 或协方差 trace>1e9 才置位），
 * 而 `EkfFusion.gated_count` 被算出来却从未发布（源码注释自己写了
 * "fusion_node 应把它发布出去"），`DEGRADE_REASON_LOCALIZATION` 更是定义了
 * 却全仓无人 set 的死原因码。结果"定位还准不准"在遥测里完全不可见，
 * P2 的门禁无从断言。
 *
 * 判定规则与 `tools/localization_integrity_sim.py` 逐条对应（单一事实源，
 * 改一处必须同步另一处；Python 侧先跑通再移植）。
 *
 * 返回值语义（关键）：`LocalizationIntegrity.trustworthy` = **定位结果仍可用于
 * 驾驶**，而不是"零告警"。单源陈旧时冗余正在生效、定位仍可信 → trustworthy
 * 保持 1，但 reasons 里仍如实列出该源已失效。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 阈值（与 localization_integrity_sim.py 的 P 字典一致）── */
#define LOC_SOURCE_STALE_MS          500.0
#define LOC_SPEED_AGREE_TOL_MPS        3.0
#define LOC_SPEED_AGREE_REL            0.35
#define LOC_HEADING_AGREE_TOL_RAD      0.4363323129985824  /* 25 deg */
#define LOC_MIN_SPEED_FOR_CHECK_MPS    3.0
#define LOC_GATED_RATIO_MAX            0.50
#define LOC_COV_TRACE_MAX            100.0
#define LOC_DISAGREE_DWELL_S           1.0

/* ── 原因码（稳定字符串，与 Python 侧 REASON_* 同名）── */
#define LOC_REASON_OK                "ok"
#define LOC_REASON_GPS_STALE         "gps_stale"
#define LOC_REASON_LIDAR_STALE       "lidar_stale"
#define LOC_REASON_ALL_SOURCES_LOST  "all_sources_lost"
#define LOC_REASON_SPEED_MISMATCH    "speed_mismatch"
#define LOC_REASON_HEADING_MISMATCH  "heading_mismatch"
#define LOC_REASON_FILTER_GATED      "filter_gated"
#define LOC_REASON_COVARIANCE_GROWN  "covariance_grown"

#define LOC_MAX_REASONS 8

typedef struct {
    /* 输入：各源龄期（ms），<0 = 该源从未到达 */
    double gps_age_ms;
    double lidar_age_ms;

    /* 输入：交叉校验的原始量（由调用方按需算好或留空） */
    bool   has_speed_check;
    bool   speed_ok;
    double lidar_speed_mps;
    double gps_speed_mps;

    bool   has_heading_check;
    bool   heading_ok;
    double heading_diff_rad;

    /* 输入：滤波器健康度 */
    bool   has_gated_ratio;
    double gated_ratio;      /* 被 χ² 拒绝的观测占比 [0,1] */
    bool   has_cov_trace;
    double cov_trace;

    /* 输出 */
    bool   trustworthy;
    int    reason_count;
    const char* reasons[LOC_MAX_REASONS];
    bool   critical;         /* 是否有 critical 原因（→ 应触发本地化降级）*/
} LocalizationIntegrity;

/*
 * 交叉校验 ①：位置差分速度 vs 另一源速度。
 *
 * @return true = 已判定（*ok 有效）；false = 无法判定（任一侧缺失或速度过低）。
 *         无法判定**不等于通过** —— 与仓库 require 语义一致。
 * 低速时位置差分被噪声主导（|Δpos| 量化误差 /dt 被放大），故低于
 * LOC_MIN_SPEED_FOR_CHECK_MPS 时不判。
 */
bool localization_check_speed(double lidar_x, double lidar_y,
                              double prev_x, double prev_y, double dt,
                              double other_speed_mps, bool* ok,
                              double* out_lidar_speed);

/*
 * 交叉校验 ②：位置差分方向 vs 另一源航向。
 * 位移过小（<0.2m）时方向不可信 → 返回 false（无法判定）。
 */
bool localization_check_heading(double lidar_x, double lidar_y,
                                double prev_x, double prev_y,
                                double other_heading_rad, bool* ok,
                                double* out_diff_rad);

/*
 * 综合判定。fills *out（含 reasons 指针数组，指向上面的字符串常量，
 * 生命周期为静态，调用方无需释放）。
 */
void localization_assess(LocalizationIntegrity* out);

/*
 * 去抖：连续不一致累计 >= LOC_DISAGREE_DWELL_S 才升级为降级。
 * @param disagree_s 已累计的不一致时长（秒）
 */
bool localization_should_escalate(double disagree_s);

#ifdef __cplusplus
}
#endif

#endif /* LOCALIZATION_INTEGRITY_H */
