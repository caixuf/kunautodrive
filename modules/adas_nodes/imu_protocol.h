/**
 * imu_protocol.h — IMU ASCII 行解析 + 静止态合成（抽自 imu_driver_node.c）
 *
 * 历史（2026-09 handoff §4.2 副本制测试）：
 *   原 parse_imu_line / make_synthetic_imu 都是 imu_driver_node.c 的 static
 *   函数，测试文件 tests/test_adas_nodes_logic.c 只能复制副本，源代码改了
 *   副本不跟改 → CI 不报。本头/源文件让节点和测试共用同一份实现，副本删除。
 *
 * 设计：
 *   - parse_imu_line 字段顺序：ax, ay, az, gx, gy, gz, temp（7 个 float）
 *   - 默认假设输入 m/s² + rad/s；若模块输出 g/°/s 在解析处乘换算系数
 *   - make_synthetic_imu 把全局 g.gravity 改成参数（解耦全局状态）
 *   - rand() 噪声种子由调用方控制（imu_driver_node.c 用全局 srand，测试
 *     用固定 idx 表保证可重现）
 */
#ifndef FLOWENGINE_IMU_PROTOCOL_H
#define FLOWENGINE_IMU_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析一行 ASCII IMU 数据（"ax,ay,az,gx,gy,gz,temp" 7 个浮点，空格/Tab/逗号分隔）。
 *
 * @param line  串口读到的一行（不含 '\n' 也兼容）
 * @param out   解析结果写入 accel_x..temperature 7 个字段
 * @return      0 成功，-1 解析失败（NULL / 空行 / 字段不足 / 格式错误）
 */
int imu_protocol_parse_line(const char* line, void* out /* ImuData* */);

/**
 * 生成静止状态 IMU 样本（用于 dry-run）。
 *
 * @param out       ImuData 输出
 * @param gravity   z 轴重力加速度（m/s²）
 * @param noise_idx 噪声采样表索引（0..3 循环）。imu_driver_node.c 传
 *                  rand() 计数器；测试用固定 idx 保证可重现。
 */
void imu_protocol_make_static(void* out /* ImuData* */, double gravity, int noise_idx);

#ifdef __cplusplus
}
#endif

#endif /* FLOWENGINE_IMU_PROTOCOL_H */