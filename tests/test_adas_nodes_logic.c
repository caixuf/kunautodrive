/**
 * test_adas_nodes_logic.c — 真车部署 driver/节点 纯逻辑单测
 *
 * 覆盖 modules/adas_nodes/ 下的 driver 节点（imu / slam / actuator_pwm）
 * 的纯逻辑函数，dry-run 路径不依赖硬件、不依赖 transport/discovery/scheduler
 * 等运行时基础设施。这是真车部署 roadmap 的第一步：先把"两头"的占位实现
 * 的逻辑正确性钉死，再换真实算法/驱动时不会因为回归把已经被验证过的行为
 * 跑丢。
 *
 * ── 测试策略：副本 + 标注来源 ──
 * 节点 .c 文件的纯逻辑函数都是 `static`（不导出符号），且依赖全局 `g` 状态，
 * 无法直接 #include 进测试。本文件采取"副本 + 行号标注"方式：
 *   1. 把源文件里要测的纯逻辑函数原样复制到这里（保留可读性，不改逻辑）
 *   2. 在每个副本顶部用 `// adapted from <file>:<lines>` 标注来源
 *   3. 测试副本本身就能验证逻辑正确性
 *   4. 漂移风险：源码改了纯逻辑但没同步改副本时，CI 不报错——
 *      这是初始可接受的代价；后续若要把这套测试升级为"防漂移"，
 *      可把节点纯逻辑抽离到独立 .c/.h（imu_protocol / slam_dead_reckon / pwm_map）
 *      让节点和测试都链接同一份实现。
 *
 * 已抽离（测试与节点共用同一份实现，无副本漂移）：
 *   imu_protocol.{h,c}      — IMU 行解析 / 静态合成
 *   perception_points.{h,c} — 点云→DBSCAN 输入、聚类→ObstacleList
 *   lidar_scan.{h,c}        — LiDAR 3D 观测模型（射线/AABB、容量守卫、可达距离）
 *
 * 编译: cmake --build build --target test_adas_nodes_logic
 * 运行: ./build/bin/test_adas_nodes_logic
 */

#include "adas_msgs_gen.h"
#include "imu_protocol.h"
#include "dbscan_cluster.h"
#include "perception_points.h"
#include "pwm_map.h"
#include "sensor_model_weather.h"
#include "slam_math.h"
#include "safety_arbiter.h"
#include "traj_safety.h"
#include "lidar_scan.h"
#include "lidar_contract.h"

/* D2-04 / D2-05 M3: lane_match 10 字段契约 -> 抽出 helper 到 flowsim/lane_match_helpers.h。
 * compute_lane_match() 是 static 不能直接调；这些 extern "C" 包装是它的纯算法
 * 部分（曲率 / 合成 llt_id / 邻 lane 查找 / 失败归零），详见对应 .cpp。 */
#include "flowsim/lane_match_helpers.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)  printf("  %-50s ", name)
#define PASS()      do { printf("✅ PASS\n"); g_passed++; } while(0)
#define FAIL(...) do { printf("❌ FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failed++; } while(0)
#define ASSERT(cond, ...) if (!(cond)) { FAIL(__VA_ARGS__); return; }
#define ASSERT_EQ(a, b, ...) if ((a) != (b)) { \
        printf("❌ FAIL: "); \
        printf(__VA_ARGS__); \
        printf(" (got %d, expected %d)\n", (int)(a), (int)(b)); \
        g_failed++; \
        return; \
    }
#define ASSERT_NEAR(a, b, eps, ...) \
    if (fabs((double)(a) - (double)(b)) > (eps)) { \
        printf("❌ FAIL: "); \
        printf(__VA_ARGS__); \
        printf(" (got %.6f, expected %.6f, eps=%.6f)\n", (double)(a), (double)(b), (double)(eps)); \
        g_failed++; \
        return; \
    }

/* ══════════════════════════════════════════════════════════ */
/* IMU 纯逻辑（2026-09 handoff §4.2 反漂移：节点 + 测试共享同一份实现） */
/* ══════════════════════════════════════════════════════════ */

/* 真代码（imu_protocol.h/.c），测试通过该 wrapper 调用，源/真同步实现一致
 * ——任何修改仅需改 imu_protocol.c，CI 自动覆盖两侧。 */
static int parse_imu_line_for_test(const char* line, ImuData* out) {
    return imu_protocol_parse_line(line, out);
}

/* 固定 idx 计数器保证测试可重现（imu_protocol.c 内部 4-idx 表）。 */
static int test_synthetic_idx = 0;
static void make_synthetic_imu_for_test(ImuData* out, double gravity) {
    imu_protocol_make_static(out, gravity, test_synthetic_idx++ & 3);
}

/* ══════════════════════════════════════════════════════════ */
/* SLAM：改用抽出的 slam_math.{h,c}（与节点共用同一份实现）      */
/* ══════════════════════════════════════════════════════════ */
/* 旧副本有两个问题：heading 归一化那份抄的是 slam_node.c 里**已被 EKF 重构删掉**
 * 的死代码（真实实现现在在 ekf_slam.c，逐处内联了 4 遍，已收口到 slam_wrap_pi）；
 * 圆轨迹那份抄的是 publish 循环里的内联代码。现在两边共用 slam_math.c。 */

/* ══════════════════════════════════════════════════════════ */
/* Actuator PWM：改用抽出的 pwm_map.{h,c}（与节点共用同一份实现） */
/* ══════════════════════════════════════════════════════════ */
/* 以前这里是"副本 + 行号标注"（源改了副本不改，CI 不报）。现在直接调用
 * pwm_map_control_cmd()，常量 PWM_CENTER_US / PWM_MAX_STEER_RAD 也来自头文件。 */

/* ══════════════════════════════════════════════════════════ */
/* IMU Driver Tests                                           */
/* ══════════════════════════════════════════════════════════ */

static void test_imu_parse_basic(void) {
    TEST("imu parse 'ax,ay,az,gx,gy,gz,temp'");
    ImuData imu;
    int rc = parse_imu_line_for_test("0.1,0.2,9.8,0.01,0.02,0.03,25.5", &imu);
    ASSERT(rc == 0, "should succeed on valid line");
    ASSERT_NEAR(imu.accel_x, 0.1, 1e-6, "ax mismatch");
    ASSERT_NEAR(imu.accel_y, 0.2, 1e-6, "ay mismatch");
    ASSERT_NEAR(imu.accel_z, 9.8, 1e-6, "az mismatch");
    ASSERT_NEAR(imu.gyro_x, 0.01, 1e-6, "gx mismatch");
    ASSERT_NEAR(imu.gyro_y, 0.02, 1e-6, "gy mismatch");
    ASSERT_NEAR(imu.gyro_z, 0.03, 1e-6, "gz mismatch");
    ASSERT_NEAR(imu.temperature, 25.5, 1e-6, "temp mismatch");
    PASS();
}

static void test_imu_parse_negative_values(void) {
    TEST("imu parse negative floats (heavy braking / reverse rotation)");
    ImuData imu;
    int rc = parse_imu_line_for_test("-3.5,-1.2,-9.8,-0.5,-0.3,-2.1,-10.0", &imu);
    ASSERT(rc == 0, "should succeed");
    ASSERT_NEAR(imu.accel_x, -3.5, 1e-6, "ax should be negative");
    ASSERT_NEAR(imu.accel_z, -9.8, 1e-6, "az should be -9.8 (inverted)");
    ASSERT_NEAR(imu.gyro_z, -2.1, 1e-6, "gz should be -2.1 (CCW rotation)");
    ASSERT_NEAR(imu.temperature, -10.0, 1e-6, "temp should be -10");
    PASS();
}

static void test_imu_parse_whitespace_and_crlf(void) {
    TEST("imu parse tolerates spaces / CR / LF");
    ImuData imu;
    int rc = parse_imu_line_for_test("  1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0\r\n", &imu);
    ASSERT(rc == 0, "should tolerate leading space + CR/LF");
    ASSERT_NEAR(imu.accel_x, 1.0, 1e-6, "ax mismatch");
    ASSERT_NEAR(imu.temperature, 7.0, 1e-6, "temp mismatch");
    PASS();
}

static void test_imu_parse_too_few_fields(void) {
    TEST("imu parse rejects too few fields");
    ImuData imu;
    int rc = parse_imu_line_for_test("1.0,2.0,3.0", &imu);
    ASSERT(rc == -1, "should reject 3-field line (need 7)");
    PASS();
}

static void test_imu_parse_empty_line(void) {
    TEST("imu parse rejects empty / whitespace-only line");
    ImuData imu;
    ASSERT(parse_imu_line_for_test("", &imu) == -1, "empty line should fail");
    ASSERT(parse_imu_line_for_test("   \t  ", &imu) == -1, "whitespace-only should fail");
    ASSERT(parse_imu_line_for_test(NULL, &imu) == -1, "NULL line should fail");
    ASSERT(parse_imu_line_for_test("1,2,3,4,5,6,7", NULL) == -1, "NULL out should fail");
    PASS();
}

static void test_imu_parse_non_numeric(void) {
    TEST("imu parse rejects non-numeric field");
    ImuData imu;
    int rc = parse_imu_line_for_test("abc,2.0,3.0,4.0,5.0,6.0,7.0", &imu);
    ASSERT(rc == -1, "non-numeric first field should fail");
    PASS();
}

