/**
 * lane_match_helpers.h — `localization/lane_match` M3 字段纯算法 helper
 *
 * 抽出 `flowsim_node.cpp::compute_lane_match()` 的核心算法到独立函数，
 * 让 `tests/test_adas_nodes_logic.c`（C/C++ 单测目标）能 extern 声明
 * 直接调用，不用 esmini 路网、不用 flowsim_node.cpp 的 `g` 全局状态。
 *
 * 职责边界：
 *   - 这里只放 **纯数学 / 纯迭代** 算法
 *   - 涉及 esmini 路网的查询（world_to_frenet / lane_width /
 *     drivable_lane_count / GetDrivableLaneIdByIndex）留在 flowsim_node.cpp
 *     内部，调用本 helper
 *
 * D2-04 / D2-05 / D2-06 演化路径：
 *   - M3 step 1（本 PR）：合成 llt_id 占位（road_id*1000 + lane_id+500）
 *   - M3 step 2（D2-03）：flowsim 加载 lanelet::LaneletMap，合成公式退役，
 *     `flowsim_lm_synthesize_lanelet_id` 替换为 `lanelet::Lanelet::id()`
 *   - M3 step 3：可选，邻 lane 查找换成 LaneletMap::laneletLayer.find
 *
 * 命名约定：`flowsim_lm_*` 前缀（lane_match 缩写）+ extern "C" 链接，
 * 方便 C 测试入口（test_adas_nodes_logic.c）extern 声明。
 */

#ifndef FLOWSIM_LANE_MATCH_HELPERS_H
#define FLOWSIM_LANE_MATCH_HELPERS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 三点曲率近似（带符号）。
 *
 * 输入三个世界坐标点（任意分布，按顺序排列），返回有符号曲率 κ (1/m)：
 *   κ > 0 ⇒ 逆时针（左转弯）
 *   κ < 0 ⇒ 顺时针（右转弯）
 *   κ ≈ 0 ⇒ 近似直线
 *
 * 公式：Menger 曲率 κ = 2·cross / (|a|·|b|·|c|)，
 * cross = (P_2-P_1) × (P_3-P_1) 的 z 分量。
 *
 * 退化保护：
 *   - 三点共线 → cross ≈ 0 → 返回 0（不返回 NaN/Inf）
 *   - 两点重合 → 边长 < 1e-9 → 返回 0
 *   - 数值爆炸 → 分母 < 1e-18 → 返回 0
 */
double flowsim_lm_curvature_3pt(double x1, double y1,
                                double x2, double y2,
                                double x3, double y3);

/**
 * M2 / M3 step 1 占位：把 (road_id, lane_id) 合成成 llt_id。
 *
 * 公式：`road_id * 1000 + (lane_id + 500)`
 *   - road_id ≥ 0，lane_id 可正可负；`(lane_id + 500)` 把负值拉到 [0, 1000)
 *   - 整体落在 [500, road_id*1000+1500)，远未触及 uint64_t 上限
 *   - 调用方对 0 (world_to_frenet 失败) 须显式置 0，否则合成 llt_id=500
 *
 * ⚠ 仅 M3 step 2 之前使用：flowsim 加载真 Lanelet2 后替换为
 * `lanelet::Lanelet::id()`（spec §6.1 / D2-05 design §4）。
 */
uint64_t flowsim_lm_synthesize_lanelet_id(int road_id, int lane_id);

/**
 * 从候选 lane id 列表里挑出当前 lane 的邻 lane。
 *
 * OpenDRIVE 约定：lane id 0 = 参考线（不参与），正 id 在参考线左侧，
 * 负 id 在参考线右侧。**所以 lane N 的左邻是 id 最大的 > N 的 lane，
 * 右邻是 id 最小的 < N 的 lane**（closest-on-side 语义）。
 *
 * @param current_lane     当前 lane id（不能等于候选 id 之一；否则返回 0）
 * @param candidate_ids    候选 lane id 数组（驱动路上所有 drivable lane）
 * @param n                候选数组长度
 * @param side             +1 = 左邻；-1 = 右邻；0 = 无意义（返回 0）
 * @return 邻 lane id；无邻时返回 0
 */
