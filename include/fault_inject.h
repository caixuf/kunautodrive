#ifndef FAULT_INJECT_H
#define FAULT_INJECT_H

/*
 * 传输层故障注入 — L3-P2 验证体系（方向六）。
 *
 * 为什么存在：P2 的放行条件是「**每类注入故障都有可观测的降级路径**」。
 * 在此之前，唯一的注入手段是 safety_control 节点内的 `raw_cmd_timeout`
 * （见 safety_fault_injection.h），它只能制造"控制指令超时"这一类故障；
 * 节点掉线、topic 丢包、传感器失效都无法注入 → 对应的降级路径**从未被验证**，
 * 放行条件在验收层面是空的。
 *
 * 设计要点：
 *   - **单点收口**：注入点放在 `transport_publish()`（全仓每个节点发布都经由此，
 *     无一例外），而不是每个节点各加一处。加一处即覆盖全部 topic。
 *   - **零成本关闭**：未配置时 `g_any_active == false`，热路径只多一次原子读，
 *     与 message_bus 的 remap 同一套手法（见 message_bus.c 的 remap_active）。
 *   - **确定性**：按仿真/单调时钟的"起始后 N 毫秒"触发，不用随机、不用负载相关
 *     的判断，同一配置每次跑出同一条时间线（可复现才能做门禁）。
 *   - **不进量产**：仅由环境变量开启，默认 pipeline.json 不含任何规则。
 *
 * 规格格式（环境变量 `FLOW_FAULT_INJECT`）：
 *
 *   <topic>:<after_ms>[:<duration_ms>][;<topic>:<after_ms>[:<duration_ms>]...]
 *
 * 例：
 *   perception/obstacles:5000                 # 5s 后永久丢弃感知障碍
 *   fusion/localization:3000:4000             # 3s 起丢弃定位，持续 4s 后恢复
 *   planning/trajectory:2000;control/cmd:2000 # 两个 topic 各自 2s 起丢弃
 *
 * duration_ms 缺省/为 0 = 永久注入（模拟节点掉线）。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAULT_INJECT_MAX_RULES 8
#define FAULT_INJECT_TOPIC_LEN 64

typedef struct {
    char     topic[FAULT_INJECT_TOPIC_LEN];
    uint64_t start_after_us;   /* 注入起点（相对 init 时刻） */
    uint64_t duration_us;      /* 0 = 永久 */
    bool     active;
} FaultInjectRule;

/**
 * 解析故障注入规格并初始化。
 *
 * @param spec     规格串（通常来自 getenv("FLOW_FAULT_INJECT")）；NULL/空 = 禁用。
 * @param start_us 注入起点的时间基（调用方传入当前时钟，便于测试注入假时钟）。
 * @return 成功解析的规则数（0 = 未启用注入）。
 *
 * 规格中任何**非法段落**都会被跳过并记 WARN，不影响其余段落的解析——
 * 故障注入是测试基础设施，一条写错不该让整条管线起不来。
 */
int fault_inject_init(const char* spec, uint64_t start_us);

/** 是否配置了至少一条规则。false 时热路径直接短路。 */
bool fault_inject_enabled(void);

/** 当前时间是否落在某条规则的注入窗口内。
 *  @return 应该丢弃该 topic 的消息时返回 true。 */
bool fault_inject_should_drop(const char* topic, uint64_t now_us);

/** 已配置的规则数（供 monitor/评估器登记"本次跑了哪些注入"）。 */
int fault_inject_rule_count(void);

/** 已累计丢弃的消息数（按 topic 汇总；供评估器确认注入真的生效）。 */
uint64_t fault_inject_dropped_count(void);

/** 拷贝第 i 条规则（i 越界返回 false）。 */
bool fault_inject_get_rule(int index, FaultInjectRule* out);

/** 清空全部规则（单测用）。 */
void fault_inject_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_INJECT_H */
