---
name: flowsim-physics
description: FlowSim 物理引擎经验：运动学/动力学模型切换、OBB 碰撞检测接入流程（Step 4/4.5/4.6）、护栏、重力贴地、实体池边界。改动 flowsim 物理/碰撞/重力相关代码时使用。
---

# FlowSim 物理引擎

沉淀自 `collision.cpp/h` 新提交与物理模型演进。改动前先看 [FLOWSIM_PHYSICS.md](../../docs/FLOWSIM_PHYSICS.md)。

## 什么时候用

- 改 `flowsim/physics.cpp`、`collision.cpp/h`、`entity.h`、`flowsim_node.cpp` 的 Step 4/4.5/4.6
- 调 `physics_model`（kinematic / dynamic）、碰撞响应、护栏、重力贴地

## 模型选择

- 默认 `step_bicycle`（运动学自行车，全 x/y 平面无 z）。dynamic 用 `step_bicycle_dynamic`（线性轮胎二自由度，含滑移角饱和 + 摩擦圆护栏），由 pipeline.json `physics_model` 控制。
- 低速 <5 m/s 退化为运动学，防前向欧拉发散。
- 运动学模型用车辆中心而非后轴参考点，须叠加 `half_wb·yaw_rate` 旋转项，否则车尾横滑。

## 碰撞接入主循环（三段）

1. **Step 4**（`flowsim_node.cpp:2072-2089`）：`detect_collisions` → `apply_collision_response`；涉及 ego 才 `publish_sim_collision`；碰撞位置跳变打 `last_teleport_cycle` 标记。
2. **Step 4.5**（:2091-2100）：`apply_guardrail`（需 roads_loaded）。
3. **Step 4.6**（:2102-2110）：`apply_gravity`（需 roads_loaded）。

## 必守约定

- `detect_collisions` **必须跳过 `crash_cooldown>0` 的实体对**，否则冷却被每 tick 重置 → 永久卡死。
- 碰撞分离：route 模式沿 route_s 纵向拉开（别沿中心连线推，会推出路外）；非 route 模式选 MTV（min-overlap 轴）。
- 碰撞是位置跳变，必须打 `last_teleport_cycle`，否则 temporal invariant 误报。
- 仅涉及 ego 才发 sim/collision topic，NPC 间重叠不发，避免 evaluator 误判。
- `EntityPool::operator[]` 带边界检查（越界返回 invalid_entity）。

## 重力贴地

- z 来自 `frenet_to_world` 的 `out.z = pd.z`（elevation，平地恒 0）。
- 后端 `apply_gravity`：vz 每 tick -9.81·dt；反查道路 ground_z；z≤ground_z 则贴地、vz=0；不在道路自由下落钳 z≥-200。
- **语义差异**：后端贴地 z 落在道路高程，前端 `roadHeightAt + 0.10`（Y_ROAD）落在高程+0.1。改造时需统一。

## 关键坑速查

1. 冷却期耦合 → 永久卡死（最易踩）。
2. 分离方向错 → 推出路外 / 卡死。
3. 碰撞冷却 2.0s 太长堵路，用 0.5s；护栏 0.3s。
4. 动力学模型高速持续转向 v_y 发散（PR #72），需 |v_y|≤vx·tan(slip)·1.5 与 |r|≤0.8g/vx 护栏。
5. brake/drag 符号必须跟 v 符号，否则倒车越刹越快。

## 改完必跑

```
./build/bin/test_collision_events     # 分离/重叠/OBB 旋转/响应归零/排除行人
bash scripts/demo.sh --no-browser 45
python3 ci/evaluators/demo_evaluator.py
```
