/**
 * VehicleActor.h — 车辆行为 Actor（Layer 3）
 *
 * 从 Entity 已有状态（steer/brake/speed/ai_state）派生车灯信号，
 * 写入 Entity.lights（VehicleLights 位掩码）。
 *
 * 设计决策（方案 D）：
 *   不改 ControlRaw / control_node / actuator 的 cmd 链路。
 *   ControlRaw 是 msg_codegen.py 自动生成的二进制消息（DO NOT EDIT），
 *   改它需要重新 codegen + 改所有订阅节点。而车灯信号完全可以从 ego
 *   已有的 steer/brake/speed 派生，不需要额外的传输字段。
 *
 *   control_node 的灯信号决策逻辑下沉到此处，flowsim_node tick 末尾调用。
 *   NPC 的灯信号根据 ai_state（CutIn→转向灯，Stop→双闪，etc.）派生。
 *
 * 调用时机：flowsim_node tick 里 step_bicycle 之后、scene_pub 之前。
 */

#ifndef FLOWSIM_VEHICLE_ACTOR_H
#define FLOWSIM_VEHICLE_ACTOR_H

#include "../entity.h"

namespace flowsim {

/**
 * 车辆行为 Actor。纯静态方法，无实例状态。
 * 灯信号从 Entity 状态实时派生，不需要持久化计时器（转向灯随 steer 实时亮灭）。
 */
class VehicleActor {
public:
    /**
     * 更新 ego 车灯（BCM 车身域控制）。在 step_bicycle 之后调用。
     * 规则（spec §5.3 & BCM 域解耦规范）：
     *   - manual_low_beam 为 true 时强制点亮近光大灯与示廓灯（车主手动最高优先级）；
     *   - AUTO 模式下由环境光照/天气传感器（dark/low_visibility/foggy）自动点亮；
     *   - steer > 0.1  → 左转向灯（仅在 ADAS 未下发转向意图时兜底）
     *   - steer < -0.1 → 右转向灯
     *   - brake > 0.1  → 刹车灯（由 VehicleView 直接读 brake 字段，不占 lights 位）
     * @param ego             ego Entity（index 0）
     * @param low_visibility  环境是否处于暗光/低能见度（AUTO 自动感应开启大灯条件）
     * @param foggy           环境是否处于浓雾/沙尘暴（AUTO 开启雾灯条件）
     * @param manual_low_beam 驾驶员是否手动开启近光灯（手动覆盖最高优先级）
     */
    static void update_ego_lights(Entity& ego, bool low_visibility, bool foggy,
                                  bool manual_low_beam = false);

    /**
     * 更新 NPC 车灯。在 npc_ai tick 之后调用。
     * 规则：
     *   - ai_state==CutIn  → 根据 target_offset 方向打转向灯
     *   - ai_state==Merge  → 汇入方向打转向灯
     *   - ai_state==Yield  → 双闪（让行警示）
     *   - ai_state==Stop 且 speed<0.5 → 双闪（紧急停车）
     *   - 其余状态 → 转向灯灭（刹车灯仍由 brake 字段驱动）
     *   - AUTO 模式根据环境光照/能见度自适应启闭大灯
     * @param npc             NPC Entity
     * @param low_visibility  环境是否处于暗光/低能见度
     * @param foggy           环境是否处于浓雾/沙尘暴
     */
    static void update_npc_lights(Entity& npc, bool low_visibility, bool foggy);

    /**
     * 更新所有车辆实体的车灯（BCM 集中处理）。遍历 EntityPool，对每个 is_vehicle()
     * 的实体调用对应的 update 方法。flowsim_node tick 末尾调用一次。
     * @param pool                实体池
     * @param low_visibility      环境是否处于暗光/低能见度
     * @param foggy               环境是否处于浓雾/沙尘暴
     * @param manual_ego_low_beam 驾驶员是否手动开启 ego 近光灯
     */
    static void update_all_lights(EntityPool& pool, bool low_visibility, bool foggy,
                                  bool manual_ego_low_beam = false);
};

}  // namespace flowsim

#endif  // FLOWSIM_VEHICLE_ACTOR_H
