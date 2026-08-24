# KunAutoDrive 术语表

> 本书（[BOOK.md](BOOK.md)）的附录 B。收录全书反复出现的术语，按领域分组。
> 每个词条给「一句话定义」+「在哪儿深入了解」。定义均以当前代码与文档为准，
> 不收录论文概念。

## 一、系统与运行时

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **KunAutoDrive** | 轻量级自动驾驶中间件：消息总线 + 调度器 + 传输层 + ADAS 参考实现 | [BOOK](BOOK.md) 总序 |
| **pipeline** | 由 `flow_launcher` 按 `config/pipeline.json` 加载的一组插件节点组成的完整链路 | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) |
| **节点（Node / NodePlugin）** | 一个编译为 `.so`、实现插件接口、订阅/发布 topic 的处理单元（如 flowsim、control） | [book/02_plugin_system.md](book/02_plugin_system.md) |
| **topic** | 消息总线上的命名通道，节点按名订阅/发布（如 `vehicle/state`、`control/cmd`） | [book/03_message_bus.md](book/03_message_bus.md) |
| **QoS** | 订阅匹配 + 消息策略（depth、drop_oldest 等） | [book/03_message_bus.md](book/03_message_bus.md) |
| **flow_launcher** | 配置驱动启动器：读 pipeline.json，dlopen 加载插件节点 | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) |
| **flowctl** | CLI 工具：list / inspect / dashboard / param / bag 等子命令 | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |
| **flowmond** | 监控守护进程：HTTP 仪表盘（8800）+ IPC/文件桥接 + 自动重连 | [MONITORING_ARCHITECTURE.md](MONITORING_ARCHITECTURE.md) |
| **FlowRegistry** | 统一元信息注册中心（Task/Topic/Type/Plugin/Schema） | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |
| **ParamRegistry** | 参数系统（int/float/bool/string、范围校验、hot-reload） | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |
| **clock_now_us()** | 唯一合法时间源（CLOCK_MONOTONIC，可注入），替代裸 `clock_gettime` | [book/06_clock_service.md](book/06_clock_service.md) |
| **node_pump()** | 节点主循环的标准写法（协程 executor + 休眠泵），禁止裸忙等 | [book/11_coroutine.md](book/11_coroutine.md) |
| **FlowCoro** | 免锁协程运行时，安全层/行为层等节点的调度骨架 | [book/11_coroutine.md](book/11_coroutine.md) |
| **Bag** | 通用录制/回放格式，`flowctl bag play` 回放给算法 | [book/05_bag_recording.md](book/05_bag_recording.md) |
| **Serializer** | 消息编解码层（跨版本/跨语言） | [book/07_serializer.md](book/07_serializer.md) |
| **Discovery** | 节点/服务互相发现的机制 | [book/09_discovery.md](book/09_discovery.md) |

## 二、算法链（ADAS）

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **FlowSim** | 仿真节点：被控对象（物理积分、发布真值），不做驾驶决策 | [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) |
| **step_bicycle** | 运动学自行车模型积分（含 half_wb·yaw_rate 切向项） | [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) |
| **sensor_model** | 传感器模型：FOV/遮挡/噪声（LiDAR/GPS/Camera） | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) |
| **perception** | 感知：DBSCAN 点云聚类 + RANSAC 地面去除 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **object_tracker** | 卡尔曼多目标跟踪（20Hz） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **fusion / EKF** | GPS+IMU 定位融合（5 维 EKF：x, y, v, heading, yaw_rate） | [book/10_fusion.md](book/10_fusion.md) |
| **behavior_planner** | 8 状态 FSM 行为决策（跟车/变道/停车/让行/掉头），只决定「做什么」 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **navigation** | 路由步骤 + 行进方向（消费 flowsim `ref_path.reverse`） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **planning** | 速度与轨迹的唯一权威（Frenet 轨迹 + ST 图 DP 速度 + 掉头 N 把方向） | [PLANNING_SPEED_UPGRADE_DESIGN.md](PLANNING_SPEED_UPGRADE_DESIGN.md) |
| **ST 图 + DP** | 速度规划：红灯墙/动态障碍/曲率限速 → 速度剖面（90×101 DP 表） | [PLANNING_SPEED_UPGRADE_DESIGN.md](PLANNING_SPEED_UPGRADE_DESIGN.md) |
| **trajectory** | planning 输出的轨迹（points[].v 含速度），control 只跟随不质疑 | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) |
| **control** | 纯轨迹跟随：从 trajectory 推 gear/steer/throttle | [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) |
| **Stanley** | 横向控制默认算法（heading 阻尼 + kappa 前馈） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **LTV MPC** | 可选横向控制器（`use_ltv_mpc` 参数），机动模式跳过 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **PID（纵向）** | 纵向速度/油门控制（anti-windup：error 翻负清正积分） | [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) |
| **ACC** | 自适应巡航：按同车道前车间距动态限速 | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) |
| **ManeuverTracker** | 机动跟随器（掉头/泊车）：弧长推进 + D/R 挡位 + 倒挡横向反号 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **safety_control** | 纯安全闸门：TTC 限幅 + 紧急制动 + 机动窗口豁免，不理解任务意图 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **TTC** | 碰撞时间（Time To Collision），安全层核心指标 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **MRM** | 最小风险机动（安全层触发的紧急避险） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **raw.mode** | control→safety 的带外信号唯一通道（字符串，如 `+MANEUVER` 后缀） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **ref_path.reverse** | 行进方向唯一事实源（flowsim 权威发布） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| **U_TURN / 掉头** | 行为状态之一：N 把方向多段掉头（≤5 把，512 点细生成 + 下采样 64） | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |

