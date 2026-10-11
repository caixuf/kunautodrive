/*
 * 定位完整性交叉校验单测（L3-P2 冗余）。
 *
 * 覆盖三件事，与 tools/localization_integrity_sim.py 的 --run-all 逐条对应：
 *   1. 单源陈旧 = 冗余生效（trustworthy）但原因码如实上报；
 *   2. 两源全丢 / 两源不一致 / 滤波器被拒 → critical（应触发本地化降级）；
 *   3. 无法判定 ≠ 通过（缺源 / 低速时交叉校验一律 abstain）。
 */

#include "localization_integrity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static int has_reason(const LocalizationIntegrity* li, const char* r) {
    for (int i = 0; i < li->reason_count; i++)
        if (strcmp(li->reasons[i], r) == 0) return 1;
    return 0;
}

static LocalizationIntegrity fresh(void) {
    LocalizationIntegrity li;
    memset(&li, 0, sizeof(li));
    li.gps_age_ms = 20.0;
    li.lidar_age_ms = 20.0;
    return li;
}

static void test_healthy(void) {
    LocalizationIntegrity li = fresh();
    li.has_speed_check = true;   li.speed_ok = true;
    li.has_heading_check = true; li.heading_ok = true;
    li.has_gated_ratio = true;   li.gated_ratio = 0.0;
    li.has_cov_trace = true;     li.cov_trace = 1.0;
    localization_assess(&li);
    CHECK(li.trustworthy, "healthy state must stay trustworthy");
    CHECK(!li.critical, "healthy state must not be critical");
    CHECK(has_reason(&li, LOC_REASON_OK), "healthy state reports ok");
}

static void test_single_source_loss_is_redundancy_not_failure(void) {
    /* GPS 丢、LiDAR 在：冗余正在生效 → 仍可信，但必须报 gps_stale */
    LocalizationIntegrity li = fresh();
    li.gps_age_ms = 5000.0;
    localization_assess(&li);
    CHECK(li.trustworthy, "GPS loss with LiDAR alive must stay trustworthy");
    CHECK(!li.critical, "single-source loss must not be critical");
    CHECK(has_reason(&li, LOC_REASON_GPS_STALE), "must report gps_stale");
    CHECK(!has_reason(&li, LOC_REASON_ALL_SOURCES_LOST),
          "GPS loss alone is NOT all_sources_lost");
    CHECK(!has_reason(&li, LOC_REASON_SPEED_MISMATCH),
          "single source must not be cross-checked");
}

static void test_all_sources_lost_is_critical(void) {
    LocalizationIntegrity li = fresh();
    li.gps_age_ms = 5000.0;
    li.lidar_age_ms = 5000.0;
    localization_assess(&li);
    CHECK(!li.trustworthy, "both sources lost must be untrustworthy");
    CHECK(li.critical, "both sources lost must be critical");
    CHECK(has_reason(&li, LOC_REASON_ALL_SOURCES_LOST), "must report all_sources_lost");
}

static void test_never_arrived_source_counts_as_lost(void) {
    /* age < 0 = 该源从未到达，必须与"陈旧"同样按失效处理 */
    LocalizationIntegrity li = fresh();
    li.gps_age_ms = -1.0;
    li.lidar_age_ms = -1.0;
    localization_assess(&li);
    CHECK(has_reason(&li, LOC_REASON_ALL_SOURCES_LOST),
          "never-arrived sources must count as lost");
}

static void test_speed_mismatch_is_critical(void) {
    /* LiDAR 说 15 m/s、GPS 说 3 m/s → 独立源互不相符 */
    bool ok = false;
    double ls = 0.0;
    const bool judged = localization_check_speed(15.0, 0.0, 0.0, 0.0, 1.0,
                                                3.0, &ok, &ls);
    CHECK(judged, "speed check must judge at driving speed");
    CHECK(!ok, "15 vs 3 m/s must be judged inconsistent");
    CHECK(fabs(ls - 15.0) < 1e-6, "derived lidar speed must be 15 m/s");

    LocalizationIntegrity li = fresh();
    li.has_speed_check = true; li.speed_ok = ok;
    localization_assess(&li);
    CHECK(!li.trustworthy, "speed mismatch must be untrustworthy");
    CHECK(has_reason(&li, LOC_REASON_SPEED_MISMATCH), "must report speed_mismatch");
}

