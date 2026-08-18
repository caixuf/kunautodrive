# KunAutoDrive 全书索引

> 本书（[BOOK.md](BOOK.md)）的附录 C。按「任务」和「主题」两种方式检索，
> 每条指向权威文档。通读用[目录](BOOK.md)，检索用本页。

## 一、按任务：我想做 X，看哪？

### 运行 / 构建
| 我想… | 去 |
|------|-----|
| 一键跑通 demo | [根 README](../README.md) → `bash scripts/demo.sh` |
| 不装依赖跑仿真 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md)（三层体系 + 场景矩阵） |
| 构建主框架 / 节点插件 | [CODE_WIKI.md](CODE_WIKI.md) §8.2 |
| 跨平台（macOS/Linux）编译差异 | [PLATFORM_ABSTRACTION.md](PLATFORM_ABSTRACTION.md) |

### 改代码 / 加功能
| 我想… | 去 |
|------|-----|
| 加一个新节点 | [tutorials/02_plugin_system.md](tutorials/02_plugin_system.md) |
| 加一个新 topic / 消息类型 | [tutorials/03_message_bus.md](tutorials/03_message_bus.md) + [tutorials/07_serializer.md](tutorials/07_serializer.md) |
| 加一个可调参数 | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md)（ParamRegistry） |
| 接第三方算法库（YOLO/Eigen…） | [ALGORITHM_INTEGRATION.md](ALGORITHM_INTEGRATION.md) |
| 让节点跨进程通信 | [tutorials/04_ipc_channel.md](tutorials/04_ipc_channel.md) |
| 写一个状态机 | [tutorials/08_state_machine.md](tutorials/08_state_machine.md) |
| 新增一个仿真场景 | [tutorials/16_flowsim_scenario_design.md](tutorials/16_flowsim_scenario_design.md) |
| 新增一个地图 | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) + [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| 新增一个 3D View | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) |
| 调控制器参数（仿真/真车） | [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) |
| 训练并部署一个驾驶模型 | [LEARNING_LOOP.md](LEARNING_LOOP.md) + [tutorials/13_e2e_learning_loop.md](tutorials/13_e2e_learning_loop.md) |

### 调试 / 验证
| 我想… | 去 |
|------|-----|
| 改完代码快速检查管道 | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md)（`pipeline_check.py`） |
| 整条链 45s 行为回归 | [tutorials/12_demo_evaluator.md](tutorials/12_demo_evaluator.md) |
| 场景矩阵批量回归 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md)（scenario_regression） |
| 检查地图连通性 | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md)（check_map_connectivity） |
| 前端 3D 门禁 | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md)（`npm run vis:check:all`） |
| 排查 3D 仪表盘黑屏/挂死 | [TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md) |
| 事故逐层追溯 | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md)（trace_incident） |
| 复现一次采样 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md)（scenarioctl replay） |

### 部署 / 硬件
| 我想… | 去 |
|------|-----|
| 部署到真车 | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) |
| 组装 RC 小车 | [RC_CAR_HARDWARE_CHECKLIST.md](RC_CAR_HARDWARE_CHECKLIST.md) |
| 接 CAN 执行器 | [tutorials/15_socketcan_actuator.md](tutorials/15_socketcan_actuator.md) |
| 接真实 SLAM | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md)（FAST-LIO2） |
| 看硬件调试命令 | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) §调试 |

## 二、按主题：概念在哪讲

| 主题 | 教程（原理/入门） | 参考（契约/细节） |
|------|------------------|-------------------|
| C 面向对象 | [tutorials/01_oop_in_c.md](tutorials/01_oop_in_c.md) | [CODE_WIKI.md](CODE_WIKI.md) |
| 消息总线 | [tutorials/03_message_bus.md](tutorials/03_message_bus.md) | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) §Message Bus |
| 传输/IPC | [tutorials/04_ipc_channel.md](tutorials/04_ipc_channel.md) | [MONITORING_ARCHITECTURE.md](MONITORING_ARCHITECTURE.md) |
| 时钟/时间 | [tutorials/06_clock_service.md](tutorials/06_clock_service.md) | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |
| 序列化 | [tutorials/07_serializer.md](tutorials/07_serializer.md) | [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) |
| 状态机 | [tutorials/08_state_machine.md](tutorials/08_state_machine.md) | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（8 状态 FSM） |
| 协程 | [tutorials/11_coroutine.md](tutorials/11_coroutine.md) | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| 融合/定位 | [tutorials/10_fusion.md](tutorials/10_fusion.md) | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（EKF） |
| 感知/跟踪 | — | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（DBSCAN/卡尔曼） |
| 行为决策 | — | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（FSM） |
| 规划（轨迹+速度） | [PLANNING_SPEED_UPGRADE_DESIGN.md](PLANNING_SPEED_UPGRADE_DESIGN.md) | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) |
| 控制 | [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（Stanley/MPC/PID） |
| 安全 | — | [ALGORITHM_STACK.md](ALGORITHM_STACK.md)（TTC/MRM） |
| 仿真物理 | [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) | [SIM_DIGEST.md](SIM_DIGEST.md) |
| 地图生成 | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) |
| 可视化/3D | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) | [VIS_3D_RENDERING.md](VIS_3D_RENDERING.md) |
| 学习闭环 | [tutorials/13_e2e_learning_loop.md](tutorials/13_e2e_learning_loop.md) | [LEARNING_LOOP.md](LEARNING_LOOP.md) |
| 数据采集 | [FLOWREC.md](FLOWREC.md) | [DATA_CLOSED_LOOP.md](DATA_CLOSED_LOOP.md) |
| 验证/门禁 | [tutorials/12_demo_evaluator.md](tutorials/12_demo_evaluator.md) | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) |

