# FlowSim 物理引擎 — 现状与经验沉淀

> FlowSim 是 2D 平面运动学/动力学 + OBB 碰撞/护栏 + 垂直重力贴地的三层能力。
> 本文沉淀物理引擎的模型选择、碰撞检测接入流程、垂直物理改造点与踩过的坑。
> 改动 `flowsim` 物理/碰撞/重力相关代码前先读本文。

## 1. 物理模型：仍是 2D 平面，但有动力学选项

- 定义：`modules/adas_nodes/flowsim/physics.h`；实现 `physics.cpp`。
- **默认 `step_bicycle`**（运动学自行车模型，`physics.cpp:79-110`）：纵向力（drive 5000N / brake 8000N / drag 按系数×v²，`longitudinal_accel` :47-53）+ 转向执行器一阶滞后限幅（`update_steer` :56-72）+ yaw_rate 积分。**全在 x/y 平面，无 z、无重力**。速度钳制 -4~60 m/s（:84-85）。
- **可选 `step_bicycle_dynamic`**（线性轮胎二自由度，`physics.cpp:160-180`）：`integrate_lateral_dynamics`（:120-158）解 v_y_body + yaw_rate，含滑移角饱和 ±0.12 rad（`SLIP_ANGLE_MAX` :35）与摩擦圆护栏（:143-146）；低速 <5 m/s 退化为运动学（`LOW_SPEED_MS` :30，:162-167）。
- 启用：`flowsim_node.cpp:132,236,2376-2379` 读 pipeline.json 的 `physics_model`（默认 `kinematic`）。Ego 步进 `flowsim_node.cpp:1832-1836`；NPC 在 `step_npc_vehicle`（npc_ai.cpp）。

## 2. 碰撞检测（collision.cpp/h）

- **两阶段**：AABB broad-phase（`collision.cpp:86-90`）→ OBB SAT narrow-phase（`obb_intersect` :46-68，4 分离轴）。
- `detect_collisions`（:70-100）：只对 is_vehicle() 对；**任一方 `crash_cooldown>0` 跳过该对**（:83）。
- `apply_collision_response`（:102-169）：双方速度归零（vx=vy=speed=0，:107-108）+ 刹车踩死（:110-111）；分离策略——route 模式沿 route_s 纵向拉开 2m（:119-128），非 route 模式沿 x/y 重叠量较小的 MTV 轴各推一半（:141-159）。
- `apply_guardrail`（:171-202）：反查 Frenet，超路缘钳制 offset 回投世界坐标，速度×0.3 并进冷却（:198-201）。
- `apply_gravity`（:204-241）：垂直物理（见 §4）。

### 接入主循环（flowsim_node.cpp:2072-2110）
- Step 4（碰撞检测+响应）:2072-2089：`detect_collisions` → `apply_collision_response`；涉及 ego 则 `publish_sim_collision`（:2087），碰撞位置跳变标记 `last_teleport_cycle` 供 invariant 跳过 Δpos 检查（:2079-2080）。
- Step 4.5（护栏）:2091-2100：`apply_guardrail`（需 roads_loaded）。
- Step 4.6（重力）:2102-2110：`apply_gravity`（需 roads_loaded）。
- `publish_sim_collision`（sim/collision topic）:1302-1325，含 overlap_x/y 估算。
- 单测：`test_collision_events.cpp`（分离/重叠/OBB 旋转区分/响应速度归零/排除行人，:21-103）。

## 3. 实体池边界检查（entity.h）

- `MAX_ENTITIES=128`（:30）；EntityPool 固定池（:190-263）。
- **`operator[]` 带边界检查**（:222-236）：id 越界返回 static invalid_entity（`active=false`）。Ego 固定 index 0。
- `Entity` 含垂直速度 `vz`（:86）、`crash_cooldown`（:125）、`route_s/route_dir`（:134-135）、`last_teleport_cycle`（:151）。