static void test_speed_agreement_passes(void) {
    bool ok = false;
    const bool judged = localization_check_speed(10.0, 0.0, 0.0, 0.0, 1.0,
                                                 10.5, &ok, NULL);
    CHECK(judged, "speed check must judge when moving");
    CHECK(ok, "10 vs 10.5 m/s must agree");
}

static void test_heading_mismatch(void) {
    /* 沿 +x 位移，但另一源报 90° 航向 → 不一致 */
    bool ok = true;
    double diff = 0.0;
    const bool judged = localization_check_heading(10.0, 0.0, 0.0, 0.0,
                                                   M_PI / 2.0, &ok, &diff);
    CHECK(judged, "heading check must judge with sufficient displacement");
    CHECK(!ok, "displacement along +x vs heading 90deg must mismatch");
    CHECK(fabs(diff - M_PI / 2.0) < 1e-6, "heading diff must be 90 deg");
}

static void test_filter_gating_is_critical(void) {
    /* χ² 拒绝了 90% 的观测 → 滤波器实际没在跟踪 */
    LocalizationIntegrity li = fresh();
    li.has_gated_ratio = true;
    li.gated_ratio = 0.9;
    localization_assess(&li);
    CHECK(!li.trustworthy, "high gating ratio must be untrustworthy");
    CHECK(has_reason(&li, LOC_REASON_FILTER_GATED), "must report filter_gated");
}

static void test_covariance_growth_is_critical(void) {
    LocalizationIntegrity li = fresh();
    li.has_cov_trace = true;
    li.cov_trace = 1e6;
    localization_assess(&li);
    CHECK(!li.trustworthy, "grown covariance must be untrustworthy");
    CHECK(has_reason(&li, LOC_REASON_COVARIANCE_GROWN), "must report covariance_grown");
}

static void test_abstain_is_not_pass(void) {
    bool ok = true;
    /* 缺一源 → abstain */
    CHECK(!localization_check_speed(1.0, 0.0, NAN, NAN, 1.0, 1.0, &ok, NULL),
          "missing source must abstain, not pass");
    /* 低速 → abstain（位置差分被噪声主导）*/
    CHECK(!localization_check_speed(1.0, 0.0, 0.9, 0.0, 1.0, 0.5, &ok, NULL),
          "below min speed must abstain");
    /* 位移过小 → 航向 abstain */
    CHECK(!localization_check_heading(10.0, 0.0, 9.99, 0.0, 0.0, &ok, NULL),
          "tiny displacement must abstain on heading");
    /* dt<=0 防守 */
    CHECK(!localization_check_speed(1.0, 0.0, 0.0, 0.0, 0.0, 10.0, &ok, NULL),
          "non-positive dt must abstain");
}

static void test_escalation_debounce(void) {
    CHECK(!localization_should_escalate(0.05),
          "a single disagreeing frame must not escalate");
    CHECK(!localization_should_escalate(0.95),
          "just under the dwell window must not escalate");
    CHECK(localization_should_escalate(1.0),
          "sustained disagreement must escalate");
    CHECK(localization_should_escalate(3.0),
          "long disagreement must escalate");
}

int main(void) {
    test_healthy();
    test_single_source_loss_is_redundancy_not_failure();
    test_all_sources_lost_is_critical();
    test_never_arrived_source_counts_as_lost();
    test_speed_mismatch_is_critical();
    test_speed_agreement_passes();
    test_heading_mismatch();
    test_filter_gating_is_critical();
    test_covariance_growth_is_critical();
    test_abstain_is_not_pass();
    test_escalation_debounce();
    if (g_fail == 0) printf("localization_integrity: all checks passed\n");
    return g_fail;
}