static void test_imu_synthetic_static_gravity(void) {
    TEST("imu synthetic: accel_z ≈ gravity at rest");
    ImuData imu;
    make_synthetic_imu_for_test(&imu, 9.80665);
    /* accel_z 必须接近重力（±0.02 噪声范围） */
    ASSERT(fabs(imu.accel_z - 9.80665) < 0.02, "accel_z should be ≈ gravity (got %.4f)", imu.accel_z);
    /* 水平轴加速度远小于重力 */
    ASSERT(fabs(imu.accel_x) < 0.02, "accel_x should be near 0 (got %.4f)", imu.accel_x);
    ASSERT(fabs(imu.accel_y) < 0.02, "accel_y should be near 0 (got %.4f)", imu.accel_y);
    /* 角速度应接近 0（±0.001 范围） */
    ASSERT(fabs(imu.gyro_x) < 0.002, "gyro_x should be near 0 (got %.5f)", imu.gyro_x);
    ASSERT(fabs(imu.gyro_z) < 0.002, "gyro_z should be near 0 (got %.5f)", imu.gyro_z);
    /* 温度应在 25°C 附近（±0.2） */
    ASSERT(fabs(imu.temperature - 25.0) < 0.3, "temp should be ≈ 25°C (got %.2f)", imu.temperature);
    PASS();
}

