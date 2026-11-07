#ifndef PLANNING_MODEL_ASSIST_H
#define PLANNING_MODEL_ASSIST_H

/*
 * 方向四：模型层 → planning 决策层的**软建议**融合（纯逻辑，header-only）。
 *
 * 规则层仍是速度/轨迹的权威（CLAUDE.md 模块职责铁律）。模型只允许在规则目标
 * 附近提出"更优解"，且受到硬边界：
 *   - 置信度 < min_conf → 完全忽略模型（规则值原样返回）；
 *   - 模型建议被夹在 [rule*0.6, rule*1.2] 内（不允许模型把巡航速度翻倍/刹死）；
 *   - 最终按 assist_gain 线性混合：rule + gain*(bounded_model - rule)。
 * 默认 gain=0（等价关闭），由 planning 参数 `model_assist` 显式打开。
 */

#include <math.h>

#include "safety_arbiter.h"   /* 置信度门限与安全仲裁共用同一默认值 */

#ifdef __cplusplus
extern "C" {
#endif

#define PLANNING_MODEL_ASSIST_LOW_RATIO   0.6
#define PLANNING_MODEL_ASSIST_HIGH_RATIO  1.2
/* 默认置信度门与 safety_arbiter 同源（不另写 0.5 字面量）。 */
#define PLANNING_MODEL_ASSIST_CONF_MIN    SAFETY_ARBITER_MODEL_CONF_MIN

static inline double planning_model_assist_blend(double rule_speed,
                                                 double model_speed,
                                                 double model_conf,
                                                 double min_conf,
                                                 double gain) {
    if (!isfinite(rule_speed)) return rule_speed;
    if (gain <= 0.0 || !isfinite(gain)) return rule_speed;
    /* 硬边界：gain 只允许 [0,1]（>1 会把夹紧后的模型值外推，越过 0.6x~1.2x）。 */
    if (gain > 1.0) gain = 1.0;
    if (!isfinite(model_speed) || !isfinite(model_conf)) return rule_speed;
    if (model_conf < min_conf) return rule_speed;

    const double lo = rule_speed * PLANNING_MODEL_ASSIST_LOW_RATIO;
    const double hi = rule_speed * PLANNING_MODEL_ASSIST_HIGH_RATIO;
    double bounded = model_speed;
    if (bounded < lo) bounded = lo;
    if (bounded > hi) bounded = hi;

    double out = rule_speed + gain * (bounded - rule_speed);
    if (!isfinite(out)) return rule_speed;
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* PLANNING_MODEL_ASSIST_H */