## 三、按文件：每个文档一句话定位

> 完整目录与导读见 [BOOK.md](BOOK.md)。这里按文件名速查定位。

| 文档 | 一句话定位 |
|------|-----------|
| [ALGORITHM_INTEGRATION.md](ALGORITHM_INTEGRATION.md) | 第三方算法库如何接成插件节点（步骤 + 推荐/不推荐做法） |
| [ALGORITHM_STACK.md](ALGORITHM_STACK.md) | 当前代码里真实跑着的算法总览 + 跨模块铁律 |
| [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) | 分层验证阶梯（L0~L3）+ Python 仿真先行流程 |
| [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) | 中间件 API 速查（工具书） |
| [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) | 控制参数标定（仿真/真车双路径） |
| [CODE_WIKI.md](CODE_WIKI.md) | 代码地图：15 节点、控制子系统详解、构建运行 |
| [DATA_CLOSED_LOOP.md](DATA_CLOSED_LOOP.md) | PEM 与车端数据采集链路 |
| [FLOWBOARD_CONTRACT.md](FLOWBOARD_CONTRACT.md) | FlowBoard /api/topology 归一化数据契约 |
| [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) | road_network schema + 场景帧字段（唯一权威） |
| [FLOWREC.md](FLOWREC.md) | flowrec 配置化留存节点 |
| [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) | 车辆物理模型（运动学/动力学）+ 碰撞/护栏/重力 |
| [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) | 真车部署 + 硬件调试 + FAST-LIO2 |
| [LEARNING_LOOP.md](LEARNING_LOOP.md) | 车端学习闭环总纲（四阶段 + 契约 + OTA） |
| [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) | 地图契约 + 工具链 + A* 路由 + 经验坑 |
| [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) | 地图生成 DSL 单一枢纽架构 + 五条铁律 |
| [MONITORING_ARCHITECTURE.md](MONITORING_ARCHITECTURE.md) | flowmond 监控 + IPC/文件桥接 + HTTP/SSE |
| [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) | 15 节点 pipeline 数据流 + 关键参数 |
| [PLANNING_SPEED_UPGRADE_DESIGN.md](PLANNING_SPEED_UPGRADE_DESIGN.md) | ST 图 + DP 速度规划设计 |
| [PLATFORM_ABSTRACTION.md](PLATFORM_ABSTRACTION.md) | macOS/Linux 平台差异收口层 |
| [RC_CAR_HARDWARE_CHECKLIST.md](RC_CAR_HARDWARE_CHECKLIST.md) | RC 小车组装/验收清单 |
| [ROAD_MARKINGS_MODULE.md](ROAD_MARKINGS_MODULE.md) | 道路标线（虚线/实线/双黄）生成与渲染 |
| [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) | 仿真三层体系 + 场景库 + 场景矩阵回归 |
| [SIM_DIGEST.md](SIM_DIGEST.md) | 仿真 digest + invariant 断言 |
| [TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md) | 3D 仪表盘故障排查 |
| [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) | 前端 3D 分层架构 + 坐标约定 + 帧契约 |
| [VIS_3D_LANE_PIPELINE.md](VIS_3D_LANE_PIPELINE.md) | 车道线/路面 3D 渲染管线 |
| [VIS_3D_RENDERING.md](VIS_3D_RENDERING.md) | 渲染经验沉淀（性能/贴地/相机） |
| [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) | 新增 View 的规范 + 前端门禁 |
| [map_engine_boundary.md](map_engine_boundary.md) | esmini vs 自研 MapEngine 职责边界 |
| tutorials/01–16 | 16 篇循序渐进教程（见 [BOOK.md](BOOK.md) 卷二/卷四/卷七/卷八/卷九） |