static void test_imu_synthetic_custom_gravity(void) {
    TEST("imu synthetic: respects custom gravity (moon = 1.62)");
    ImuData imu;
    make_synthetic_imu_for_test(&imu, 1.62);
    ASSERT(fabs(imu.accel_z - 1.62) < 0.02, "accel_z should track custom gravity (got %.4f)", imu.accel_z);
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* SLAM (dead reckoning dry-run circle) Tests                  */
/* ══════════════════════════════════════════════════════════ */

static void test_slam_circle_pose_t0(void) {
    TEST("slam dry-run circle at t=0: (R, 0), heading=π/2");
    Pose2D pose;
    slam_dry_run_pose(0, 20, 10.0f, &pose);
    ASSERT_NEAR(pose.x, 10.0, 1e-4, "x should be R at t=0");
    ASSERT_NEAR(pose.y, 0.0, 1e-4, "y should be 0 at t=0");
    ASSERT_NEAR(pose.heading, M_PI / 2.0, 1e-4, "heading should be π/2 at t=0");
    ASSERT(pose.converged, "converged should be true");
    ASSERT(pose.source == 2u, "source should be POSE_SOURCE_SLAM=2");
    PASS();
}

static void test_slam_circle_pose_quarter(void) {
    TEST("slam dry-run circle at t≈π/2s: near top of circle, on-circle");
    /* 20Hz 离散采样无法精确命中 t=π/2≈1.5708s（需 poses_published=31.4），
     * 取最近的整数样本 poses_published=31 → t=1.55s。
     * 此时 x=10*cos(1.55)≈0.208, y=10*sin(1.55)≈9.998 —— 量化误差是
     * 采样的固有性质，不是 bug。改用「在圆上」不变量 + 位置方向断言更稳健。 */
    Pose2D pose;
    slam_dry_run_pose(31, 20, 10.0f, &pose);
    /* 不变量：x² + y² = R²，对任意 t 恒成立（验证圆参数化正确） */
    float r2 = pose.x * pose.x + pose.y * pose.y;
    ASSERT_NEAR(r2, 100.0f, 0.1f, "should be on circle of radius 10 (|pos|²=%.4f)", r2);
    /* 方向断言：四分之一圈处 y 接近峰值 R，x 接近 0 */
    ASSERT(pose.y > 9.9f, "y should be near R=10 at quarter circle (got %.4f)", pose.y);
    ASSERT(fabs(pose.x) < 0.3f, "x should be near 0 at quarter circle (got %.4f)", pose.x);
    PASS();
}

static void test_slam_circle_pose_full_loop(void) {
    TEST("slam dry-run circle at t≈2πs: returns near start");
    /* 2π≈6.2832s，20Hz 下最近整数样本 poses_published=126 → t=6.3s。
     * sin(6.3)=0.0168 → y=0.168（量化误差，非 bug）。用 on-circle 不变量
     * +「回到起点附近」断言。 */
    Pose2D pose_start, pose_end;
    slam_dry_run_pose(0, 20, 10.0f, &pose_start);
    slam_dry_run_pose(126, 20, 10.0f, &pose_end);
    /* 不变量：绕一圈后仍在圆上 */
    float r2 = pose_end.x * pose_end.x + pose_end.y * pose_end.y;
    ASSERT_NEAR(r2, 100.0f, 0.1f, "should still be on circle after full loop (|pos|²=%.4f)", r2);
    /* 回到起点附近（x≈R, y≈0），容差容纳 20Hz 量化误差（≤0.25m） */
    ASSERT_NEAR(pose_end.x, 10.0f, 0.3f, "x should return near R after full loop");
    ASSERT(fabs(pose_end.y) < 0.3f, "y should return near 0 after full loop (got %.4f)", pose_end.y);
    PASS();
}

static void test_slam_circle_pose_convergence_flag(void) {
    TEST("slam dry-run: cov_xx/yy/hh are constant 0.1/0.1/0.05");
    Pose2D pose;
    for (int i = 0; i < 5; i++) {
        slam_dry_run_pose((uint64_t)i * 20, 20, 10.0f, &pose);
        ASSERT_NEAR(pose.cov_xx, 0.1, 1e-6, "cov_xx should be constant 0.1");
        ASSERT_NEAR(pose.cov_yy, 0.1, 1e-6, "cov_yy should be constant 0.1");
        ASSERT_NEAR(pose.cov_hh, 0.05, 1e-6, "cov_hh should be constant 0.05");
    }
    PASS();
}

static void test_slam_heading_normalize_basic(void) {
    TEST("slam heading normalize wraps to [-π, π]");
    /* 4π → 0 */
    ASSERT_NEAR(slam_wrap_pi(4.0f * (float)M_PI), 0.0f, 1e-5, "4π → 0");
    /* 3π/2 → -π/2 */
    ASSERT_NEAR(slam_wrap_pi(1.5f * (float)M_PI), -0.5f * (float)M_PI, 1e-5, "3π/2 → -π/2");
    /* -3π/2 → π/2 */
    ASSERT_NEAR(slam_wrap_pi(-1.5f * (float)M_PI), 0.5f * (float)M_PI, 1e-5, "-3π/2 → π/2");
    PASS();
}

static void test_slam_heading_normalize_in_range(void) {
    TEST("slam heading normalize: in-range values unchanged");
    float test_vals[] = { 0.0f, 0.5f, -0.5f, (float)M_PI, -(float)M_PI, 1.0f, -1.0f };
    for (size_t i = 0; i < sizeof(test_vals)/sizeof(test_vals[0]); i++) {
        float v = test_vals[i];
        /* M_PI 本身和 -M_PI 本身按定义是合法范围（≤π / ≥-π），不变 */
        ASSERT_NEAR(slam_wrap_pi(v), v, 1e-6, "in-range value should be unchanged");
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* Actuator PWM (ControlCmd → PWM mapping) Tests               */
/* ══════════════════════════════════════════════════════════ */

static void test_pwm_throttle_full_forward(void) {
    TEST("pwm: throttle=+1.0 → esc=2000μs (full forward)");
    int esc, steer;
    pwm_map_control_cmd(1.0, 0.0, 0.0, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(esc, 2000, "throttle +1 should map to 2000μs");
    ASSERT_EQ(steer, 1500, "steering 0 should map to 1500μs");
    PASS();
}

static void test_pwm_throttle_full_reverse(void) {
    TEST("pwm: throttle=-1.0 → esc=1000μs (full reverse / brake)");
    int esc, steer;
    pwm_map_control_cmd(-1.0, 0.0, 0.0, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(esc, 1000, "throttle -1 should map to 1000μs");
    PASS();
}

static void test_pwm_brake_full(void) {
    TEST("pwm: brake=1.0 → esc=1000μs (full brake)");
    int esc, steer;
    pwm_map_control_cmd(0.0, 1.0, 0.0, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(esc, 1000, "brake=1 should map to 1000μs (reverse of throttle)");
    PASS();
}

static void test_pwm_brake_overrides_throttle(void) {
    TEST("pwm: brake>0.01 overrides throttle (priority safety)");
    /* 源文件 actuator_pwm_node.c:280 用 `else if (brake > 0.01)` 走刹车路径，
     * 即 throttle 路径被忽略。这是安全设计：刹车时油门信号被丢弃。 */
    int esc, steer;
    pwm_map_control_cmd(1.0, 0.5, 0.0, 0, 500.0, 500.0, &esc, &steer);
    /* brake=0.5 → esc = 1500 - 0.5*500 = 1250, 不是 2000 */
    ASSERT_EQ(esc, 1250, "brake should override throttle (esc=1250, not 2000)");
    PASS();
}

static void test_pwm_e_stop_overrides_all(void) {
    TEST("pwm: emergency_stop forces esc=1500μs (neutral)");
    int esc, steer;
    /* e_stop 即使有 throttle / brake 也应该强制中位 */
    pwm_map_control_cmd(1.0, 1.0, 0.5, 1, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(esc, 1500, "e_stop should force esc=1500 (neutral)");
    PASS();
}

static void test_pwm_steering_max_left(void) {
    TEST("pwm: steering=+0.22rad → steer=2000μs (full right)");
    /* 注意：源文件 actuator_pwm_node.c:286-291 的实现是
     *   steer_norm = steering_rad / PWM_MAX_STEER_RAD
     *   steer_us = 1500 + steer_norm * steering_scale
     * steering_rad=+0.22 → steer_norm=+1.0 → steer_us=2000
     * 按舵机约定 2000μs 是"全右"。这里只验证映射不验证物理方向。 */
    int esc, steer;
    pwm_map_control_cmd(0.0, 0.0, 0.22, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(steer, 2000, "steering=+0.22rad should map to 2000μs");
    PASS();
}

static void test_pwm_steering_max_right(void) {
    TEST("pwm: steering=-0.22rad → steer=1000μs (full left)");
    int esc, steer;
    pwm_map_control_cmd(0.0, 0.0, -0.22, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(steer, 1000, "steering=-0.22rad should map to 1000μs");
    PASS();
}

static void test_pwm_steering_clamp(void) {
    TEST("pwm: steering beyond max is clamped to ±0.22rad equivalent");
    int esc, steer_over, steer_under;
    pwm_map_control_cmd(0.0, 0.0, 0.5, 0, 500.0, 500.0, &esc, &steer_over);
    pwm_map_control_cmd(0.0, 0.0, -0.5, 0, 500.0, 500.0, &esc, &steer_under);
    ASSERT_EQ(steer_over, 2000, "steering=+0.5rad should clamp to 2000μs");
    ASSERT_EQ(steer_under, 1000, "steering=-0.5rad should clamp to 1000μs");
    PASS();
}

static void test_pwm_zero_cmd_is_neutral(void) {
    TEST("pwm: zero throttle/brake/steer → both 1500μs (neutral)");
    int esc, steer;
    pwm_map_control_cmd(0.0, 0.0, 0.0, 0, 500.0, 500.0, &esc, &steer);
    ASSERT_EQ(esc, 1500, "zero cmd should produce esc=1500");
    ASSERT_EQ(steer, 1500, "zero cmd should produce steer=1500");
    PASS();
}

static void test_pwm_custom_scale(void) {
    TEST("pwm: custom throttle_scale=300 changes range to 1500±300");
    /* 真车场景：throttle_scale 可能不是默认的 500，验证参数确实生效 */
    int esc, steer;
    pwm_map_control_cmd(1.0, 0.0, 0.0, 0, 300.0, 300.0, &esc, &steer);
    ASSERT_EQ(esc, 1800, "throttle=1 with scale=300 should map to 1800μs");
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* Sensor Model: Weather Attenuation                          */
/* ══════════════════════════════════════════════════════════ */

/* 天气衰减改用抽出的 sensor_model_weather.{h,c}（与节点共用同一份实现）。
 * 以前这里是"副本 + 行号标注"，源改了副本不改时 CI 不报。 */

static void test_weather_attenuation_clear(void) {
    TEST("sensor_model: clear weather 200m vis -> 0.0 att");
    double att = sensor_model_weather_attenuation(200.0, "clear");
    ASSERT_NEAR(att, 0.0, 1e-6, "clear weather with 200m visibility should have 0 attenuation");
    PASS();
}

static void test_weather_attenuation_fog(void) {
    TEST("sensor_model: fog 50m vis -> 0.75 att");
    double att = sensor_model_weather_attenuation(50.0, "fog");
    ASSERT_NEAR(att, 0.75, 1e-6, "50m visibility in fog should have 0.75 attenuation");
    PASS();
}

static void test_weather_attenuation_rain_floor(void) {
    TEST("sensor_model: rain with high vis -> min 0.30 att");
    double att = sensor_model_weather_attenuation(200.0, "rain");
    ASSERT_NEAR(att, 0.30, 1e-6, "rain should enforce minimum 0.30 attenuation");
    PASS();
}

static void test_weather_attenuation_dense_fog(void) {
    TEST("sensor_model: dense fog clamp -> 0.90 att");
    double att = sensor_model_weather_attenuation(5.0, "dense_fog");
    ASSERT_NEAR(att, 0.90, 1e-6, "extreme low vis should clamp factor to 0.1, att to 0.90");
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* Safety Control: Dynamic Arbiter                             */
/* ══════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════ */
/* 安全仲裁：改用抽出的 safety_arbiter.{h,c}（与节点共用同一份实现） */
/* ══════════════════════════════════════════════════════════ */
/* 以前这里维护 TestCmd + 仲裁函数两个副本（源改了不报）。现在直接调
 * safety_arbiter_apply()，输入用接口里的 SafetyArbiterCmd。节点私有的 ControlCmd
 * （含 speed/target/error/mode）不进这个模块 —— 节点侧只做一次字段映射。 */

static SafetyArbiterCmd make_cmd(double thr, double brk, double steer, int gear) {
    SafetyArbiterCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.throttle = thr;
    cmd.brake = brk;
    cmd.steer = steer;
    cmd.gear = gear;
    return cmd;
}

static void test_arbiter_no_model(void) {
    TEST("arbiter: no model -> return rule command");
    SafetyArbiterCmd rule = make_cmd(0.4, 0.0, 0.05, 1);
    SafetyArbiterCmd model = make_cmd(0.8, 0.0, 0.15, 1);
    int intervened = 0;
    SafetyArbiterCmd out = safety_arbiter_apply(&rule, &model, 0, 0, &intervened);
    ASSERT_NEAR(out.steer, 0.05, 1e-6, "should keep rule steer");
    ASSERT_NEAR(out.throttle, 0.4, 1e-6, "should keep rule throttle");
    ASSERT_EQ(intervened, 0, "should not be intervened");
    PASS();
}

static void test_arbiter_degraded_fallback(void) {
    TEST("arbiter: degraded state -> fallback to rule command");
    SafetyArbiterCmd rule = make_cmd(0.3, 0.0, -0.02, 1);
    SafetyArbiterCmd model = make_cmd(0.7, 0.0, 0.08, 1);
    int intervened = 0;
    SafetyArbiterCmd out = safety_arbiter_apply(&rule, &model, 1, 1, &intervened);
    ASSERT_NEAR(out.steer, -0.02, 1e-6, "should fallback to rule steer");
    ASSERT_NEAR(out.throttle, 0.3, 1e-6, "should fallback to rule throttle");
    ASSERT_EQ(intervened, 0, "fallback mode not intervened");
    PASS();
}

static void test_arbiter_model_within_envelope(void) {
    TEST("arbiter: model in envelope (|d_steer| <= 0.12) -> accept");
    SafetyArbiterCmd rule = make_cmd(0.5, 0.0, 0.05, 1);
    SafetyArbiterCmd model = make_cmd(0.6, 0.0, 0.10, 1);
    int intervened = 0;
    SafetyArbiterCmd out = safety_arbiter_apply(&rule, &model, 1, 0, &intervened);
    ASSERT_NEAR(out.steer, 0.10, 1e-6, "should accept model steer");
    ASSERT_NEAR(out.throttle, 0.6, 1e-6, "should accept model throttle");
    ASSERT_EQ(intervened, 0, "should not intervene when within envelope");
    PASS();
}

static void test_arbiter_steer_reject(void) {
    TEST("arbiter: model steer deviates > 0.12 rad -> reject to rule");
    SafetyArbiterCmd rule = make_cmd(0.5, 0.0, 0.0, 1);
    SafetyArbiterCmd model = make_cmd(0.5, 0.0, 0.18, 1);
    int intervened = 0;
    SafetyArbiterCmd out = safety_arbiter_apply(&rule, &model, 1, 0, &intervened);
    ASSERT_NEAR(out.steer, 0.0, 1e-6, "excessive steer should be rejected to rule");
    ASSERT_EQ(intervened, 1, "should flag intervention");
    PASS();
}

static void test_arbiter_brake_priority(void) {
    TEST("arbiter: rule brake active -> enforce brake, throttle=0");
    SafetyArbiterCmd rule = make_cmd(0.0, 0.7, 0.02, 1);
    SafetyArbiterCmd model = make_cmd(0.5, 0.0, 0.03, 1);
    int intervened = 0;
    SafetyArbiterCmd out = safety_arbiter_apply(&rule, &model, 1, 0, &intervened);
    ASSERT_NEAR(out.brake, 0.7, 1e-6, "should enforce rule brake");
    ASSERT_NEAR(out.throttle, 0.0, 1e-6, "should clamp throttle to 0");
    ASSERT_EQ(intervened, 1, "should flag intervention");
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* 轨迹扫掠碰撞检查（traj_safety.{h,c}）—— W3 的证据         */
/* ══════════════════════════════════════════════════════════ */

static void test_swept_catches_thin_obstacle_between_samples(void) {
    TEST("traj_swept: 0.5m 薄墙落在两个采样点之间 → 点测漏、扫掠抓到");
    /* 轨迹沿 x 轴，采样点间隔 4m（远大于薄墙 0.5m） */
    float xs[3] = {0.0f, 4.0f, 8.0f};
    float ys[3] = {0.0f, 0.0f, 0.0f};
    /* 薄墙：中心 x=2（正好在 0 与 4 之间），ol=0.5（沿 x），ow=2.0（沿 y） */
    double ox[1] = {2.0}, oy[1] = {0.0}, ow[1] = {2.0}, ol[1] = {0.5};

    ASSERT_EQ(traj_point_test_hits(xs, ys, 3, ox, oy, ow, ol, 1), 0,
              "点测必须漏掉（这就是 W3 的定义）");
    TrajSweptResult r = traj_swept_check(xs, ys, 3, ox, oy, ow, ol, 1);
    ASSERT_EQ(r.hits, 1, "扫掠必须抓到");
    ASSERT_EQ(r.seg_idx, 0, "命中的是第 0 段 (0,0)->(4,0)");
    ASSERT_EQ(r.obs_idx, 0, "命中的是第 0 个障碍物");
    PASS();
}

static void test_swept_ignores_side_obstacle(void) {
    TEST("traj_swept: 横向错开的障碍物不算命中（无假阳性）");
    float xs[3] = {0.0f, 4.0f, 8.0f};
    float ys[3] = {0.0f, 0.0f, 0.0f};
    /* 障碍物在 y=3（半宽 1）→ 与 y=0 的轨迹最近距离 2m，不应命中 */
    double ox[1] = {2.0}, oy[1] = {3.0}, ow[1] = {2.0}, ol[1] = {0.5};
    ASSERT_EQ(traj_swept_check(xs, ys, 3, ox, oy, ow, ol, 1).hits, 0, "错开的不该命中");
    /* 挪到 y=1.5（半宽 1 → 下沿 0.5）仍然不碰 y=0 */
    oy[0] = 1.5;
    ASSERT_EQ(traj_swept_check(xs, ys, 3, ox, oy, ow, ol, 1).hits, 0, "擦边也不该命中");
    /* y=1.0（下沿 0.0）刚好压线 → 命中 */
    oy[0] = 1.0;
    ASSERT(traj_swept_check(xs, ys, 3, ox, oy, ow, ol, 1).hits >= 1, "压线应命中");
    PASS();
}

static void test_swept_degenerate_and_empty(void) {
    TEST("traj_swept: 退化输入（点数<2 / 退化盒 / NULL）安全返回 0");
    float xs[1] = {0.0f}, ys[1] = {0.0f};
    /* 两个障碍物都是退化盒（半宽/半长 = 0）→ 全部忽略 */
    double ox[2] = {0.0, 1.0}, oy[2] = {0.0, 0.0}, ow[2] = {0.0, 0.0}, ol[2] = {0.0, 2.0};
    ASSERT_EQ(traj_swept_check(xs, ys, 1, ox, oy, ow, ol, 2).hits, 0, "单点无段");
    float xs2[2] = {0.0f, 2.0f}, ys2[2] = {0.0f, 0.0f};
    ASSERT_EQ(traj_swept_check(xs2, ys2, 2, ox, oy, ow, ol, 2).hits, 0, "退化盒忽略");
    ASSERT_EQ(traj_swept_check(NULL, ys2, 2, ox, oy, ow, ol, 2).hits, 0, "NULL 安全");
    PASS();
}

static void test_swept_counts_all_segments(void) {
    TEST("traj_swept: 多段多障碍物按对计数");
    /* 3 点 2 段；第 0 段(0→10)穿 x=2/x=8，第 1 段(10→20)穿 x=12/x=18 → 4 */
    float xs[3] = {0.0f, 10.0f, 20.0f};
    float ys[3] = {0.0f, 0.0f, 0.0f};
    double ox[4] = {2.0, 8.0, 12.0, 18.0}, oy[4] = {0.0, 0.0, 0.0, 0.0};
    double ow[4] = {2.0, 2.0, 2.0, 2.0},  ol[4] = {1.0, 1.0, 1.0, 1.0};
    TrajSweptResult r = traj_swept_check(xs, ys, 3, ox, oy, ow, ol, 4);
    ASSERT_EQ(r.hits, 4, "两段各穿两个障碍物");
    ASSERT_EQ(r.seg_idx, 0, "首次命中在第 0 段");
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* Perception: 点云消费纯逻辑（perception_points.{h,c}）       */
/* ══════════════════════════════════════════════════════════ */
/* 与 imu_protocol 相同的反漂移做法：测试直接链接节点用的同一份实现，
 * 不再维护"副本 + 行号标注"。 */

static void cloud_fill(LidarPointCloud* c, uint32_t n,
                       float x, float y, float z) {
    memset(c, 0, sizeof(*c));
    c->frame_id     = 1;
    c->timestamp_us = 1000;
    c->count        = n;
    for (uint32_t i = 0; i < n && i < PERCEPTION_MAX_CLOUD_POINTS; i++) {
        c->points[i].x = x;
        c->points[i].y = y;
        c->points[i].z = z;
        c->points[i].intensity = 1.0f;
    }
}

static void test_perception_points_basic(void) {
    TEST("perception_points: 车体系点云原样拷贝，不做位姿变换");
    LidarPointCloud c;
    cloud_fill(&c, 3, 10.0f, -2.0f, 0.0f);
    Point3D pts[8];
    uint32_t n = perception_points_from_cloud(&c, pts, 8, 60.0, 1.0);
    ASSERT_EQ(n, 3, "should copy all in-range points");
    ASSERT_NEAR(pts[0].x, 10.0, 1e-6, "x must pass through unchanged");
    ASSERT_NEAR(pts[0].y, -2.0, 1e-6, "y must pass through unchanged");
    PASS();
}

static void test_perception_points_rejects_nonfinite(void) {
    TEST("perception_points: NaN/Inf 点被丢弃");
    LidarPointCloud c;
    cloud_fill(&c, 4, 5.0f, 0.0f, 0.0f);
    c.points[0].x = NAN;
    c.points[1].y = INFINITY;
    c.points[2].z = -INFINITY;
    Point3D pts[8];
    uint32_t n = perception_points_from_cloud(&c, pts, 8, 60.0, 1.0);
    ASSERT_EQ(n, 1, "only the finite point survives");
    ASSERT_NEAR(pts[0].x, 5.0, 1e-6, "survivor is points[3]");
    PASS();
}

static void test_perception_points_range_filter(void) {
    TEST("perception_points: 超量程与近场自反射被丢弃");
    LidarPointCloud c;
    cloud_fill(&c, 3, 5.0f, 0.0f, 0.0f);
    c.points[0].x = 100.0f;   /* > max_range 60 */
    c.points[1].x = 0.3f;     /* < min_range 1.0 */
    c.points[2].x = 5.0f;     /* 保留 */
    Point3D pts[8];
    uint32_t n = perception_points_from_cloud(&c, pts, 8, 60.0, 1.0);
    ASSERT_EQ(n, 1, "only the in-range point survives");
    ASSERT_NEAR(pts[0].x, 5.0, 1e-6, "survivor is the in-range one");
    PASS();
}

static void test_perception_points_capacity_contract(void) {
    TEST("perception_points: count 超契约容量 → 整帧拒绝（不静默截断）");
    LidarPointCloud c;
    cloud_fill(&c, 2, 5.0f, 0.0f, 0.0f);
    c.count = PERCEPTION_MAX_CLOUD_POINTS + 1u;
    Point3D pts[8];
    ASSERT_EQ(perception_points_from_cloud(&c, pts, 8, 60.0, 1.0), 0,
              "over-capacity cloud must be rejected wholesale");
    /* 空云（空场景）返回 0 而不是报错 —— 这是合法输入 */
    cloud_fill(&c, 0, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(perception_points_from_cloud(&c, pts, 8, 60.0, 1.0), 0,
              "empty cloud is valid input, yields 0 points");
    PASS();
}

static void test_perception_points_max_out(void) {
    TEST("perception_points: 受 max_out 限制");
    LidarPointCloud c;
    cloud_fill(&c, 5, 5.0f, 0.0f, 0.0f);
    Point3D pts[2];
    ASSERT_EQ(perception_points_from_cloud(&c, pts, 2, 60.0, 1.0), 2,
              "must not write past max_out");
    PASS();
}

static void test_perception_clusters_null_and_noise(void) {
    TEST("clusters_to_obstacles: NULL 入参安全 + 小簇（噪声）丢弃");
    ObstacleList out;
    PerceptionFramePose pose = {1, 0.0, 0.0, 4, 3.5};
    ASSERT_EQ(perception_clusters_to_obstacles(NULL, 0, &pose, &out), 0,
              "NULL clusters → 0");
    ClusterBounds cb;
    memset(&cb, 0, sizeof(cb));
    cb.point_count = 2;   /* < 3 视为噪声 */
    ASSERT_EQ(perception_clusters_to_obstacles(&cb, 1, &pose, &out), 0,
              "2-point cluster is noise");
    ASSERT_EQ(out.count, 0, "output count must be 0");
    PASS();
}

static void test_perception_clusters_type_and_geometry(void) {
    TEST("clusters_to_obstacles: 尺寸启发式 → 车/行人/骑行者类型");
    ObstacleList out;
    PerceptionFramePose pose = {7, 0.0, 0.0, 4, 3.5};
    ClusterBounds cb[3];
    memset(cb, 0, sizeof(cb));
    /* 车：车头面 → 车体系纵向薄、横向 2m */
    cb[0].cx = 20.0f; cb[0].cy = -1.75f; cb[0].width = 0.4f; cb[0].length = 2.0f;
    cb[0].point_count = 12; cb[0].confidence = 0.8f; cb[0].cls = CLS_VEHICLE;
    /* 行人：0.5×0.5 */
    cb[1].cx = 6.0f;  cb[1].cy = 8.5f;  cb[1].width = 0.5f; cb[1].length = 0.5f;
    cb[1].point_count = 6;  cb[1].confidence = 0.5f; cb[1].cls = CLS_PEDESTRIAN;
    /* 骑行者 */
    cb[2].cx = 9.0f;  cb[2].cy = -4.0f; cb[2].width = 1.0f; cb[2].length = 2.0f;
    cb[2].min_z = 0.5f; cb[2].max_z = 1.8f;
    cb[2].point_count = 8;  cb[2].confidence = 0.5f; cb[2].cls = CLS_CYCLIST;

    uint32_t n = perception_clusters_to_obstacles(cb, 3, &pose, &out);
    ASSERT_EQ(n, 3, "3 clusters → 3 obstacles");
    ASSERT_EQ(out.frame_id, 7, "frame_id propagated");
    ASSERT_EQ(out.obstacles[0].type, OBJ_TYPE_VEHICLE, "cluster 0 → vehicle");
    ASSERT_EQ(out.obstacles[1].type, OBJ_TYPE_PEDESTRIAN, "cluster 1 → pedestrian");
    ASSERT_EQ(out.obstacles[2].type, OBJ_TYPE_CYCLIST, "cluster 2 → cyclist");
    /* 车体坐标原样透传（簇中心即障碍物位置） */
    ASSERT_NEAR(out.obstacles[0].x, 20.0, 1e-6, "x passthrough");
    ASSERT_NEAR(out.obstacles[0].y, -1.75, 1e-6, "y passthrough");
    ASSERT_NEAR(out.obstacles[1].width, 0.5, 1e-6, "pedestrian width kept");
    PASS();
}

/* ── 碎片合并（一个目标不应报成多个障碍物；E2E 收益未证实，
 *    也不是 straight_road 那条 FAIL 的成因 —— 见 perception_points.c 头注）── */

static void test_cluster_fragments_merged_into_one(void) {
    TEST("clusters_to_obstacles: 同一目标的碎片（正面 + 近侧面）并成一个");
    ObstacleList out;
    PerceptionFramePose pose = {1, 0.0, 0.0, 4, 3.5};
    ClusterBounds cb[2];
    memset(cb, 0, sizeof(cb));
    /* 取自 8m/10° 的离线探针实测：正面主簇 2001 点 + 近侧面细长条（y 向仅 0.02m） */
    cb[0].point_count = 2001; cb[0].cls = CLS_VEHICLE;
    cb[0].cx = 5.61f; cb[0].cy = 1.30f; cb[0].width = 2.31f; cb[0].length = 2.08f;
    cb[1].point_count = 47;   cb[1].cls = CLS_VEHICLE;
    cb[1].cx = 8.61f; cb[1].cy = 0.39f; cb[1].width = 2.35f; cb[1].length = 0.02f;

    ASSERT_EQ(perception_clusters_to_obstacles(cb, 2, &pose, &out), 1,
              "碎片应并成 1 个障碍物");
    /* 并集包围盒 x∈[4.46,9.79] y∈[0.26,2.34] → 中心 (7.12, 1.30) */
    ASSERT_NEAR(out.obstacles[0].x, 7.12, 0.05, "并集中心 x");
    ASSERT_NEAR(out.obstacles[0].y, 1.30, 0.05, "并集中心 y");
    PASS();
}

static void test_cluster_fragments_do_not_cross_lanes(void) {
    TEST("clusters_to_obstacles: 相邻车道两辆车不并（间隙 1.5m > 1.0m 闸）");
    ObstacleList out;
    PerceptionFramePose pose = {1, 0.0, 0.0, 4, 3.5};
    ClusterBounds cb[2];
    memset(cb, 0, sizeof(cb));
    for (int i = 0; i < 2; i++) {
        cb[i].point_count = 300; cb[i].cls = CLS_VEHICLE;
        cb[i].cx = 30.0f; cb[i].width = 4.6f; cb[i].length = 2.0f;
    }
    cb[0].cy = -1.75f;   /* 车道中心距 3.5m、车宽 2.0 → 空隙 1.5m */
    cb[1].cy = -5.25f;
    ASSERT_EQ(perception_clusters_to_obstacles(cb, 2, &pose, &out), 2,
              "相邻车道的两辆车必须保持 2 个");
    PASS();
}

static void test_cluster_fragments_dimensional_guard(void) {
    TEST("clusters_to_obstacles: 并起来不像一辆车 → 拒绝（第二道闸）");
    ObstacleList out;
    PerceptionFramePose pose = {1, 0.0, 0.0, 4, 3.5};
    ClusterBounds cb[2];
    memset(cb, 0, sizeof(cb));
    for (int i = 0; i < 2; i++) {
        cb[i].point_count = 300; cb[i].cls = CLS_VEHICLE;
        cb[i].cx = 30.0f; cb[i].width = 4.6f; cb[i].length = 2.0f;
    }
    /* 间距 2.4m - 两个 y 向 2.0m 的半宽 → 间隙 0.4m ≤ 1.0m（过第一闸），
     * 但并集 y 向跨度 = 2.4 + 2.0 = 4.4m > 3.5m（不像一辆车）→ 必须拒绝 */
    cb[0].cy = -1.75f;
    cb[1].cy = -4.15f;
    ASSERT_EQ(perception_clusters_to_obstacles(cb, 2, &pose, &out), 2,
              "并集超出一辆车占地时必须保持 2 个");
    PASS();
}

static void test_perception_clusters_lane_id(void) {
    TEST("clusters_to_obstacles: lane_id 反算 + 越界夹取");
    ObstacleList out;
    /* ego 在 y=0、朝向 0；车道宽 3.5、4 条 → 最左 lane 0（y>0 侧） */
    PerceptionFramePose pose = {1, 0.0, 0.0, 4, 3.5};
    ClusterBounds cb[3];
    memset(cb, 0, sizeof(cb));
    for (int i = 0; i < 3; i++) {
        cb[i].point_count = 5;
        cb[i].cls = CLS_VEHICLE;
        cb[i].width = 0.4f; cb[i].length = 2.0f;
    }
    cb[0].cy = 0.0f;    /* 车道中间 → 世界 y=0 → lane 2（右二） */
    cb[1].cy = 5.25f;   /* 车体左 5.25 → 世界 y=+5.25 → lane 0 */
    cb[2].cy = -99.0f;  /* 远超出右侧 → 夹到 lane 3 */

    ASSERT_EQ(perception_clusters_to_obstacles(cb, 3, &pose, &out), 3, "3 obstacles");
    ASSERT_EQ(out.obstacles[0].lane_id, 2, "y=0 → middle-right lane");
    ASSERT_EQ(out.obstacles[1].lane_id, 0, "leftmost lane");
    ASSERT_EQ(out.obstacles[2].lane_id, 3, "clamped to last lane");
    PASS();
}

/* 这条是 perception_node 选 GROUND_REMOVE_NONE 的依据：真点云（z 恒 0）在
 * RANSAC 下会被整帧当"地面"清空 → 0 簇。回归保护：谁把 ground_mode 改回
 * RANSAC，这里立刻红。
 *
 * 注意这个坑是**点数相关**的、因而更隐蔽：ransac_fit_ground() 的候选平面
 * 预筛要求 quick_count >= 20，而 quick_count 只在 min(50, n) 个点上采样，
 * 所以只有 n >= 20 的点云才真的会拟合出 z=0 平面（命中 100% 内点 → 全部移除）；
 * n < 20 时 RANSAC 直接放弃、地面去除静默失效。同一辆车 8m 处（约 30 点）
 * 会消失、30m 处（约 8 点）反而可见 —— 正是"看不见近处、看得见远处"的
 * 反直觉漏检。故用 25 点覆盖 n>=20 这一档。 */
static void test_perception_ground_remove_none_rationale(void) {
    TEST("dbscan: z=0 点云 RANSAC 清空、GROUND_REMOVE_NONE 保留");
    enum { N_PTS = 25 };   /* 5×5，>=20 才会触发 ransac_fit_ground 的预筛 */
    Point3D pts[N_PTS], work[N_PTS];
    int k = 0;
    for (int gx = 0; gx < 5; gx++) {
        for (int gy = 0; gy < 5; gy++) {
            pts[k].x = (float)gx * 1.0f;
            pts[k].y = (float)gy * 1.0f;
            pts[k].z = 0.0f;            /* 真点云只有障碍物命中，z 恒 0 */
            pts[k].intensity = 1.0f;
            k++;
        }
    }
    srand(42u);

    DbscanCluster db;
    dbscan_init(&db, 2.0f, 4);
    dbscan_set_ransac(&db, 100, 0.2f, 0.3f);
    memcpy(work, pts, sizeof(pts));
    ASSERT_EQ(dbscan_run(&db, work, N_PTS), 0,
              "RANSAC ground removal wipes a z=0 cloud (this is why we use NONE)");

    dbscan_set_ground_mode(&db, GROUND_REMOVE_NONE);
    memcpy(work, pts, sizeof(pts));
    ASSERT(dbscan_run(&db, work, N_PTS) >= 1,
           "GROUND_REMOVE_NONE must keep the cluster");
    PASS();
}

/* ══════════════════════════════════════════════════════════ */
/* LiDAR 观测模型（lidar_scan.{h,c}）— 3D 扫描 / 容量守卫 / 可达性 */
/* ══════════════════════════════════════════════════════════ */

/* 几何测试用：关掉噪声/丢失，让结果只由几何决定（不 flaky）。 */
static LidarScanPlan make_scan_plan(int az_rays, int channels,
                                    double range_m, double hfov_deg) {
    LidarScanPlan p;
    memset(&p, 0, sizeof(p));
    p.azimuth_rays    = az_rays;
    p.channels        = channels;
    p.hfov_deg        = hfov_deg;
    p.vfov_min_deg    = -20.0;
    p.vfov_max_deg    = 2.0;
    p.range_max_m     = range_m;
    p.mount_height_m  = 1.8;
    p.noise_std_m     = 0.0;
    p.noise_rel       = 0.0;
    p.loss_rate       = 0.0;
    p.intensity_tau_m = 30.0;
    p.sweep_us        = 50000.0;
    return p;
}

static LidarScanTarget make_target(double x, double y, double l, double w, double h) {
    LidarScanTarget t;
    memset(&t, 0, sizeof(t));
    t.x = x; t.y = y; t.length = l; t.width = w; t.height = h;
    return t;
}

static void test_lidar_scan_azimuth_fov_bounds(void) {
    TEST("lidar_scan: FOV 内目标命中 / FOV 外不命中");
    LidarScanPlan plan = make_scan_plan(241, 1, 100.0, 120.0);
    LidarPointCloud cloud;
    const double r = 50.0;

    /* channels=1 → 射线恒在传感器水平面（安装高 1.8m）。目标取 3.0m 高
     * （高过安装面）以便只考验方位角几何，不受"低于安装面看不见"影响。 */
    const double b_in = 30.0 * M_PI / 180.0;   /* ±60° 之内 */
    LidarScanTarget t = make_target(r * cos(b_in), r * sin(b_in), 4.6, 2.0, 3.0);
    uint64_t rng = lidar_scan_rng_seed(1u);
    uint32_t n = lidar_scan_generate(&plan, &t, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL);
    ASSERT(n > 0, "target at +30deg should be hit, got 0 points");

    const double b_out = 70.0 * M_PI / 180.0;  /* ±60° 之外 */
    t = make_target(r * cos(b_out), r * sin(b_out), 4.6, 2.0, 3.0);
    rng = lidar_scan_rng_seed(1u);
    n = lidar_scan_generate(&plan, &t, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL);
    ASSERT_EQ(n, 0, "target at +70deg (outside FOV) should miss");
    PASS();
}

static void test_lidar_scan_range_limit(void) {
    TEST("lidar_scan: 量程外的目标不产生点（量程放开后命中）");
    LidarScanPlan plan = make_scan_plan(241, 1, 60.0, 120.0);
    LidarPointCloud cloud;
    /* 3.0m 高：高过安装面，确保考验的是量程而不是仰角 */
    LidarScanTarget t = make_target(90.0, 0.0, 4.6, 2.0, 3.0);  /* 正前方 90m */

    uint64_t rng = lidar_scan_rng_seed(7u);
    ASSERT_EQ(lidar_scan_generate(&plan, &t, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL), 0,
              "90m target with 60m range must yield no points");

    plan.range_max_m = 120.0;   /* 同目标、只放量程 */
    rng = lidar_scan_rng_seed(7u);
    ASSERT(lidar_scan_generate(&plan, &t, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL) > 0,
           "90m target with 120m range should be hit, got 0");
    PASS();
}

static void test_lidar_scan_capacity_guard(void) {
    TEST("lidar_scan: 命中超容量 → 写点数封顶且上报丢弃（旧实现越界）");
    LidarScanPlan plan = make_scan_plan(2048, 32, 200.0, 120.0);
    /* 罩住整个 FOV 的大盒子：65536 根射线全部命中 */
    LidarScanTarget t = make_target(50.0, 0.0, 100.0, 200.0, 200.0);
    LidarPointCloud cloud;
    uint64_t rng = lidar_scan_rng_seed(3u);
    uint32_t dropped = 0;
    const uint32_t written = lidar_scan_generate(&plan, &t, 1, 0, 0, 0, 1, 1,
                                                 &rng, &cloud, &dropped);
    const uint32_t cap = (uint32_t)(sizeof(cloud.points) / sizeof(cloud.points[0]));
    ASSERT_EQ(written, cap, "written points must stop exactly at capacity");
    ASSERT_EQ(cloud.count, cap, "cloud.count must not exceed capacity");
    ASSERT(dropped > 0, "capacity overflow must be reported as dropped, got 0");
    PASS();
}

static void test_lidar_scan_deterministic(void) {
    TEST("lidar_scan: 同种子+同输入 → 同点云（自持 RNG，不碰全局 rand）");
    LidarScanPlan plan = make_scan_plan(180, 4, 120.0, 120.0);
    plan.noise_std_m = 0.05;
    plan.noise_rel   = 0.002;
    plan.loss_rate   = 0.10;

    LidarScanTarget ts[2];
    ts[0] = make_target(30.0,  1.5, 4.6, 2.0, 1.5);
    ts[1] = make_target(12.0, -2.0, 0.5, 0.5, 1.7);

    LidarPointCloud a, b;
    uint64_t ra = lidar_scan_rng_seed(2026u);
    uint64_t rb = lidar_scan_rng_seed(2026u);
    const uint32_t na = lidar_scan_generate(&plan, ts, 2, 0, 0, 0, 1, 1, &ra, &a, NULL);
    const uint32_t nb = lidar_scan_generate(&plan, ts, 2, 0, 0, 0, 1, 1, &rb, &b, NULL);

    ASSERT_EQ(na, nb, "same seed must give same point count");
    ASSERT(na > 0, "expected some hits in the scene");
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        FAIL("same seed produced different clouds");
        return;
    }
    PASS();
}

static void test_lidar_scan_z_axis(void) {
    TEST("lidar_scan: 多层扫描有非零 z；单层退化为平面（z 恒 0）");
    LidarPointCloud cloud;

    LidarScanPlan multi = make_scan_plan(360, 8, 120.0, 120.0);
    LidarScanTarget near_car = make_target(20.0, 0.0, 4.6, 2.0, 1.5);
    uint64_t rng = lidar_scan_rng_seed(11u);
    ASSERT(lidar_scan_generate(&multi, &near_car, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL) > 0,
           "expected hits on 20m target");
    int nonzero_z = 0;
    for (uint32_t i = 0; i < cloud.count; i++) {
        if (fabs((double)cloud.points[i].z) > 1e-6) { nonzero_z = 1; break; }
    }
    ASSERT(nonzero_z, "channels>1 must produce 3D points with z != 0");

    /* 单层：射线在传感器水平面，用高过安装面的目标才扫得到 */
    LidarScanPlan single = make_scan_plan(360, 1, 120.0, 120.0);
    LidarScanTarget tall = make_target(20.0, 0.0, 4.6, 2.0, 3.0);
    rng = lidar_scan_rng_seed(11u);
    ASSERT(lidar_scan_generate(&single, &tall, 1, 0, 0, 0, 1, 1, &rng, &cloud, NULL) > 0,
           "single layer should still hit a target taller than the mount");
    for (uint32_t i = 0; i < cloud.count; i++) {
        if (fabs((double)cloud.points[i].z) > 1e-6) {
            FAIL("channels==1 must stay planar (z == 0)");
            return;
        }
    }
    PASS();
}

static void test_lidar_scan_detectable_range(void) {
    TEST("lidar_scan: 可达距离单调（量程/分辨率↑ 不减，车 >= 行人）");
    LidarScanPlan plan = make_scan_plan(720, 8, 120.0, 120.0);
    const double car = lidar_scan_detectable_range(&plan, 2.0, 4.6, 1.5, 2.0, 4);
    const double ped = lidar_scan_detectable_range(&plan, 0.5, 0.5, 1.7, 2.0, 4);
    ASSERT(car > 0.0, "car should be detectable within 120m");
    ASSERT(ped > 0.0, "pedestrian should be detectable within 120m");
    ASSERT(car >= ped, "bigger target must not be less detectable");

    LidarScanPlan short_range = plan;
    short_range.range_max_m = 60.0;
    ASSERT(car >= lidar_scan_detectable_range(&short_range, 2.0, 4.6, 1.5, 2.0, 4),
           "larger range_max must not reduce detectable range");

    LidarScanPlan low_res = plan;
    low_res.azimuth_rays = 240;   /* 旧实现的 0.5°/根 */
    ASSERT(car >= lidar_scan_detectable_range(&low_res, 2.0, 4.6, 1.5, 2.0, 4),
           "higher azimuth resolution must not reduce detectable range");
    PASS();
}

static void test_lidar_scan_plan_sanitize(void) {
    TEST("lidar_scan: plan 夹紧（量程<=200 / rays<=2048 / channels>=1）");
    LidarScanPlan bad;
    memset(&bad, 0, sizeof(bad));
    bad.azimuth_rays    = 99999;
    bad.channels        = 0;
    bad.hfov_deg        = 120.0;
    bad.range_max_m     = 5000.0;
    bad.mount_height_m  = 1.8;
    bad.intensity_tau_m = 30.0;
    bad.loss_rate       = 5.0;
    bad.noise_rel       = 5.0;

    ASSERT(lidar_scan_plan_sanitize(&bad) == 1, "out-of-range plan must report clamping");
    ASSERT_EQ(bad.azimuth_rays, 2048, "azimuth_rays clamp");
    ASSERT_EQ(bad.channels, 1, "channels clamp");
    ASSERT(bad.range_max_m <= LIDAR_SCAN_MAX_RANGE_M + 1e-9,
           "range must clamp to the transfer contract limit");
    ASSERT(bad.loss_rate >= 0.0 && bad.loss_rate <= 1.0, "loss_rate clamp");
    ASSERT(bad.noise_rel <= 0.1, "noise_rel clamp");
    ASSERT(bad.vfov_max_deg > bad.vfov_min_deg, "vfov span must stay non-degenerate");

    LidarScanPlan ok = make_scan_plan(720, 8, 120.0, 120.0);
    ASSERT(lidar_scan_plan_sanitize(&ok) == 0, "valid plan must be reported unchanged");
    PASS();
}

static void test_lidar_scan_height_and_range_contract(void) {
    TEST("lidar_scan: 高度启发式 + 量程上限与传输契约一致（防漂移）");
    ASSERT_NEAR(lidar_scan_height_from_width(0.5), 1.7, 1e-9, "pedestrian 0.5m -> 1.7m");
    ASSERT_NEAR(lidar_scan_height_from_width(2.0), 1.5, 1e-9, "car 2.0m -> 1.5m");
    ASSERT_NEAR(lidar_scan_height_from_width(0.0), 1.5, 1e-9, "degenerate -> fallback 1.5m");
    ASSERT(LIDAR_SCAN_MAX_RANGE_M == (double)LIDAR_POINT_CLOUD_MAX_RANGE_M,
           "lidar_scan range cap must equal lidar_contract's LIDAR_POINT_CLOUD_MAX_RANGE_M");
    PASS();
}


/* ══════════════════════════════════════════════════════════ */
/* Lane Match M3 helper（flowsim_node.cpp::compute_lane_match 抽出的纯算法）*/
/* ══════════════════════════════════════════════════════════ */
/* D2-04 M3 起 compute_lane_match 输出 10 字段（M2 5 + M3 5）。其中 4 个核心算法
 * 在 flowsim/lane_match_helpers.{h,cpp} 里抽出成 extern "C" 纯函数，可直接调：
 *   - flowsim_lm_curvature_3pt        三点曲率（Menger 公式，带符号）
 *   - flowsim_lm_synthesize_lanelet_id M2/M3 step1 占位 llt_id 合成公式
 *   - flowsim_lm_pick_adjacent_lane_id closest-on-side 邻 lane 挑选
 *   - flowsim_lm_zero_outputs_on_failure 失败路径 10 字段归零
 *
 * esmini 路网相关部分（world_to_frenet / lane_width / drivable_lane_ids）由
 * compute_lane_match 集成时调 `g.roads.*`，本测试只覆盖纯算法（不依赖 esmini）。
 * 路网集成覆盖在 tests/test_adas_nodes_logic 之外的 test_road_network 里（场景无关
 * invariant 测试）+ demo runtime smoke（scripts/demo.sh --no-browser 10s）。 */

/* ── 1. 三点曲率：直线 → ≈ 0 ── */
static void test_compute_lane_match_curvature_straight_zero(void) {
    TEST("compute_lane_match: 三点共线 -> curvature ≈ 0 (±0.01)");
    /* 水平直线：y 恒为 0，x 等距。三点严格共线 → Menger 分子 cross = 0 → κ = 0 */
    const double k1 = flowsim_lm_curvature_3pt(0.0, 0.0,  1.0, 0.0,  2.0, 0.0);
    ASSERT_NEAR(k1, 0.0, 0.01, "horizontal colinear: kappa should be 0");
    /* 反向点序也得 0（cross 变号但 |kappa| 仍是 0） */
    const double k2 = flowsim_lm_curvature_3pt(2.0, 0.0,  1.0, 0.0,  0.0, 0.0);
    ASSERT_NEAR(k2, 0.0, 0.01, "horizontal colinear reversed: kappa should be 0");
    /* 倾斜直线 */
    const double k3 = flowsim_lm_curvature_3pt(0.0, 0.0,  1.0, 1.0,  2.0, 2.0);
    ASSERT_NEAR(k3, 0.0, 0.01, "45-degree colinear: kappa should be 0");
    PASS();
}

/* ── 2. 三点曲率：左弯 → > 0 ── */
static void test_compute_lane_match_curvature_left_turn_positive(void) {
    TEST("compute_lane_match: 左弯三点（CCW 弧）-> curvature > 0");
    /* 单位圆 R=1：3 点取 0°, 60°, 120° 走 CCW（左转方向），间距近 1m 量级。
     * 这三个点的外接圆就是原单位圆 → κ = 1/R = 1.0。
     * cross 方向：CCW → z 分量 > 0 → signed κ > 0。 */
    const double c60 = 0.5;
    const double s60 = 0.866025403784;
    const double c120 = -0.5;
    const double s120 = 0.866025403784;
    const double kappa = flowsim_lm_curvature_3pt(
        1.0, 0.0,         /* (1, 0) = 0 deg */
        c60, s60,         /* (0.5, sqrt(3)/2) = 60 deg */
        c120, s120);      /* (-0.5, sqrt(3)/2) = 120 deg */
    ASSERT(kappa > 0.5,
           "left arc points should give positive curvature (got %.4f, want > 0.5)", kappa);
    ASSERT_NEAR(kappa, 1.0, 0.05, "unit-circle 3-point approx should be ~1.0");

    /* 反向点序（右弯，CW）→ κ < 0（验证符号语义） */
    const double kappa_cw = flowsim_lm_curvature_3pt(
        1.0, 0.0,
        c120, s120,
        c60, s60);
    ASSERT(kappa_cw < -0.5,
           "reversed (CW) arc should give negative curvature (got %.4f)", kappa_cw);
    PASS();
}

/* ── 3. lane_match 字段语义：合成 llt_id + 失败路径零化（覆盖「已知 width=3.5 → 输出 3.5」测试意图）── */
/* 「已知 width=3.5 → 输出 3.5」测试意图是验证 lane_width 字段的透传语义。
 * 在不依赖 esmini 的前提下，把这条拆成两部分：
 *   - 验证合成 llt_id 公式的正确性（这是 lane_match_id 字段的来源）
 *   - 验证失败路径零化函数对 lane_width 也置 0（关键 invariant）：
 *     一旦 compute_lane_match 进入失败分支，所有字段必须一致归零，
 *     不能保留 stale 的 3.5。 */
static void test_compute_lane_match_synthesize_and_zero_path(void) {
    TEST("compute_lane_match: synthesize_lanelet_id 公式 + 失败路径零化");
    /* 合成公式: road_id*1000 + (lane_id+500)
     * road=0, lane=-1 -> 0*1000 + 499 = 499
     * road=0, lane= 0 -> 0*1000 + 500 = 500  (ref line 不参与，但公式仍算)
     * road=0, lane= 1 -> 0*1000 + 501 = 501
     * road=5, lane=-3 -> 5*1000 + 497 = 5497
     * road=1000, lane=-100 -> 1000*1000 + 400 = 1000400 */
    ASSERT(flowsim_lm_synthesize_lanelet_id(0, -1) == 499ULL,
           "synthesize(0, -1) should be 499");
    ASSERT(flowsim_lm_synthesize_lanelet_id(0, 0) == 500ULL,
           "synthesize(0, 0) should be 500 (ref line but formula still applies)");
    ASSERT(flowsim_lm_synthesize_lanelet_id(5, -3) == 5497ULL,
           "synthesize(5, -3) should be 5497");
    ASSERT(flowsim_lm_synthesize_lanelet_id(1000, -100) == 1000400ULL,
           "synthesize(1000, -100) should be 1000400");

    /* 防御: road_id < 0 (不应该发生，esmini 返 >= 0) -> 0 */
    ASSERT(flowsim_lm_synthesize_lanelet_id(-1, -1) == 0ULL,
           "synthesize(-1, -1) should be 0 (defensive)");
    /* 防御: lane_id < -500 -> 合成负数 -> 0 (不合法 llt_id) */
    ASSERT(flowsim_lm_synthesize_lanelet_id(0, -501) == 0ULL,
           "synthesize(0, -501) should be 0 (lane_offset < 0)");

    /* 关键 invariant: 失败路径零化函数对全部 10 字段生效
     * （lane_width=3.5 这个用例的核心: 在 valid=0 时 lane_width 必须也是 0，
     * 不能保留 stale 的 3.5）。 */
    uint64_t llt_id = 999, left_id = 999, right_id = 999;
    double llt_s = 1.0, llt_off = 0.5, h_err = 0.1;
    double curv = 0.2, lw = 3.5;   /* 故意设非零值模拟"漏清零" */
    int valid = 1;
    uint32_t flags = 0xFF;
    flowsim_lm_zero_outputs_on_failure(
        &llt_id, &llt_s, &llt_off, &h_err, &valid,
        &curv, &lw, &left_id, &right_id, &flags);
    ASSERT_EQ(llt_id, 0ULL, "failure path: llt_id must be 0 (not 999)");
    ASSERT_EQ(llt_s, 0.0, "failure path: llt_s must be 0");
    ASSERT_EQ(llt_off, 0.0, "failure path: llt_offset must be 0");
    ASSERT_EQ(h_err, 0.0, "failure path: llt_heading_err_rad must be 0");
    ASSERT_EQ(valid, 0, "failure path: valid must be 0");
    ASSERT_EQ(curv, 0.0, "failure path: curvature must be 0");
    ASSERT_EQ(lw, 0.0, "failure path: lane_width must be 0 (not stale 3.5)");
    ASSERT_EQ(left_id, 0ULL, "failure path: left_lanelet_id must be 0");
    ASSERT_EQ(right_id, 0ULL, "failure path: right_lanelet_id must be 0");
    ASSERT_EQ(flags, 0U, "failure path: flags must be 0");
    PASS();
}

/* ── 4. 邻 lane 查找: 3 车道中中间车道 left/right 都有；边上车道一侧 0 ── */
static void test_compute_lane_match_left_right_neighbors(void) {
    TEST("compute_lane_match: 3 车道 -> 中间 lane 双侧有邻 / 边上 lane 一侧 0");
    /* 模拟 OpenDRIVE 双向 3 车道（右侧 rht_three）:
     *   lane_id: -1, -2, -3
     *   -1 是离参考线最近的（右侧最内车道）
     *   -3 是最外的车道
     * 「中间」车道 = -2（左右各一个邻） */
    const int ids_rht[] = {-1, -2, -3};

    /* 中间车道 -2: 左邻（id 更大）= -1，右邻（id 更小）= -3 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-2, ids_rht, 3, +1), -1,
               "middle (-2) left neighbor should be -1");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-2, ids_rht, 3, -1), -3,
               "middle (-2) right neighbor should be -3");

    /* 边车道 -1: 左邻 = 0（候选里没有 > -1 的 id，但 ref line id=0 不在候选），
     * 右邻 = -2 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_rht, 3, +1), 0,
               "edge (-1) left neighbor must be 0 (no drivable on that side)");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_rht, 3, -1), -2,
               "edge (-1) right neighbor should be -2");

    /* 边车道 -3: 左邻 = -2，右邻 = 0 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-3, ids_rht, 3, +1), -2,
               "edge (-3) left neighbor should be -2");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-3, ids_rht, 3, -1), 0,
               "edge (-3) right neighbor must be 0 (no drivable on that side)");

    /* 4 车道（双侧各有车道）: 正负 id 都存在
     *   lane_id: -2, -1（右行驶）, +1, +2（左行驶 — ref 是 0 不在 drivable 列表）
     * 中间车道 -1: 左邻（id 更大）= +1（跳过 ref 0）, 右邻（id 更小）= -2
     * 中间车道 +1: 左邻 = +2, 右邻 = -1（同样跳过 ref 0）
     * ⚠ ref lane id=0 虽存在但不可行驶（参考线），本 helper 跳过；
     * closest-on-side 自然会找到 +1 / -1 而不是 0。 */
    const int ids_bidir[] = {-2, -1, 1, 2};
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_bidir, 4, +1), +1,
               "middle (-1) left neighbor should be +1 (skipping non-drivable ref 0)");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_bidir, 4, -1), -2,
               "middle (-1) right neighbor should be -2");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(+1, ids_bidir, 4, +1), +2,
               "middle (+1) left neighbor should be +2");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(+1, ids_bidir, 4, -1), -1,
               "middle (+1) right neighbor should be -1 (skipping non-drivable ref 0)");

    /* 真正的边车道 +2（最外左车道）: 左邻 = 0, 右邻 = +1 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(+2, ids_bidir, 4, +1), 0,
               "edge (+2) left neighbor must be 0 (no drivable on outer side)");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(+2, ids_bidir, 4, -1), +1,
               "edge (+2) right neighbor should be +1");
    /* 边车道 -2（最外右车道）: 左邻 = -1, 右邻 = 0 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-2, ids_bidir, 4, +1), -1,
               "edge (-2) left neighbor should be -1");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-2, ids_bidir, 4, -1), 0,
               "edge (-2) right neighbor must be 0 (no drivable on outer side)");

    /* 退化: 空候选、side=0 -> 0；candidate_ids=nullptr -> 0 */
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_rht, 0, +1), 0,
               "empty candidate list returns 0");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, ids_rht, 3, 0), 0,
               "side=0 returns 0");
    ASSERT_EQ(flowsim_lm_pick_adjacent_lane_id(-1, NULL, 3, +1), 0,
               "NULL candidates returns 0");
    PASS();
}

/* ── 5. 失败路径语义: world_to_frenet 失败 -> 全 10 字段归零 + valid=0 ── */
/* compute_lane_match 是 static 不能直接调；本测试模拟「失败路径」控制流:
 * 通过 flowsim_lm_zero_outputs_on_failure 验证当 flowsim_node.cpp 走 failed
 * 分支（g.roads_loaded=false / world_to_frenet==false / frenet_to_world==false）
 * 时调用方拿到的 10 字段语义。已在用例 3 验证了零化函数本身的正确性，
 * 本用例再补一组对照: 模拟"非零初值 + valid=1 -> 调零化 -> 全 0 + valid=0"
 * 的状态机转移，确保 cJSON 序列化层看到一致状态。
 *
 * 真正的「失败路径 -> valid=0」集成覆盖由 demo runtime smoke 验证
 * （scripts/demo.sh --no-browser 10s，flowsim 启动期 g.roads_loaded=false 时
 * publish_lane_match 走失败路径）。 */
static void test_compute_lane_match_failure_path_zeros(void) {
    TEST("compute_lane_match: 失败路径 -> 全 10 字段 0 + valid=0");
    /* 模拟"成功解算后某帧突然失败"（lane 被画到 junction 里、road_id=-1 等）
     * 的状态机转移: 前一帧 valid=1 的输出被 reset 到全 0。 */
    uint64_t llt_id = 12345;
    double llt_s = 50.0, llt_off = -0.3, h_err = 0.05;
    int valid = 1;
    double curv = 0.01, lw = 3.5;
    uint64_t left_id = 12344, right_id = 12346;
    uint32_t flags = 0;
    /* 模拟失败回调（compute_lane_match 内部任一条件不满足时调零化函数） */
    flowsim_lm_zero_outputs_on_failure(
        &llt_id, &llt_s, &llt_off, &h_err, &valid,
        &curv, &lw, &left_id, &right_id, &flags);
    /* gate lane_match_schema_check 的关键不变量:
     *   1) valid=0
     *   2) llt_id==0（避免"valid=1 requires llt_id>0"语义矛盾）
     *   3) 所有 8 个数值字段都是 0（lane_match_id_mismatch 等下游诊断
     *      不会拿到 stale 数据） */
    ASSERT_EQ(valid, 0, "valid must flip to 0 on failure");
    ASSERT_EQ(llt_id, 0ULL, "llt_id must be 0 on failure (gate invariant)");
    ASSERT_EQ(llt_s, 0.0, "llt_s must be 0 on failure");
    ASSERT_EQ(llt_off, 0.0, "llt_offset must be 0 on failure");
    ASSERT_EQ(h_err, 0.0, "llt_heading_err_rad must be 0 on failure");
    ASSERT_EQ(curv, 0.0, "curvature must be 0 on failure");
    ASSERT_EQ(lw, 0.0, "lane_width must be 0 on failure");
    ASSERT_EQ(left_id, 0ULL, "left_lanelet_id must be 0 on failure");
    ASSERT_EQ(right_id, 0ULL, "right_lanelet_id must be 0 on failure");
    ASSERT_EQ(flags, 0U, "flags must be 0 on failure (ABI reserved)");
    /* 重复调零化: 仍是全 0（幂等性） */
    flowsim_lm_zero_outputs_on_failure(
        &llt_id, &llt_s, &llt_off, &h_err, &valid,
        &curv, &lw, &left_id, &right_id, &flags);
    ASSERT_EQ(valid, 0, "valid stays 0 on repeated zeroing (idempotence)");
    ASSERT_EQ(llt_id, 0ULL, "llt_id stays 0 on repeated zeroing");
    PASS();
}

int main(void) {
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  FlowEngine ADAS Nodes Logic Tests        ║\n");
    printf("║  (imu / slam / actuator_pwm dry-run)     ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    printf("═══ IMU Driver (parse_imu_line / synthetic) ═══\n");
    test_imu_parse_basic();
    test_imu_parse_negative_values();
    test_imu_parse_whitespace_and_crlf();
    test_imu_parse_too_few_fields();
    test_imu_parse_empty_line();
    test_imu_parse_non_numeric();
    test_imu_synthetic_static_gravity();
    test_imu_synthetic_custom_gravity();

    printf("\n═══ SLAM dry-run circle + heading normalize ═══\n");
    test_slam_circle_pose_t0();
    test_slam_circle_pose_quarter();
    test_slam_circle_pose_full_loop();
    test_slam_circle_pose_convergence_flag();
    test_slam_heading_normalize_basic();
    test_slam_heading_normalize_in_range();

    printf("\n═══ Actuator PWM (ControlCmd → PWM mapping) ═══\n");
    test_pwm_throttle_full_forward();
    test_pwm_throttle_full_reverse();
    test_pwm_brake_full();
    test_pwm_brake_overrides_throttle();
    test_pwm_e_stop_overrides_all();
    test_pwm_steering_max_left();
    test_pwm_steering_max_right();
    test_pwm_steering_clamp();
    test_pwm_zero_cmd_is_neutral();
    test_pwm_custom_scale();

    printf("\n═══ Sensor Model Weather Attenuation ═══\n");
    test_weather_attenuation_clear();
    test_weather_attenuation_fog();
    test_weather_attenuation_rain_floor();
    test_weather_attenuation_dense_fog();

    printf("\n═══ Safety Control Dynamic Arbiter ═══\n");
    test_arbiter_no_model();
    test_arbiter_degraded_fallback();
    test_arbiter_model_within_envelope();
    test_arbiter_steer_reject();
    test_arbiter_brake_priority();

    printf("\n═══ Trajectory Swept Collision Check (W3) ═══\n");
    test_swept_catches_thin_obstacle_between_samples();
    test_swept_ignores_side_obstacle();
    test_swept_degenerate_and_empty();
    test_swept_counts_all_segments();

    printf("\n═══ Perception Point Cloud Consumption ═══\n");
    test_perception_points_basic();
    test_perception_points_rejects_nonfinite();
    test_perception_points_range_filter();
    test_perception_points_capacity_contract();
    test_perception_points_max_out();
    test_perception_clusters_null_and_noise();
    test_perception_clusters_type_and_geometry();
    test_perception_clusters_lane_id();
    test_perception_ground_remove_none_rationale();
    test_cluster_fragments_merged_into_one();
    test_cluster_fragments_do_not_cross_lanes();
    test_cluster_fragments_dimensional_guard();

    printf("\n═══ Lane Match M3 helpers (D2-04 10-field contract) ═══\n");
    test_compute_lane_match_curvature_straight_zero();
    test_compute_lane_match_curvature_left_turn_positive();
    test_compute_lane_match_synthesize_and_zero_path();
    test_compute_lane_match_left_right_neighbors();
    test_compute_lane_match_failure_path_zeros();

    printf("\n═══ LiDAR Observation Model (3D scan / capacity guard) ═══\n");
    test_lidar_scan_azimuth_fov_bounds();
    test_lidar_scan_range_limit();
    test_lidar_scan_capacity_guard();
    test_lidar_scan_deterministic();
    test_lidar_scan_z_axis();
    test_lidar_scan_detectable_range();
    test_lidar_scan_plan_sanitize();
    test_lidar_scan_height_and_range_contract();

    printf("\n═══════════════════════════════════\n");
    printf("  Total: %d  ✅ Passed: %d  ❌ Failed: %d\n",
           g_passed + g_failed, g_passed, g_failed);
    printf("═══════════════════════════════════\n\n");

    return g_failed > 0 ? 1 : 0;
}