## 三、仿真与验证

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **场景（scenario）** | `scenarios/*.json` 定义的仿真场景（actor + ego 配置） | [book/16_flowsim_scenario_design.md](book/16_flowsim_scenario_design.md) |
| **场景矩阵（suite）** | `scenarios/suite.json` 组织的批量回归场景集 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) |
| **demo_evaluator** | 黑盒回归评估器：采样拓扑 JSON 自动评分（碰撞/偏航/停滞/频率） | [book/12_demo_evaluator.md](book/12_demo_evaluator.md) |
| **scenario_regression** | 场景矩阵批量回归 + 基线对比（退化即 FAIL） | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) |
| **pipeline_check** | L0 秒级离线管道完整性检查（9 类 32 项指标） | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) |
| **test_param_regression** | 参数回归 A/B：保存 baseline，改参后检测退化 | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) |
| **Digest / Invariant** | 仿真静态/动态 digest 编码空间关系，invariant 断言提交前抓几何错 | [SIM_DIGEST.md](SIM_DIGEST.md) |
| **NPC AI（IDM）** | 交通参与者智能：IDM 纵向跟车 + 中央 Route Frenet 车道跟随 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) |
| **MOBIL** | 变道决策算法（当前 `#if 0` 禁用，代码保留参考） | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) |
| **ADE / FDE / MPI** | 开环预测指标（平均/最终位移误差、平均路径偏差），仅 computed 才算数 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) |

## 四、地图

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **.kmap** | 声明式地图 DSL（`map_compiler.py` 编译为 map.json） | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) |
| **map.json** | 静态道路事实源（road/lane/successors），运行时契约 | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| **routes.json** | 路线定义（id、kind、road_chain、lane_direction） | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| **map_compiler.py** | kmap → map.json 的唯一编译器（校验内置） | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| **osm2kmap.py** | OSM 路网 → .kmap 的适配器（含 Overpass 桥隧高程） | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) |
| **netconvert / SUMO** | 外部几何引擎：OSM → SUMO net.xml | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) |
| **XODR / esmini** | OpenDRIVE 格式与场景库（RoadManager 消费路网） | [map_engine_boundary.md](map_engine_boundary.md) |
| **A\*** | 车道级 successors 图上的路径搜索（astar_route.py） | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| **road_network** | 传给可视化的路网 schema（含 buildings 字段渲染真实建筑） | [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) |

## 五、可视化

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **FlowBoard** | 前端仪表盘（3D + 2D + 图表 + D3 拓扑，ES modules） | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) |
| **vis/** | 前端 3D 渲染分层（core/ director/ view/ math/ store/） | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) |
| **SceneDirector** | 场景导演：把拓扑帧转成 3D 场景，驱动各 View tick | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) |
| **View** | 渲染单元（VehicleView/RoadView/TrajectoryView…），实现 build/update | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) |
| **Coord.js** | 坐标/朝向/高度转换的唯一合法纯函数库 | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) |
| **worldToThree / placeOnRoad** | Coord 提供的坐标映射纯函数（禁手写裸 -y/atan2） | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) |
| **帧契约** | `frame: THREE | up: +Y | 单位 m | ENU→THREE [x, z, -y]` | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) |
| **vis:check:all** | 前端门禁（模块加载 + ESLint + tick 冒烟 + invariant + grep） | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) |

## 六、学习闭环与硬件

| 术语 | 一句话定义 | 深入 |
|------|-----------|------|
| **Learning Loop** | 车端学习闭环：采集 → 训练 → 影子推理 → 晋级 → OTA | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **data_recorder** | 训练样本采集节点（Stage 0） | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **inference（影子模式）** | tiny-MLP 影子推理（Stage 2），只对比监控、不接入控制 | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **learner** | 车端增量微调节点（Stage 3） | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **model_ota** | 模型 OTA + 版本管理（Stage 4，热重载 + A-B 对比） | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **promote gate** | 影子评估通过才晋级的门禁 | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| **flowrec** | 配置化留存节点（数据采集） | [FLOWREC.md](FLOWREC.md) |
| **waypoint_follower** | 航点跟随（RC 小车 L2，Pure Pursuit） | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) |
| **SocketCAN / CAN** | 车端执行器总线（油门/转向） | [book/15_socketcan_actuator.md](book/15_socketcan_actuator.md) |
| **FAST-LIO2** | 可选真实 SLAM 后端（slam_node 占位可接） | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) |
| **dead reckoning** | 航位推算：GPS 丢帧时的位姿外推 | [book/14_dead_reckoning.md](book/14_dead_reckoning.md) |
