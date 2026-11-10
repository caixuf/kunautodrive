# KunAutoDrive 算法栈（实际实现）

> 本文是**当前代码里真实跑着的算法**总览（覆盖 Milestone M1~M6 全量落地算法）：
> 每个模块用什么算法、在哪个文件、关键约束是什么。深入某一模块请跳对应教程/设计文档。

## 数据流

```
flowsim(60Hz 真值, 6-DOF 悬架/坡道/湿滑) 
    → sensor_model(16线 3D LiDAR/FOV/遮挡/噪声/雷达) 
    → perception(DBSCAN 几何聚类 + M1 CenterPoint-Lite BEV CNN 纯 C 推理)
    → object_tracker(M2 64 槽匈牙利卡尔曼跟踪) 
    → prediction(M2 Frenet 交互式多模态预测 LK/CutIn/Yield)
    → fusion(EKF 定位与状态对齐)
    → behavior_planner(FSM 决策) ⇄ navigation(路由/travel_dir)
    → planning(M3 Scenario-Stage-Task 架构:
         - LaneFollow: Frenet 轨迹 + ST 图 DP 速度 + 交互切入预制动
         - ValetParking: Hybrid A* 开放空间泊车 + Reeds-Shepp 倒车入库
         - U-Turn: N 把方向多段机动)
    → control(LTV MPC 横向 + PID 纵向 + ManeuverTracker 挡位推进)
    → safety_control(TTC 安全闸门 + MRM) → 执行
旁路: 
    - M5: 3D 6-DOF / SR 世界模型 / HUD 沉浸式数字孪生 (flowboard)
    - M6: OpenDRIVE 1.4 (json_to_xodr + xodr_compat_check) / CARLA 6-DOF 桥接器 (carla_bridge)
    - 学习闭环: inference (tiny-MLP 影子) / data_recorder / learner / model_ota
```

## 模块 × 算法一览

