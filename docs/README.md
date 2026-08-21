# KunAutoDrive 文档导航

本页是 `docs/` 的**导航入口**。根目录 [README](../README.md) 只保留项目概览、
快速运行和常用入口；模块事实、契约和操作说明以本页指定的文档为准。

## 阅读方式

`docs/` 现在以「一本书」的形式组织：

| 入口 | 用途 |
|------|------|
| [**KunAutoDrive 技术全书**（BOOK.md）](BOOK.md) | **通读用**：总序 + 九卷分卷目录 + 每卷导读 + 五条阅读路线 |
| [**术语表**（GLOSSARY.md）](GLOSSARY.md) | 查术语：全书反复出现的名词一句话定义 |
| [**全书索引**（INDEX.md）](INDEX.md) | 检索用：按「任务 / 主题 / 文件」三种方式找文档 |
| 本页（README.md） | 快速导航：按模块查阅权威文档 |

新手从 [BOOK.md](BOOK.md) 的「新人线」读起；老手直接用 [INDEX.md](INDEX.md) 检索。

## 按模块查阅

| 模块 / 任务 | 权威文档 | 辅助资料 |
|---|---|---|
| 构建、运行与项目定位 | [根目录 README](../README.md) | [仿真指南](SIMULATION_GUIDE.md) |
| 核心运行时、插件和源码入口 | [代码索引](CODE_WIKI.md) | [API 速查](API_QUICK_REFERENCE.md)、[教程 01–11](book/) |
| 默认 ADAS 节点、topic 与配置 | [Pipeline 架构](PIPELINE_ARCHITECTURE.md) | [代码索引](CODE_WIKI.md) |
| 当前算法与职责边界 | [算法栈](ALGORITHM_STACK.md) | [算法验证](ALGORITHM_VERIFY_PATTERN.md)、[算法集成](ALGORITHM_INTEGRATION.md) |
| 规划速度剖面（ST 图 + DP） | [速度规划说明](PLANNING_SPEED_UPGRADE_DESIGN.md) | [算法栈](ALGORITHM_STACK.md) |
| 控制与真车标定 | [标定指南](CALIBRATION_GUIDE.md) | [算法验证](ALGORITHM_VERIFY_PATTERN.md) |
| FlowSim、场景与场景回归 | [仿真指南](SIMULATION_GUIDE.md) | [场景设计教程](book/16_flowsim_scenario_design.md) |
| FlowSim 几何 / 运动 invariant | [Sim Digest](SIM_DIGEST.md) | [仿真指南](SIMULATION_GUIDE.md) |
| Bag 通用录制与回放 | [Bag 教程](book/05_bag_recording.md) | [API 速查](API_QUICK_REFERENCE.md) |
| flowrec 配置化留存节点 | [flowrec](FLOWREC.md) | [监控架构](MONITORING_ARCHITECTURE.md) |
| 监控、flowmond 与 HTTP/SSE | [监控架构](MONITORING_ARCHITECTURE.md) | [FlowBoard API 契约](FLOWBOARD_CONTRACT.md) |
| FlowBoard 场景帧与 `road_network` schema | [FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md) | [可视化架构](VISUALIZATION_ARCHITECTURE.md) |
| FlowBoard 运行时架构 | [可视化架构](VISUALIZATION_ARCHITECTURE.md) | [vis View 接入规范](VIS_MODULE_GUIDE.md) |
| PEM 与车端数据采集 | [数据闭环](DATA_CLOSED_LOOP.md) | [硬件部署](HARDWARE_DEPLOYMENT.md) |
| 训练、影子推理与 OTA | [学习闭环](LEARNING_LOOP.md) | [学习教程](book/13_e2e_learning_loop.md) |
| 真车 profile、打包与升级 | [硬件部署](HARDWARE_DEPLOYMENT.md) | [RC 小车清单](RC_CAR_HARDWARE_CHECKLIST.md) |
| 3D 仪表盘故障 | [3D 仪表盘排查](TROUBLESHOOTING_3D_DASHBOARD.md) | [监控架构](MONITORING_ARCHITECTURE.md) |

## 教程

`book/` 是循序渐进的学习资料，不重复定义模块契约：

| 范围 | 教程 |
|---|---|
| C / 插件 / 消息总线 / IPC / Bag / 时钟 / 序列化 / 状态机 / 发现 | [01–09](book/) |
| 融合、协程、评估器 | [10–12](book/) |
| 学习闭环、航位推算、SocketCAN、场景 | [13–16](book/)；vis View 见 [接入规范](VIS_MODULE_GUIDE.md) |

## 维护约定

- 修改运行时行为，更新对应模块的权威文档；教程只补充原理和示例。
- 修改 API 或 JSON 字段，更新相应契约文档；其中 `road_network` 只在
  [FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md) 定义。
- `FLOWREC.md` 是 flowrec 的独立权威文档；本导航只建立入口，不复制其内容。
- **新增 / 删除 / 重命名文档**时，同步维护 [BOOK.md](BOOK.md)（卷目录登记一行）与
  [INDEX.md](INDEX.md)（三类索引里的引用），避免「书」与磁盘脱节。

| 文档 | 说明 |
|------|------|
| [FLOWBOARD_CONTRACT.md](FLOWBOARD_CONTRACT.md) | FlowBoard 数据契约 |
| [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) | FlowBoard 3D Scene 数据契约 |
| [SCHEMA_road_network.md](SCHEMA_road_network.md) | road_network JSON Schema |
| [SIM_DIGEST.md](SIM_DIGEST.md) | 仿真 digest / invariant 与调试可视化 |

