/**
 * test_obstacle_hint_field.c — D2-07 phase 1: Obstacle.obs_lane_match_hint
 *                              序列化 round-trip + 字段契约测试
 *
 * 覆盖：
 *   1. test_obstacle_serialize_with_hint_true   — 构造 hint=true,  round-trip → true
 *   2. test_obstacle_serialize_with_hint_false  — 构造 hint=false, round-trip → false
 *   3. test_obstacle_schema_version_is_2        — 编译期/运行期断言 SCHEMA_VERSION == 2
 *   4. test_obstacle_wire_size_35               — wire bytes 总长 = 35
 *   5. test_obstacle_hint_byte_position         — wire 末尾字节 @34 = hint 位
 *   6. test_obstacle_field_desc_present         — fields[] 数组含 obs_lane_match_hint
 *
 * 说明：此测试是 D2-07 phase 1 的"接口铁律"测试。
 * 父 agent 通过这些用例确认 IDL 扩展正确传导到 codegen，
 * 然后 sub-task B (perception_fusion_node) 才能基于该字段写融合逻辑。
 */

#include "adas_msgs_gen.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(cond, name) do { \
    if (cond) { g_passed++; printf("  PASS: %s\n", name); } \
    else      { g_failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

/* ──────────────────────────────────────────────────────────────
 *  Round-trip 1: hint = true
 * ────────────────────────────────────────────────────────────── */
static void test_obstacle_serialize_with_hint_true(void) {
    Obstacle src = {0};
    src.id           = 0xDEADBEEFu;
    src.x            = 12.5f;
    src.y            = -0.3f;
    src.vx           = 15.0f;
    src.vy           = 0.1f;
    src.width        = 1.8f;
    src.length       = 4.5f;
    src.lane_id      = 0;
    src.type         = OBJ_TYPE_VEHICLE;
    src.confidence   = 0.95f;
    src.obs_lane_match_hint = true;  /* 关键: fusion 标记为同/邻车道 */

    uint8_t buf[64] = {0};
    size_t out_size = 0;
    int rc = Obstacle_serialize(&src, buf, &out_size);
    TEST(rc == 0, "serialize hint=true returns 0");
    TEST(out_size == 35, "serialize hint=true reports size=35");

    Obstacle dst = {0};
    rc = Obstacle_deserialize(&dst, buf, out_size);
    TEST(rc == 0, "deserialize hint=true returns 0");
    TEST(dst.id == src.id, "round-trip id preserved");
    TEST(dst.x == src.x, "round-trip x preserved");
    TEST(dst.y == src.y, "round-trip y preserved");
    TEST(dst.vx == src.vx, "round-trip vx preserved");
    TEST(dst.vy == src.vy, "round-trip vy preserved");
    TEST(dst.width == src.width, "round-trip width preserved");
    TEST(dst.length == src.length, "round-trip length preserved");
    TEST(dst.lane_id == src.lane_id, "round-trip lane_id preserved");
    TEST(dst.type == src.type, "round-trip type preserved");
    TEST(dst.confidence == src.confidence, "round-trip confidence preserved");
    TEST(dst.obs_lane_match_hint == true,
         "round-trip obs_lane_match_hint == true (fused to true → still true)");
}

/* ──────────────────────────────────────────────────────────────
 *  Round-trip 2: hint = false
 * 语义 (与 spec FR-RT-05 一致): false = fusion 暂无法判断,
 * 绝不是"obs 在其它车道"的意思。
 * ────────────────────────────────────────────────────────────── */
static void test_obstacle_serialize_with_hint_false(void) {
    Obstacle src = {0};
    src.id           = 0x12345678u;
    src.x            = -50.0f;
    src.y            = 4.2f;
    src.vx           = -8.0f;
    src.vy           = 0.05f;
    src.width        = 0.6f;
    src.length       = 1.7f;
    src.lane_id      = -1;  /* 未分配 */
    src.type         = OBJ_TYPE_PEDESTRIAN;
    src.confidence   = 0.40f;
    src.obs_lane_match_hint = false;  /* 关键: fusion 暂无法判断 */

    uint8_t buf[64] = {0};
    size_t out_size = 0;
    int rc = Obstacle_serialize(&src, buf, &out_size);
    TEST(rc == 0, "serialize hint=false returns 0");
    TEST(out_size == 35, "serialize hint=false reports size=35");

    Obstacle dst = {0};
    rc = Obstacle_deserialize(&dst, buf, out_size);
    TEST(rc == 0, "deserialize hint=false returns 0");
    TEST(dst.confidence == src.confidence, "confidence preserved (false)");
    TEST(dst.lane_id == -1, "lane_id=-1 preserved (false case)");
    TEST(dst.obs_lane_match_hint == false,
         "round-trip obs_lane_match_hint == false (default / undecided preserved)");
    /* 反向断言: 必须 *不* 因为 hint=false 而丢失 lane_id 等其他字段 */
    TEST(dst.x == src.x, "x preserved alongside false hint");
    TEST(dst.type == src.type, "type preserved alongside false hint");
}

/* ──────────────────────────────────────────────────────────────
 *  Schema 版本号: phase 1 必须 bump 到 2。
 * 编译期 _Static_assert + 运行期 TEST 双保险(供老编译器兜底)。
 * ────────────────────────────────────────────────────────────── */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(OBSTACLE_SCHEMA_VERSION == 2,
               "OBSTACLE_SCHEMA_VERSION must be 2 after D2-07 phase 1 (+obs_lane_match_hint)");
#endif

static void test_obstacle_schema_version_is_2(void) {
    /* 运行期兜底:即使老编译器不支持 _Static_assert 也能 fail 出来 */
    TEST(OBSTACLE_SCHEMA_VERSION == 2,
         "OBSTACLE_SCHEMA_VERSION == 2 (compile-time + run-time check)");
}

/* ──────────────────────────────────────────────────────────────
 *  Wire 字节位置: hint 落在 buf[34] (最后一字节),前 34B 不变。
 * 旧消费者用 size < 34 校验的代码仍能识别前 34B。
 * ────────────────────────────────────────────────────────────── */
static void test_obstacle_hint_byte_position(void) {
    Obstacle src = {0};
    src.id = 1; src.x = 1.0f; src.y = 2.0f; src.vx = 0; src.vy = 0;
    src.width = 0; src.length = 0; src.lane_id = 0; src.type = 0;
    src.confidence = 0; src.obs_lane_match_hint = true;

    uint8_t buf[64] = {0};
    size_t out_size = 0;
    Obstacle_serialize(&src, buf, &out_size);
    TEST(out_size == 35, "wire size stays 35");
    TEST(buf[34] == 1, "wire[34] == 1 when hint=true");

    src.obs_lane_match_hint = false;
    memset(buf, 0, sizeof(buf));
    Obstacle_serialize(&src, buf, &out_size);
    TEST(buf[34] == 0, "wire[34] == 0 when hint=false");
    /* 前 34B 在序列化时不应被 bool 字段污染(serialize 顺序固定) */
    TEST(buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x00,
         "wire[0..3] == id=1 little-endian (前 34B 未受 hint 影响)");
}

/* ──────────────────────────────────────────────────────────────
 *  Field descriptor: schema 表里必须能查到新字段(给运行时 introspection 用)。
 * ────────────────────────────────────────────────────────────── */
static void test_obstacle_field_desc_present(void) {
    int found_hint = 0;
    int found_type = 0;  /* sanity: 既有的也还在 */
    size_t n = sizeof(Obstacle_fields) / sizeof(Obstacle_fields[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(Obstacle_fields[i].name, "obs_lane_match_hint") == 0) {
            found_hint = 1;
            /* 字段类型/offset/size 校验 */
            TEST(Obstacle_fields[i].elem_size == 1, "FIELD_KIND_BOOL: elem_size == 1");
            TEST(Obstacle_fields[i].array_len == 1, "hint is scalar (array_len == 1)");
            /* offset = offsetof(Obstacle, obs_lane_match_hint) — 由 codegen 自动注入 */
        } else if (strcmp(Obstacle_fields[i].name, "type") == 0) {
            found_type = 1;
        }
    }
    TEST(found_hint == 1,
         "Obstacle_fields[] 包含 obs_lane_match_hint(运行时 schema 自描述可见)");
    TEST(found_type == 1,
         "Obstacle_fields[] 仍包含 type(确保 codegen 没破坏既有字段表)");
    TEST(OBSTACLE_FIELD_COUNT == 11,
         "OBSTACLE_FIELD_COUNT == 11(D2-07 phase 1 后应为 11 个字段,原有 10 + hint)");
}

/* ──────────────────────────────────────────────────────────────
 *  Wire size 契约: codegen 生成的 deserialize 严格 == 35B(即 wire_size),
 * 旧版 34B 包会被拒绝(返回 -1)。
 *
 *  设计取舍(spec FR-RT-05):
 *    - D2-07 phase 1 不强制兼容 34B 老 wire;旧 producer 必须 re-flash
 *      到新 codegen 后才能与新 consumer 互操作。
 *    - 新字段 @34 是 "additive extension": 发包侧必须保证 35B 都填;
 *      收包侧默认拒绝不完整包,避免部分字段零初始化的隐患。
 *  这与 CLAUDE.md 的"显式契约"铁律一致:不暗中做静默 fallback。
 * ────────────────────────────────────────────────────────────── */
static void test_obstacle_wire_size_contract(void) {
    uint8_t full[35] = {0};
    uint8_t short34[34] = {0};
    Obstacle dst = {0};

    int rc_full = Obstacle_deserialize(&dst, full, sizeof(full));
    TEST(rc_full == 0, "deserialize 35B 完整包成功");

    /* 34B 老格式被严格拒绝(SPEC 要求 producer 必须升级) */
    int rc_short = Obstacle_deserialize(&dst, short34, sizeof(short34));
    TEST(rc_short == -1,
         "deserialize 34B 老包被拒绝(rc == -1)— producer 升级是 hard requirement");

    /* 36B / 63B(任意更大)应当正常接受,只读前 35B */
    uint8_t padded[64] = {0};
    int rc_padded = Obstacle_deserialize(&dst, padded, sizeof(padded));
    TEST(rc_padded == 0, "deserialize 超过 35B 的 padded 包成功(只读前 35B)");

    /* NULL/0 dst 或 buf 仍需拒绝 */
    int rc_null = Obstacle_deserialize(NULL, full, sizeof(full));
    TEST(rc_null == -1, "deserialize NULL dst 拒绝");
}

int main(void) {
    printf("Running D2-07 phase 1 Obstacle.obs_lane_match_hint tests...\n");
    test_obstacle_serialize_with_hint_true();
    test_obstacle_serialize_with_hint_false();
    test_obstacle_schema_version_is_2();
    test_obstacle_hint_byte_position();
    test_obstacle_field_desc_present();
    test_obstacle_wire_size_contract();
    printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
