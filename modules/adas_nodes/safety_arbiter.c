/**
 * safety_arbiter.c — 安全仲裁纯逻辑实现（详见 safety_arbiter.h）
 *
 * 逐行对应 safety_control_node.cpp::arbitrate_control 的语义（含未显式赋值的字段
 * 从 rule_cmd 继承这一点），只把 std::fabs/std::max/std::min 换成 C 的 fabs/fmax/fmin。
 */

#include "safety_arbiter.h"

#include <math.h>
#include <string.h>

SafetyArbiterCmd safety_arbiter_apply(const SafetyArbiterCmd* rule_cmd,
                                      const SafetyArbiterCmd* model_cmd,
                                      int has_fresh_model,
                                      int is_degraded,
                                      int* out_intervened) {
    SafetyArbiterCmd zero;
    memset(&zero, 0, sizeof(zero));
    if (!rule_cmd) {
        if (out_intervened) *out_intervened = 0;
        return zero;
    }
    if (!has_fresh_model || is_degraded || !model_cmd) {
        if (out_intervened) *out_intervened = 0;
        return *rule_cmd;
    }

    SafetyArbiterCmd out = *rule_cmd;
    int intervened = 0;

    /* 1. 转向角安全包络：模型偏离规则基线超过 6.9° 判定超限，拒绝模型转向 */
    const double delta_steer = fabs(model_cmd->steer - rule_cmd->steer);
    if (delta_steer > SAFETY_ARBITER_STEER_ENVELOPE_RAD) {
        out.steer = rule_cmd->steer;
        intervened = 1;
    } else {
        out.steer = model_cmd->steer;
    }

    /* 2. 纵向安全仲裁：规则处于制动态时以规则为主，禁止模型油门冲撞 */
    if (rule_cmd->brake > SAFETY_ARBITER_RULE_BRAKE_ACTIVE) {
        out.brake = fmax(rule_cmd->brake, model_cmd->brake);
        out.throttle = 0.0;
        intervened = 1;
    } else {
        const double max_thr = fmax(rule_cmd->throttle, SAFETY_ARBITER_MODEL_THROTTLE_CAP);
        out.throttle = fmin(model_cmd->throttle, max_thr);
        out.brake = model_cmd->brake;
    }

    /* 3. 灯光与档位继承规则安全态 */
    out.turn_signal = (rule_cmd->turn_signal != 0) ? rule_cmd->turn_signal
                                                   : model_cmd->turn_signal;
    out.hazard = (rule_cmd->hazard || model_cmd->hazard) ? 1 : 0;
    out.gear = rule_cmd->gear;

    if (out_intervened) *out_intervened = intervened;
    return out;
}
