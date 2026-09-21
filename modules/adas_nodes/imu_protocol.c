/**
 * imu_protocol.c — IMU ASCII 行解析 + 静止态合成（抽自 imu_driver_node.c）
 *
 * 见 imu_protocol.h 注释。本文件由 imu_driver_node.c 和 tests 共享。
 */

#include "imu_protocol.h"

#include <stdlib.h>   /* strtof */
#include <stddef.h>

/* ImuData 字段布局由 adas_msgs_gen.h 提供；这里复制结构布局避免把
 * gen header 拖进纯算法 .c（gen 依赖 build/ 生成，测试无法直接链接）。
 * 必须与 build/gen/ImuData.h 字段顺序一致。
 * 注：serialize 用 36 字节（无 padding），sizeof 含 4B trailing pad → 40B，
 * 字段偏移和访问模式完全相同。 */
typedef struct {
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
    uint64_t timestamp_us;
    float temperature;
} imu_protocol_layout_t;

_Static_assert(sizeof(imu_protocol_layout_t) == 40,
               "imu_protocol_layout 必须与 ImuData 大小一致 (40B with 8B-aligned uint64_t)");

int imu_protocol_parse_line(const char* line, void* out) {
    imu_protocol_layout_t* imu = (imu_protocol_layout_t*)out;
    if (!line || !imu) return -1;

    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;                 /* 跳过前导空白 */
    if (*p == '\0' || *p == '\r' || *p == '\n') return -1;

    /* 顺序解析 7 个浮点：ax, ay, az, gx, gy, gz, temp */
    float  v[7];
    int    cnt = 0;
    char*  end = NULL;
    for (cnt = 0; cnt < 7; cnt++) {
        v[cnt] = strtof(p, &end);
        if (end == p) return -1;                         /* 当前位置无数值 */
        p = end;
        /* 跳过分隔符（逗号/空白/回车）到下一字段 */
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    }

    /* 默认假设输入已是 m/s² 与 rad/s；若模块输出 g/°/s，在此处乘换算系数 */
    imu->accel_x = v[0];
    imu->accel_y = v[1];
    imu->accel_z = v[2];
    imu->gyro_x  = v[3];
    imu->gyro_y  = v[4];
    imu->gyro_z  = v[5];
    imu->temperature = v[6];
    return 0;
}

void imu_protocol_make_static(void* out, double gravity, int noise_idx) {
    imu_protocol_layout_t* imu = (imu_protocol_layout_t*)out;
    if (!imu) return;
    /* 固定 4-idx 噪声表（与原 imu_driver_node.c:170-182 幅度一致：
     *   na ±0.01 m/s², ng ±0.001 rad/s）；
     * 用 static const 表 + noise_idx 切换，imu_driver_node.c 传
     *   g.synthetic_idx++（可重现），测试传固定值（可断言）。 */
    static const float na_table[4] = { 0.005f, -0.003f, 0.008f, -0.006f };
    static const float ng_table[4] = { 0.0003f, -0.0002f, 0.0005f, -0.0001f };
    float na = na_table[noise_idx & 3];
    float ng = ng_table[noise_idx & 3];
    imu->accel_x = na;
    imu->accel_y = na * 0.5f;
    imu->accel_z = (float)gravity + na;
    imu->gyro_x  = ng;
    imu->gyro_y  = ng * 0.5f;
    imu->gyro_z  = ng;
    imu->temperature = 25.0f + na * 10.0f;
}