| 模块 | 算法 | 文件 | 要点 |
|------|------|------|------|
| 仿真 (M4) | 运动学自行车 + 2-DOF 线性/Pacejka 动力学 + 6-DOF 簧上质量悬架（俯仰 pitch 制动点头/加速抬头、侧倾 roll 侧向载荷转移、纵向轴荷转移 $\Delta F_z$）+ 局部湿滑摩擦区（`friction_zones` / `mu_scale` 降级）+ 道路坡度高程驱动与重力补偿 | `flowsim/physics.cpp` `flowsim/entity.h` `tools/tire_dynamics_sim.py` | 纯被控对象，只执行指令不做驾驶决策；支持 `wet_road_evasion.json` 湿滑路面紧急避险验证 |
| NPC | IDM 跟车 + 中央 Route Frenet 车道跟随 + MOBIL（运行时 `enable_mobil`，状态保持/完成事件已修）+ 多 OD 链分配 + 行人路边行走 | `flowsim/npc_ai.cpp` `flowsim/route.cpp` `flowsim_node.cpp` | 安全间距 = 5.0 + v×1.5；行人沿 road s 推进 |
| 传感器模型 (M4) | 16 线 3D 旋转 LiDAR 几何投影与 3D OBB 旋转包围盒生成 + FOV 裁剪 + 视线遮挡 + 高斯噪声 + 毫米波雷达 | `sensor_model_node.c` `modules/adas_nodes/lidar_3d.h` `radar_model.h` | 3D 点云与包围盒输出，雷达经 `radar_post.h` 可选并入 perception |
| 感知 (M1) | DBSCAN 点云聚类 baseline + **CenterPoint-Lite BEV 卷积检测网络**（输入 [6, 88, 88] 真实传感器 6 通道多模态 BEV 特征栅格，4 层紧凑全卷积，输出类别热图、亚像素中心偏移、3D 尺寸与速度，`models/bev_centernet_v1.{bin,onnx}`，纯 C 原生推理核 `bev_cnn_native.c` / `tiny_bev.h`） | `perception_node.cpp` `modules/adas_nodes/bev_detection_node.cpp` `modules/adas_nodes/bev_cnn_native.c` `models/bev_centernet_v1.bin` `tools/train_bev/train_bev_centernet.py` | 经 `scenarios/bev_shadow.json` 影子评测，Recall 98.82%，位置 MAE 0.21m，PyTorch/ONNX/C 纯原生推理三端最大绝对误差 `3.81e-5`，推理帧率 30Hz+ |
| 跟踪 (M2) | **统一 64 槽方阵补零 Kuhn-Munkres 匈牙利卡尔曼跟踪**（4D 状态 [x, y, vx, vy] + `hungarian_tracker.h` 关联代价最优二分图匹配） | `object_tracker_node.c` `modules/adas_nodes/hungarian_tracker.h` `tools/tracker_prediction_sim.py` | 32 目标高噪声漏检下 0 丢槽，ID-Switch 下降 70.1%，速度 RMSE 下降 ≥25% |
| 预测 (M2) | **车道几何与前车交互耦合三模态预测器**（沿道路中心线投影 s/d，多模态概率评估 `lane_keep`, `cut_in_left/right`, `yield_decel`，未来 5s 轨迹生成）+ `st_graph.c` 预测切入障碍物注入 | `modules/adas_nodes/prediction_node.cpp` `modules/adas_nodes/prediction_interactive.h` `modules/adas_nodes/st_graph.c` `tools/tracker_prediction_sim.py` | 为下游规划提供障碍物未来时空占据包络，提前 1.5s 柔和预降速制动 |
| 融合 | EKF（GPS+IMU 定位、时间对齐） | `fusion_node.cpp` | 20Hz |
| 行为 | 8 状态 FSM：CRUISE / FOLLOW / LEFT_CHANGE / RIGHT_CHANGE / STOP / YIELD / U_TURN / EMERGENCY | `behavior_planner_node.cpp` | 只决定"做什么"，不输出连续控制量 |
| 导航 | 路由步骤（enter_noa / prepare_u_turn）+ travel_dir（唯一事实源 = flowsim `ref_path.reverse`） | `navigation_node.c` | 10Hz |
| 规划-架构 (M3) | **ScenarioManager 在线接入**（`LaneFollowScenario`, `ValetParkingScenario` 分层解耦架构） | `planning_node.cpp` `include/scenario_stage_task.h` | 统一抽象 Stage 生命周期与 Task 执行流，模块正交解耦 |
| 规划-路径 | Frenet 最优轨迹（横向五次多项式；变道段固定曲率圆弧回填 kappa）+ 可选模型软建议（`model_assist`，默认 0） | `planning_node.cpp` / `planning_model_assist.h` | 速度/轨迹唯一权威；模型建议被 0.6×~1.2× 规则值与置信度门夹住 |
| 规划-开放空间 (M3) | **ValetParkingScenario 开放空间泊车**（`APPROACH` → `OPEN_SPACE_CRUISE` Hybrid A* + Reeds-Shepp 尖点保留 64 点轨迹 → `PRECISION_HOLD`）+ ManeuverTracker D/R 挡闭环 | `planning_node.cpp` `tools/open_space_parking_sim.py` `scenarios/valet_parking.json` | 库位终态位置误差 < 0.25m，航向误差 < 3° (0.052 rad)，严格保证零碰撞 |
| 规划-速度 (M2) | **ST 图 + DP 速度规划 + 交互切入预制动**（红灯墙 + 动态障碍占据 + 曲率限速，90×101 DP 表；对 Cut-In 目标提前 1.5s 柔和预降速） | `st_graph.c` `planning_node.cpp` | 由 `tools/speed_planner_sim.py --run-all` 对照验证 |
| 规划-掉头 | N 把方向多段掉头（前进满舵弧 → 刹停换 R → 倒车反打 → 循环，≤5 把），512 点细生成 + 段感知下采样 64 | `planning_node.cpp` generate_uturn_trajectory | 生成一次即缓存重放，防「重规划抖动死循环」 |
| 控制-横向 | Stanley + kappa 前馈（默认）；LTV MPC（`use_ltv_mpc` 参数启用，机动模式跳过） | `control_node.cpp` `ltv_mpc.c` | MPC 求解前注入真实 steer 限幅（内外限幅必须一致） |
| 控制-纵向 | PID + ACC 跟车（anti-windup：error 翻负清正积分） | `control_node.cpp` | 20Hz |
| 控制-机动 (M3) | ManeuverTracker：弧长推进 + D/R 挡位状态机 + 倒挡横向反馈反号 | `maneuver_tracker.h` | 掉头/泊车共用；header-only 可独立单测 |
| 安全 | TTC 闸门（前车/迎头/行人/横向交叉，全部沿车头方向投影）+ MRM + 机动窗口豁免 + 模型置信度门（`ControlRaw.confidence < 0.5` 回退规则主干） | `safety_control_node.cpp` / `safety_arbiter.c` | FlowCoro 协程；只做限幅+紧急制动，不理解任务意图 |
| 数字孪生 (M5) | 3D 6-DOF 姿态渲染（`VehicleView.js` 簧上/簧下分离 `sprungGroup`）+ SR 世界模型图层（预测流光带/高度着色点云/泊车发光车位框/结冰积水贴花/高层退台建筑）与驾驶舱 HUD | `tools/flowboard/` `tools/flowboard/js/vis/view/VehicleView.js` `tools/flowboard/js/vis/view/WorldView.js` `tools/flowboard/js/vis/view/HudView.js` | 真实 3D 车辆俯仰/侧倾、路面湿滑高光与交互预测可视化 |
| 仿真生态桥接 (M6) | **OpenDRIVE 1.4 3D 路网/摩擦材质导出与预检** (`json_to_xodr.py` + `xodr_compat_check.py`) + **CARLA 6-DOF 双向桥接** (`carla_bridge.py`) | `tools/json_to_xodr.py` `tools/xodr_compat_check.py` `tools/carla_bridge.py` | 支持 3D 坡道高程 `elevationProfile`、超高横坡 `lateralProfile`、车道摩擦材质 `<material>`；左手系→右手 ENU 转换、6-DOF 姿态 `VehiclePose6DOF`、`friction_zones` 最小 `mu_scale` 命中与 `capabilities_6dof` 契约 |
| 学习 | BC（tiny-MLP / PyTorch）→ DAgger 自我对弈回灌 → PPO（换老师路线）；ONNX 导出 + 等价性门禁；影子评估 + promote 门禁 + OTA | `inference_node.cpp` `tools/train_e2e/` | 详见 [LEARNING_LOOP.md](LEARNING_LOOP.md) |