int flowsim_lm_pick_adjacent_lane_id(int current_lane,
                                     const int* candidate_ids, int n,
                                     int side);

/**
 * 车道归属迟滞（贴分界线时防止 lane_id 每帧翻转）。
 *
 * 背景（2026-10-22 实测）：esmini `RM_SetWorldXYHPosition` 给出的 lane_id 是
 * **最近车道中心**的硬判据。自车贴在两条车道分界上时（实测直道巡航 ego 距
 * 分界 0.13m），该判据每帧在两侧 lane 之间翻转，`llt_offset` 随之在
 * +1.6 / -1.6 之间翻转 —— 下游把 `llt_offset` 当"相对当前车道中心"用，
 * 于是横向参考每次翻转都阶跃一个车道宽（实测 63 次/45s，车在两车道中心
 * 之间锯来锯去）。
 *
 * 规则：held_lane 是上一帧归属；只有当自车**越过**分界线超过
 * `LM_LANE_STICK_RATIO · lane_width` 之后才改判。0.25 与 planning 侧
 * `planning_coord::hold_lane_index` 同一余量口径，两层不会互相打架。
 *
 * 迟滞带要占一半车道宽，所以判据只做**符号化比较**，不用真实几何距离：
 * 传入 `offset_in_lane` = 自车相对**held 车道**中心的横向偏移（左正），
 * `lane_width` = 当前车道宽，`candidate_lane` = 硬判据给出的最近车道，
 * `lane_id_sign` = lane id 增大方向与横向正方向的符号关系（OpenDRIVE 靠右
 * 行驶时 lane id 增大 = 向左 = 横向正，传 +1；拿不准时传 +2 表示"未知"，
 * 此时只做单向保护，见下）。
 *
 * @param candidate_lane   硬判据（world_to_frenet）给出的最近 lane id
 * @param held_lane        上一帧归属 lane id（0 = 尚未锁存，直接采纳候选）
 * @param offset_in_lane   自车相对 held_lane 中心的横向偏移 (m)，左正
 * @param lane_width       held_lane 的车道宽 (m)；≤0 视为未知 → 直接采纳候选
 * @return 本帧应发布的 lane id
 *
 * 语义：
 *   - held_lane == 0（未锁存）或 lane_width 非法 → 返回 candidate_lane
 *   - candidate_lane == held_lane → 返回 held_lane（常规）
 *   - 候选在左（candidate > held，OpenDRIVE id 左增）：只有 offset_in_lane >
 *     +0.25·lane_width 才改判；否则保持 held
 *   - 候选在右（candidate < held）：只有 offset_in_lane < -0.25·lane_width 才改判
 *   - 因此自车在分界附近（|offset| < 0.25·w）时归属恒定，不会翻转
 */
int flowsim_lm_hold_lane_id(int candidate_lane, int held_lane,
                            double offset_in_lane, double lane_width);

/**
 * 失败路径：把 compute_lane_match 所有 10 个输出参数全部置 0。
 *
 * 调用方在 world_to_frenet / frenet_to_world 失败、或 road_id < 0 时调用，
 * 保证下游 cJSON 序列化时所有字段值稳定为 0（gate lane_match_schema_check
 * `valid=0` 路径不触发 `valid=1 requires llt_id > 0` 矛盾）。
 *
 * 10 参数顺序与 `compute_lane_match` 出参一致（M2 5 + M3 5）：
 *   llt_id, llt_s, llt_offset, llt_heading_err_rad, valid,
 *   curvature, lane_width, left_lanelet_id, right_lanelet_id, flags
 */
void flowsim_lm_zero_outputs_on_failure(
    uint64_t* out_llt_id, double* out_llt_s, double* out_llt_offset,
    double* out_heading_err, int* out_valid,
    double* out_curvature, double* out_lane_width,
    uint64_t* out_left_lanelet_id, uint64_t* out_right_lanelet_id,
    uint32_t* out_flags);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* FLOWSIM_LANE_MATCH_HELPERS_H */
