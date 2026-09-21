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
/* Main                                                        */
/* ══════════════════════════════════════════════════════════ */

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

    printf("\n═══════════════════════════════════\n");
    printf("  Total: %d  ✅ Passed: %d  ❌ Failed: %d\n",
           g_passed + g_failed, g_passed, g_failed);
    printf("═══════════════════════════════════\n\n");

    return g_failed > 0 ? 1 : 0;
}
