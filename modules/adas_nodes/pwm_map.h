#ifndef PWM_MAP_H
#define PWM_MAP_H

/**
 * pwm_map.h — ControlCmd → PWM 脉宽的纯映射（无 IO / 无全局量）
 *
 * 从 actuator_pwm_node.c 抽出来，理由与 imu_protocol.{h,c} 相同：节点内的纯逻辑
 * 无法被 CI 覆盖，只能靠测试副本 + 行号标注（会漂移）。抽成 .c 后
 * tests/test_adas_nodes_logic.c 直接编同一份实现 —— 真车改动 PWM 映射曲线时，
 * 单测立刻跟着变。
 */

#ifdef __cplusplus
extern "C" {
#endif

#define PWM_CENTER_US      1500   /**< 中位脉宽 μs */
#define PWM_MIN_US         1000   /**< 安全下限 μs */
#define PWM_MAX_US         2000   /**< 安全上限 μs */
/** 舵机满量程对应的前轮转角（rad）。steering 输入按此归一化到 [-1, 1]。 */
#define PWM_MAX_STEER_RAD  0.22

/**
 * ControlCmd 标量 → ESC / 舵机脉宽（μs）。
 *
 * 语义（与 actuator_pwm_node.c 原实现逐字一致）：
 *   - emergency_stop → ESC 回中位（PWM_CENTER_US）
 *   - 否则 brake > 0.01 优先于 throttle：ESC = center − brake·throttle_scale
 *   - 否则 ESC = center + throttle·throttle_scale
 *   - steering_rad 归一化到 ±PWM_MAX_STEER_RAD 后乘 steering_scale
 *   - 两路输出都钳位到 [PWM_MIN_US, PWM_MAX_US]
 *
 * @param throttle/brake   ∈ [-1, 1]
 * @param steering_rad     前轮转角（rad）
 * @param e_stop           非 0 = 紧急停
 * @param throttle_scale   throttle/brake 满量程对应脉宽偏移（μs）
 * @param steering_scale   舵机满量程对应脉宽偏移（μs）
 * @param esc_us/steer_us  输出（允许 NULL）
 */
void pwm_map_control_cmd(double throttle, double brake, double steering_rad,
                         int e_stop,
                         double throttle_scale, double steering_scale,
                         int* esc_us, int* steer_us);

#ifdef __cplusplus
}
#endif

#endif /* PWM_MAP_H */