## M1~M6 全栈升级技术指标与落地基准

### 1. 感知层 (Milestone M1)
- **真实传感器 6 通道 BEV 栅格化**：将点云/高度/强度投影至 `[6, 88, 88]` 栅格特征图；
- **CenterPoint-Lite 紧凑全卷积网络**：`models/bev_centernet_v1.{bin,onnx}` 模型体积仅 ~71KB；
- **纯 C 原生推理核**：`modules/adas_nodes/bev_cnn_native.c` 无第三方推理框架依赖，帧率 30Hz+；
- **实测性能指标**：
  - 检测召回率 Recall：**98.82%**
  - 位置平均绝对误差 MAE：**0.21m**
  - PyTorch / ONNX / 纯 C 三端推理最大绝对误差：**`3.81e-5`**（数值完全等价）。

### 2. 跟踪与预测层 (Milestone M2)
- **统一 64 槽方阵补零 Kuhn-Munkres 匈牙利匹配器**：`modules/adas_nodes/hungarian_tracker.h`，支持 32 目标高噪声漏检下 0 丢槽；
  - 关联代价最优二分图匹配，实测 ID-Switch 下降 **70.1%**，速度估计 RMSE 下降 ≥25%；
- **车道几何与前车交互耦合三模态预测器**：`modules/adas_nodes/prediction_node.c` / `prediction_interactive.h`，沿 Frenet 道路中心线投影评估：
  - `lane_keep`（车道保持）
  - `cut_in_left` / `cut_in_right`（交互式切入）
  - `yield_decel`（减速让行）
- **ST 图切入障碍物注入**：`modules/adas_nodes/st_graph.c` 消费预测切入包络，提前 1.5s 柔和预降速制动。

### 3. 规划与控制层 (Milestone M3)
- **ScenarioManager 在线接入**：`planning_node.cpp` 中以 Scenario-Stage-Task 架构管理生命周期，支持 `LaneFollowScenario` 与 `ValetParkingScenario` 解耦流转；
- **ValetParkingScenario 开放空间泊车**：
  - 阶段状态机：`APPROACH` → `OPEN_SPACE_CRUISE` → `PRECISION_HOLD`；
  - 轨迹生成：Hybrid A* 启发式搜索 + Reeds-Shepp 双向平滑曲线，保留尖点（cusp）精确切分的前进/倒车 64 点参考轨迹；
  - 泊车精度：终态横纵向位置误差 < 0.25m，航向角误差 < 3° (0.052 rad)，严格保证零碰撞；
- **ManeuverTracker D/R 挡闭环**：`modules/adas_nodes/maneuver_tracker.h` 弧长推进 + 挡位自动切换状态机 + 倒挡横向反馈反号，掉头与泊车统一闭环跟随。

