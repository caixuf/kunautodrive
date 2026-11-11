# FlowSim 物理引擎 — 6-DOF 簧载悬架、轮胎动力学与碰撞沉淀

> FlowSim 提供**运动学自行车 / 2-DOF 轮胎动力学 / 6-DOF 簧载质量悬架（俯仰 pitch + 侧倾 roll + 纵向轴荷转移 $\Delta F_z$）+ 局部湿滑摩擦区 `friction_zones` + 坡道高程重力补偿 + OBB 碰撞/护栏**的全套车辆物理仿真能力。
> 本文沉淀物理引擎的模型选择、M4 6-DOF 升级架构、碰撞检测接入流程与工程踩坑。
> 改动 `flowsim` 物理/碰撞/重力相关代码前先读本文。

## 1. 物理模型（运动学 / 2-DOF 动力学 / M4 6-DOF 簧载悬架）

- 定义：`modules/adas_nodes/flowsim/physics.h`；实现 `modules/adas_nodes/flowsim/physics.cpp`；离线原型与参数对照 `tools/tire_dynamics_sim.py`。
- **运动学基线 `step_bicycle`**（`physics.cpp`）：纵向驱动/制动/风阻合力 + 转向执行器一阶滞后限幅 + yaw_rate 积分。速度钳制 `-4 ~ 60 m/s`。
- **2-DOF 横向轮胎动力学 `step_bicycle_dynamic`**：积分车身侧向速度 `v_y_body` 与横摆角速度 `yaw_rate`，含滑移角饱和（`±0.12 rad`）、摩擦圆护栏以及低速 `< 5 m/s` 运动学平滑退化。
- **M4 6-DOF 簧上质量悬架与环境耦合**：
  - **俯仰（Pitch）与侧倾（Roll）动态解算**：根据纵向加速度 $a_x$（制动点头 dive / 加速抬头 squat）与侧向向心加速度 $a_y$（过弯离心侧倾）二阶阻尼积分车身姿态角 `pitch`、`roll`，透传至 `VehicleState`、`scene/frame` 与 CARLA 6-DOF 桥接器；
  - **纵向轴荷转移 $\Delta F_z$**：前/后轴法向载荷随纵向加减速动态重分配，耦合前后轮地面最大附着力包络；
  - **局部湿滑摩擦区（`friction_zones`）**：场景 JSON 可声明矩形低附着区（如 `wet_road_evasion.json` 的 `surface: "wet"`, `mu_scale: 0.35` 或结冰路面），命中区域内轮胎峰值摩擦系数按最小 `mu_scale` 实时折减，并输出至 HUD `μ` 指标与 3D 积水/结冰贴花；
  - **道路坡度高程驱动与重力补偿**：由路网高程剖面推导 `road_grade`，在纵向动力学中注入坡道重力分力 $-g \sin\theta$。
- **M4 16 线束旋转 3D OBB LiDAR**：配合 6-DOF 车身与 3D 障碍物尺寸，`modules/adas_nodes/lidar_3d.h` 与 `sensor_model_node.c` 执行 16 线几何光线投影、视线遮挡裁剪、高斯测距噪声与 3D OBB 旋转包围盒输出。
- 启用方式：`config/pipeline.json` 的 `flowsim` 参数 `physics_model`（`kinematic` 或 `dynamic`）。Ego 在 `flowsim_node.cpp` 步进；NPC 在 `npc_ai.cpp` 走 IDM + Route Frenet + 可选 MOBIL 自主变道。

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
- **3D 姿态分离渲染（M5 `VehicleView.js`）**：前端将车轮/底盘（簧下质量）贴合 `roadHeightAt + Y_ROAD(0.10)`，而车身主体挂载在 `sprungGroup`（簧上质量）上叠加后端实时解算的 `pitch` 与 `roll`，使急刹点头、过弯侧倾和匝道爬坡在视觉与物理上完全同源。

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

## 6. 关联文档

- [ALGORITHM_STACK.md](ALGORITHM_STACK.md)：M4 6-DOF 簧载悬架、轴荷转移、湿滑摩擦区与 M6 OpenDRIVE 1.4 / CARLA 6-DOF 桥接契约。
- [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md)：仿真三层体系与 38 场景回归验证（含 `wet_road_evasion.json`）。
- [book/20_flowsim_scenario_design.md](book/20_flowsim_scenario_design.md)：高架高程、多段路网与 NPC 交互设计。
