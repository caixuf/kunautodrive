#ifndef L3_EDR_H
#define L3_EDR_H

/*
 * L3 法规事件数据记录（EDR）— L3-P2 方向六。
 *
 * 为什么存在：PEM 里此前只有 **degrade_transition** 一类事件
 * （monitor_node.c 的 `pem/degrade_event`），L3 特有的四类**一个都没有**：
 *   · 系统激活/退出 —— L3 法规要求记录"系统何时接管/停止负责驾驶任务"
 *   · TOR 请求 / 驾驶员接管 / 超时
 *   · MRM 进入 / 完成
 *   · 碰撞（含**前后窗口**可回溯）
 *
 * 本头文件是事件分类的 C 侧单一事实源，与 `tools/l3_edr_sim.py` 的
 * `EDR_TAXONOMY` **逐条对应**（改一处必须同步另一处；Python 侧先跑通再移植）。
 *
 * 记录语义：
 *   - **只在跃迁时记一条**。TOR/ODD 状态 topic 是 10~20Hz 持续发布的，若每帧
 *     都记，PEM 会被稳态重复事件淹掉，真正的取证信息被埋。故 monitor 侧做
 *     边沿检测（见 monitor_node.c 的 pem_last_* 快照）。
 *   - **关键事件走 fsync**（`critical=true`）：L3 取证要求掉电不可丢；非关键
 *     的周期量沿用既有 buffer 策略。
 *   - 每条固定 8 个 double（PEM_RECORD_VALUE_COUNT），字段顺序见下表。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 事件名（写入 PEM 记录的 name 字段；统一 l3_ 前缀便于 grep）── */
#define L3_EDR_EV_ACTIVATION "l3_activation"
#define L3_EDR_EV_TOR        "l3_tor"
#define L3_EDR_EV_MRM        "l3_mrm"
#define L3_EDR_EV_ODD_EXIT   "l3_odd_exit"
#define L3_EDR_EV_COLLISION  "l3_collision"

/* ── 碰撞前后窗口（秒）——法规取证最小要求 ──
 * 本项只记碰撞**本身**这一条；"前后窗口"由 PEM 持续记录的周期性 topic
 * （vehicle/state 等）提供。故 PEM 必须持续记录这些量，不能只在事件时刻记
 * —— 这是"碰撞前后 N 秒可回溯"的实现方式。 */
#define L3_EDR_COLLISION_PRE_WINDOW_S  5.0
#define L3_EDR_COLLISION_POST_WINDOW_S 5.0

/* ── TOR 状态码（与 tor_manager_node.c 的 TOR_ST_* 对齐；此处复刻为稳定数值，
 *    避免 EDR 依赖 tor_manager 的内部枚举而耦合）── */
#define L3_EDR_TOR_OFF              0
#define L3_EDR_TOR_ACTIVE           1
#define L3_EDR_TOR_REQUESTED        2
#define L3_EDR_TOR_DRIVER_TAKEOVER  3
#define L3_EDR_TOR_MRM              4

/** 把 tor_manager 的状态字符串转成稳定数值（未知 → -1）。 */
int l3_edr_tor_state_code(const char* state_name);

/**
 * 字段填充辅助：把 8 元数组清零后再填指定位置，保证未用槽显式为 0
 * （避免"字段含义靠猜"）。
 */
void l3_edr_values_init(double values[8]);

#ifdef __cplusplus
}
#endif

#endif /* L3_EDR_H */