## 4. 车辆 z 定位 / 重力贴地

- **z 数据来源**：`FlowRoadNetwork::frenet_to_world` `road_network.cpp:96-109`，`out.z = pd.z`（OpenDRIVE `<elevationProfile>` 高架高程，平地恒 0）。
- **后端贴地（`apply_gravity`）**：每 tick `vz -= 9.81*dt`（:207）；反查 `world_to_frenet` → `frenet_to_world` 取路面高度 ground_z（:211-220）；在道路上且 z≤ground_z → z=ground_z、vz=0（:229-232）；高于路面 → 下落落回（:233-240）；不在道路上 → 自由下落，钳制 z≥-200（:222-227）。
- **前端贴地（独立实现）**：`roadHeightAt` + `Y_ROAD=0.10`（路面厚度），车辆 y = 道路高度 + 0.10，匝道/高架自动跟随，非固定偏移。
- **语义差异待对齐**：后端 `apply_gravity` 贴地 z 直接用道路高程，**未叠加 Y_ROAD(0.10) 路面厚度**；前端 y 落在高程+0.1。两端贴地语义不同，后续改造需统一。地面支撑目前是硬贴合（z=ground_z 即 vz=0），无悬挂/轮胎模型。

## 5. 经验坑速查（改物理/碰撞必读）

1. **碰撞冷却期耦合 bug**：`detect_collisions` 不跳过冷却期实体对 → 每 tick 重叠仍报告 → `apply_collision_response` 又重置 cooldown → 冷却永远归零 → 两车永久卡死（`collision.cpp:80-83`）。这是最容易踩的坑。
2. **分离不能沿中心连线任意方向推**：route 模式必须沿 route_s 纵向拉开（:113-128），否则 heading 偏差时把车推出路外，冷却结束后 world_to_frenet 失败飞出（E3 修复）。
3. **非 route 分离要选 MTV（min-overlap 轴）**：OBB SAT 不返回穿透深度，用 AABB 重叠量近似；沿中心连线推在 max-overlap 对齐时无法脱开 → 仍卡死（:130-159）。
4. **冷却时长经验值**：碰撞冷却 2.0s 太长会堵死整条路，改为 0.5s（:162-167）；护栏冷却 0.3s（:201）。
5. **碰撞分离是位置跳变**：必须打 `last_teleport_cycle` 标记，否则 temporal invariant 误报（`flowsim_node.cpp:2078-2080`）。
6. **仅涉及 ego 才发布 sim/collision topic**：NPC 间重叠不发消息，避免 evaluator 误判（:2085-2087）。
7. **动力学模型稳定护栏**：线性轮胎 + 滑移角硬饱和下高速持续转向 v_y 发散（PR #72 实测 v_y 涨到 5×10⁴），需加 |v_y|≤vx·tan(slip)·1.5 与 |r|≤0.8g/vx 护栏（`physics.cpp:137-146`）；低速 <5m/s 退化为运动学防前向欧拉发散（:30,162）。
8. **运动学模型用车辆中心而非后轴参考点**：需叠加 half_wb·yaw_rate 旋转项，否则车尾横滑（`physics.cpp:94-109`）。
9. **纵向力耗散方向随运动方向**：brake/drag 符号必须跟 v 的符号，否则倒车时越刹越快（`physics.cpp:47-53`）。
10. **重力贴地 z 与前端 Y_ROAD 语义差异**：见 §4，改造时统一。

## 6. 文档覆盖现状

- `docs/SIMULATION_GUIDE.md`：运动学模型（:108-111）、collision 日志 grep（:79）。**未覆盖** OBB 算法/响应/护栏/重力。
- `docs/tutorials/16_flowsim_scenario_design.md`：高架 elevation/z 抬升约定（:38,45）、碰撞/幽灵变道调试（:61,114）。
- `docs/DATA_CLOSED_LOOP.md`：完全未涉及物理。