## 硬件部署

| 文档 | 说明 |
|------|------|
| [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) | 真车硬件部署指南 |
| [RC_CAR_HARDWARE_CHECKLIST.md](RC_CAR_HARDWARE_CHECKLIST.md) | RC 小车硬件连接操作清单 |

## 场景与演示

| 文档 | 说明 |
|------|------|
| [DRIVING_SCHOOL_PLAN.md](DRIVING_SCHOOL_PLAN.md) | 驾校计划（科目一至科目四场景） |

## 故障排查

| 文档 | 说明 |
|------|------|
| [TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md) | 3D 仪表盘"加载失败"排查与修复 |

> 更多运行期故障速查见 [CLAUDE.md](../CLAUDE.md) 的「常见故障模式」表。

## KunAutoDrive 技术专著与实战教程（book/）

《KunAutoDrive：从零构建高性能自动驾驶系统与仿真引擎》（中间件内核 FlowEngine 实战专著），按系统分卷组织（总目录见 [BOOK.md](BOOK.md)）：

### 第一卷：微内核与系统编程底座 (Core & OS Primitives)
| 章节 | 专章教程 | 核心主题与深度解析 |
|---|---|---|
| 01 | [第 01 章：C 语言面向对象与微内核架构](book/01_oop_in_c.md) | C11 标准首成员内存保证、vtable 虚表分发、生命周期链与内存安全 |
| 02 | [第 02 章：dlopen 插件化系统与微内核解耦](book/02_plugin_system.md) | ABI 门禁校验、RTLD_LOCAL 符号隔离、依赖注入与生命周期状态机 |
| 03 | [第 03 章：高性能进程内消息总线](book/03_message_bus.md) | Pub/Sub 拓扑、64KB 动态消息帧、Free-List 零拷贝内存池、QoS 丢弃策略 |
| 04 | [第 04 章：跨进程共享内存通信](book/04_ipc_channel.md) | POSIX SHM 环形队列、Robust Mutex 崩溃自愈、大 JSON 分块传输协议 |
| 05 | [第 05 章：数据录制与回放](book/05_bag_recording.md) | Bag v2 格式、标准 MCAP 规范与时序索引无损回放 |
| 06 | [第 06 章：统一时钟服务](book/06_clock_service.md) | 真实时钟 vs 仿真步进时钟、统一时间戳 uint64 μs 语义 |
| 07 | [第 07 章：类型安全序列化层](book/07_serializer.md) | IDL 代码生成器、FNV-1a 哈希校验与二进制内存对齐 |

### 第二卷：执行流与高级调度 (Scheduler & Coroutines)
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 08 | [第 08 章：反射式状态机](book/08_state_machine.md) | 事件驱动状态转移矩阵与拓扑反射 |
| 09 | [第 09 章：去中心化服务发现](book/09_discovery.md) | UDP 广播、心跳自愈与 SysMonitor 节点内省 |
| 10 | [第 10 章：C++20 协程框架 FlowCoro](book/10_coroutine.md) | Task、Awaitable、Select、超时与优雅取消 |
| 11 | [第 11 章：DAG 任务流与混合调度器](book/11_scheduler.md) | Classic FIFO + Choreo DAG + CPU 亲和性 |

### 第三卷：ADAS 算法栈从理论到实现 (Algorithms & Pipeline)
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 12 | [第 12 章：点云聚类与卡尔曼追踪](book/12_lidar_tracking.md) | DBSCAN 点云聚类与 Kalman 目标跟踪 |
| 13 | [第 13 章：多传感器融合与定位](book/13_sensor_fusion.md) | EKF 状态估计、GPS/IMU 解算与 EKF-SLAM |
| 14 | [第 14 章：离散决策状态机](book/14_behavior_decision.md) | 8 状态 Behavior FSM（跟车、变道、让行、掉头）与 NOA 导航主动变道 |
| 15 | [第 15 章：轨迹与速度规划](book/15_trajectory_planning.md) | Frenet 最优轨迹、ST 图 + DP 动态规划速度剖面 |
| 16 | [第 16 章：跟踪控制与特殊机动](book/16_tracking_control.md) | Stanley 横向 + LTV MPC + ManeuverTracker 掉头泊车 |
| 17 | [第 17 章：FlowCoro 协程安全包络](book/17_safety_envelope.md) | TTC 碰撞闸门、横向干涉豁免与行人防护 |

### 第四卷：仿真验证、学习闭环与运维 (Sim, Learning & Ops)
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 18 | [第 18 章：FlowSim 场景设计](book/18_flowsim_scenario_design.md) | 多 edge 路网拓扑、NPC 交互与 OpenDRIVE 桥接 |
| 19 | [第 19 章：端到端学习闭环](book/19_e2e_learning_loop.md) | 数据采集、v3 59 维特征、tiny-MLP/PyTorch 训练、DAgger 与 Promote 门禁 |
| 20 | [第 20 章：flowmond 监控与 3D 可视化](book/20_flowmond_3d_vis.md) | Three.js 前端、航位推算 Dead Reckoning 与 View 模块规范 |
| 21 | [第 21 章：黑盒回归评估体系](book/21_demo_evaluator.md) | Demo Evaluator、分层校验阶梯 L0/L1/L2、参数敏感度扫描 |

### 第五卷：真车部署与硬件落地 (Hardware Deployment)
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 附录 A | [SocketCAN 与 PWM 执行器落地指南](book/22_socketcan_actuator.md) | RC 智能小车与真车底盘软硬件连接、SocketCAN 与 PCA9685 PWM 驱动 |