### 4. 物理仿真、3D 渲染与桥接层 (Milestones M4 ~ M6)
- **物理仿真与 3D 传感器 (M4)**：
  - 6-DOF 簧载悬架：车身俯仰（pitch 制动点头/加速抬头）、侧倾（roll 离心侧倾）动态解算；
  - 载荷转移与重力坡度：纵向轴荷转移 $\Delta F_z$ 计算，道路坡度高程驱动与坡道分力补偿；
  - 局部湿滑摩擦区：`friction_zones` / 动态 `mu_scale` 附着系数降级，验证 `wet_road_evasion.json` 极限工况；
  - 16 线束旋转 3D LiDAR：几何光线投影、视线遮挡、高斯测距噪声与 3D OBB 旋转包围盒生成；
- **数字孪生与 SR 世界模型 (M5)**：
  - `tools/flowboard/js/vis/view/VehicleView.js`：簧上/簧下分离 `sprungGroup` 真实姿态呈现；
  - SR 世界模型多图层：交互预测多模态流光带、高度渐变着色点云、泊车发光车位框、结冰积水路面贴花与高层退台建筑体块；
  - 驾驶舱 HUD：实时车速、挡位、前车时距、TTC 预警与横纵向加速度动态仪表；
- **OpenDRIVE 1.4 与 CARLA 6-DOF 桥接 (M6)**：
  - 3D 路网导出与预检：`tools/json_to_xodr.py` 导出 3D 高程 `elevationProfile`、超高横坡 `lateralProfile/superelevation` 与车道摩擦材质 `<material>`；
  - 物理与合规预检：`tools/xodr_compat_check.py` 拦截陡坡告警 `steep_elevation_grade` (|b| > 0.25) 与非法摩擦系数 `invalid_lane_friction`；
  - CARLA 6-DOF 双向桥接：`tools/carla_bridge.py` 实现 CARLA 左手系 deg → FlowEngine 右手 ENU 系 rad 转换（$y = -\text{loc}.y, \text{pitch} = -\text{rad}(\text{rot.pitch}), \text{heading} = -\text{rad}(\text{rot.yaw}), \text{roll} = \text{rad}(\text{rot.roll})$）、`VehiclePose6DOF` 提取、`friction_zones` 最小 `mu_scale` 命中、$\text{road\_grade} = \tan(\text{pitch})$ 与 `capabilities_6dof` 契约声明。

## 关键设计约束（跨模块铁律）

- **速度与轨迹唯一权威 = planning**；control 纯跟随不质疑轨迹；safety 只限幅不改任务性指令。
- **行进方向唯一事实源 = flowsim `road/ref_path.reverse`**，navigation/behavior/control 全部消费它。
- control→safety 带外信号唯一通道 = `raw.mode` 字符串（如 `"+MANEUVER"` 后缀）。
- 算法升级必须**先 Python 仿真验证再移植 C++**（`tools/control_sim.py` 6 场景 + `tools/speed_planner_sim.py` + `tools/tire_dynamics_sim.py` + `tools/tracker_prediction_sim.py` + `tools/open_space_parking_sim.py`），
  移植后跑 `ci/evaluators/demo_evaluator.py` 验证行为一致。
- ST 图速度剖面的约束、实现边界和验证入口见
  [速度规划说明](PLANNING_SPEED_UPGRADE_DESIGN.md)。

## 性能实测参考

| 阶段 | 频率 | 备注 |
|------|------|------|
| flowsim 物理 | 60Hz | 场景 JSON 加载 actor，含 6-DOF 悬架与轮胎动力学 |
| 感知 DBSCAN / BEV | 10~30Hz | 点云聚类与 CenterPoint-Lite 纯 C 推理 |
| 跟踪预测 | 20Hz | 64 槽匈牙利卡尔曼跟踪 + Frenet 多模态预测 |
| 融合 EKF | 20Hz | |
| 规划（Frenet+ST 图 / Hybrid A*） | 20Hz | DP 表静态 142KB，泊车 Hybrid A* 毫秒级求解 |
| 控制（MPC+PID / ManeuverTracker） | 20Hz | |
| 消息总线 | — | 进程内 <100µs |

## 第三方算法库集成

想把 OpenCV/YOLO、Eigen、OSQP 等外部算法库接成插件节点？集成模式
（dlopen 插件 API、topic 订阅发布、构建方法）见
[book/02_plugin_system.md](book/02_plugin_system.md) 与
[ALGORITHM_INTEGRATION.md](ALGORITHM_INTEGRATION.md)。本仓库不内置这些依赖。
