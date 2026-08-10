# KunAutoDrive 文档导航

本页是 `docs/` 的**唯一导航入口**。根目录 [README](../README.md) 只保留项目概览、
快速运行和常用入口；模块事实、契约和操作说明以本页指定的文档为准。

## 按模块查阅

| 模块 / 任务 | 权威文档 | 辅助资料 |
|---|---|---|
| 构建、运行与项目定位 | [根目录 README](../README.md) | [仿真指南](SIMULATION_GUIDE.md) |
| 核心运行时、插件和源码入口 | [代码索引](CODE_WIKI.md) | [API 速查](API_QUICK_REFERENCE.md)、[教程 01–11](tutorials/) |
| 默认 ADAS 节点、topic 与配置 | [Pipeline 架构](PIPELINE_ARCHITECTURE.md) | [代码索引](CODE_WIKI.md) |
| 当前算法与职责边界 | [算法栈](ALGORITHM_STACK.md) | [算法验证](ALGORITHM_VERIFY_PATTERN.md)、[算法集成](ALGORITHM_INTEGRATION.md) |
| 规划速度剖面（ST 图 + DP） | [速度规划说明](PLANNING_SPEED_UPGRADE_DESIGN.md) | [算法栈](ALGORITHM_STACK.md) |
| 控制与真车标定 | [标定指南](CALIBRATION_GUIDE.md) | [算法验证](ALGORITHM_VERIFY_PATTERN.md) |
| FlowSim、场景与场景回归 | [仿真指南](SIMULATION_GUIDE.md) | [场景设计教程](tutorials/16_flowsim_scenario_design.md) |
| FlowSim 几何 / 运动 invariant | [Sim Digest](SIM_DIGEST.md) | [仿真指南](SIMULATION_GUIDE.md) |
| Bag 通用录制与回放 | [Bag 教程](tutorials/05_bag_recording.md) | [API 速查](API_QUICK_REFERENCE.md) |
| flowrec 配置化留存节点 | [flowrec](FLOWREC.md) | [监控架构](MONITORING_ARCHITECTURE.md) |
| 监控、flowmond 与 HTTP/SSE | [监控架构](MONITORING_ARCHITECTURE.md) | [FlowBoard API 契约](FLOWBOARD_CONTRACT.md) |
| FlowBoard 场景帧与 `road_network` schema | [FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md) | [可视化架构](VISUALIZATION_ARCHITECTURE.md) |
| FlowBoard 运行时架构 | [可视化架构](VISUALIZATION_ARCHITECTURE.md) | [vis View 接入规范](VIS_MODULE_GUIDE.md) |
| PEM 与车端数据采集 | [数据闭环](DATA_CLOSED_LOOP.md) | [硬件部署](HARDWARE_DEPLOYMENT.md) |
| 训练、影子推理与 OTA | [学习闭环](LEARNING_LOOP.md) | [学习教程](tutorials/13_e2e_learning_loop.md) |
| 真车 profile、打包与升级 | [硬件部署](HARDWARE_DEPLOYMENT.md) | [RC 小车清单](RC_CAR_HARDWARE_CHECKLIST.md) |
| 3D 仪表盘故障 | [3D 仪表盘排查](TROUBLESHOOTING_3D_DASHBOARD.md) | [监控架构](MONITORING_ARCHITECTURE.md) |

## 教程

`tutorials/` 是循序渐进的学习资料，不重复定义模块契约：

| 范围 | 教程 |
|---|---|
| C / 插件 / 消息总线 / IPC / Bag / 时钟 / 序列化 / 状态机 / 发现 | [01–09](tutorials/) |
| 融合、协程、评估器 | [10–12](tutorials/) |
| 学习闭环、航位推算、SocketCAN、场景 | [13–16](tutorials/)；vis View 见 [接入规范](VIS_MODULE_GUIDE.md) |

## 维护约定

- 修改运行时行为，更新对应模块的权威文档；教程只补充原理和示例。
- 修改 API 或 JSON 字段，更新相应契约文档；其中 `road_network` 只在
  [FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md) 定义。
- `FLOWREC.md` 是 flowrec 的独立权威文档；本导航只建立入口，不复制其内容。
