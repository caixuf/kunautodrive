#ifndef SAFETY_ARBITER_H
#define SAFETY_ARBITER_H

/**
 * safety_arbiter.h — "规则主干 vs 端到端模型" 控制命令仲裁（纯逻辑）
 *
 * 从 safety_control_node.cpp 抽出，理由与 imu_protocol / perception_points /
 * pwm_map 相同：节点内的纯逻辑只能靠测试副本覆盖，副本漂移时 CI 不报。抽成 .c 后
 * tests/test_adas_nodes_logic.c 直接编同一份实现。
 *
 * 只用普通 C 结构体 —— 节点私有的 ControlCmd（含 std::string mode）留在节点侧做映射，
 * 这样安全关键路径的数据结构不动，行为逐位一致。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 仲裁输入/输出（只含仲裁关心的字段）。 */
typedef struct {
    double throttle;    /**< [0, 1] */
    double brake;       /**< [0, 1] */
    double steer;       /**< rad，正右负左 */
    int    turn_signal; /**< 0=off, 1=left, 2=right */
    int    hazard;      /**< 0/1 双闪 */
    int    gear;        /**< 档位（透传规则主干） */
} SafetyArbiterCmd;

/** 模型转向偏离规则基线超过该值（rad）→ 拒绝模型转向。 */
#define SAFETY_ARBITER_STEER_ENVELOPE_RAD  0.12
/** 规则制动力超过该值 → 纵向以规则安全制动为准，禁止模型油门。 */
#define SAFETY_ARBITER_RULE_BRAKE_ACTIVE   0.10
/** 规则未制动时，模型油门的上限基准（取 max(rule.throttle, 它)）。 */
#define SAFETY_ARBITER_MODEL_THROTTLE_CAP  0.85

/**
 * 仲裁规则主干与模型命令。
 *
 *   1) 无新鲜模型 或 系统降级 → 原样返回 rule_cmd（不算干预）
 *   2) 转向：|model.steer − rule.steer| > 0.12 → 用规则转向并标记干预；否则采纳模型
 *   3) 纵向：rule.brake > 0.10 → 取 max(rule.brake, model.brake)、throttle 归零、标记干预；
 *            否则 throttle = min(model.throttle, max(rule.throttle, 0.85))、brake 采纳模型
 *   4) 灯光/档位：转向灯规则优先、双闪取或、档位透传规则
 *
 * @param rule_cmd         规则主干命令（NULL → 返回全零、无干预）
 * @param model_cmd        模型命令（NULL 视为无新鲜模型）
 * @param has_fresh_model  模型是否新鲜
 * @param is_degraded      系统是否降级
 * @param out_intervened   输出：是否发生干预（可为 NULL）
 * @return                 仲裁后的命令
 */
SafetyArbiterCmd safety_arbiter_apply(const SafetyArbiterCmd* rule_cmd,
                                      const SafetyArbiterCmd* model_cmd,
                                      int has_fresh_model,
                                      int is_degraded,
                                      int* out_intervened);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_ARBITER_H */
