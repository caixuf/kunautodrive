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
| LTV MPC 横向控制器 | [LTV MPC 设计](LTV_MPC_DESIGN.md) | [算法栈](ALGORITHM_STACK.md)（Stanley/MPC/PID） |
| FlowSim、场景与场景回归 | [仿真指南](SIMULATION_GUIDE.md) | [场景设计教程](book/20_flowsim_scenario_design.md) |
| FlowSim 几何 / 运动 invariant | [Sim Digest](SIM_DIGEST.md) | [仿真指南](SIMULATION_GUIDE.md) |
| Bag 通用录制与回放 | [Bag 教程](book/12_bag_recording.md) | [API 速查](API_QUICK_REFERENCE.md) |
| flowrec 配置化留存节点 | [flowrec](FLOWREC.md) | [监控架构](MONITORING_ARCHITECTURE.md) |
| 监控、flowmond 与 HTTP/SSE | [监控架构](MONITORING_ARCHITECTURE.md) | [FlowBoard API 契约](FLOWBOARD_CONTRACT.md) |
| FlowBoard 场景帧与 `road_network` schema | [FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md) | [可视化架构](VISUALIZATION_ARCHITECTURE.md) |
| FlowBoard 运行时架构 | [可视化架构](VISUALIZATION_ARCHITECTURE.md) | [vis View 接入规范](VIS_MODULE_GUIDE.md) |
| PEM 与车端数据采集 | [数据闭环](DATA_CLOSED_LOOP.md) | [硬件部署](HARDWARE_DEPLOYMENT.md) |
| 训练、影子推理与 OTA | [学习闭环](LEARNING_LOOP.md) | [学习教程](book/24_e2e_learning_loop.md) |
| 真车 profile、打包与升级 | [硬件部署](HARDWARE_DEPLOYMENT.md) | [RC 小车清单](RC_CAR_HARDWARE_CHECKLIST.md) |
| 3D 仪表盘故障 | [3D 仪表盘排查](TROUBLESHOOTING_3D_DASHBOARD.md) | [监控架构](MONITORING_ARCHITECTURE.md) |
| 运行期行为异常（控制/规划/感知/仿真） | [故障模式表](FAILURE_MODES.md) | [算法排查范式](ALGORITHM_VERIFY_PATTERN.md) |

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
| [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) | road_network 顶层 Schema 与路由契约 |
| [SIM_DIGEST.md](SIM_DIGEST.md) | 仿真 digest / invariant 与调试可视化 |

## 硬件部署

| 文档 | 说明 |
|------|------|
| [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) | 真车硬件部署指南 |
| [RC_CAR_HARDWARE_CHECKLIST.md](RC_CAR_HARDWARE_CHECKLIST.md) | RC 小车硬件连接操作清单 |

## 场景与演示

| 文档 | 说明 |
|------|------|
| [scenarios/ 驾校考试场景](../scenarios/) | 驾校考试场景 JSON（路考/安全/交规/侧方停车等科目） |

## 故障排查

| 文档 | 说明 |
|------|------|
| [FAILURE_MODES.md](FAILURE_MODES.md) | 运行期故障模式表（43 行「现象 → 根因 → 位置」，按 6 类分节，仓库权威副本） |
| [TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md) | 3D 仪表盘"加载失败"排查与修复 |

> 行为异常（转向灯反 / 该停不停 / 该走不走 / 刹停到 0 / 改代码现象不变）先按
> [FAILURE_MODES.md](FAILURE_MODES.md) 的现象列对号入座，再按 [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md)
> 的分层验证阶梯定位。

## KunAutoDrive 技术专著与实战教程（book/）

《KunAutoDrive：从零构建高性能自动驾驶系统与仿真引擎》（中间件内核 KunAutoDrive 实战专著），按系统分卷组织（总目录见 [BOOK.md](BOOK.md)）：

### 第零部：先跑起来
| 章节 | 专章教程 | 核心主题与深度解析 |
|---|---|---|
| 00 | [前言](book/00_preface.md) | 这本书要造一个什么样的系统 |
| 00b | [构建、演示与 pipeline.json](book/00b_run_pipeline.md) | 编出来、跑起来、看懂进程表 |

### 第一部：框架骨架
| 章节 | 专章教程 | 核心主题与深度解析 |
|---|---|---|
| 01 | [第 01 章：用 C 造一个对象](book/01_oop_in_c.md) | C11 标准首成员内存保证、vtable 虚表分发、生命周期链与内存安全 |
| 02 | [第 02 章：可插拔 .so 与启动器](book/02_plugin_system.md) | ABI 门禁校验、RTLD_LOCAL 符号隔离、依赖注入与生命周期状态机 |
| 03 | [第 03 章：注册中心与参数系统](book/03_registry_and_params.md) | 参数注册与越界拒绝、热重载改值为何不能改行为 |
| 04 | [第 04 章：反射式状态机](book/04_state_machine.md) | 事件驱动状态转移矩阵与拓扑反射 |

### 第二部：通信与时间
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 05 | [第 05 章：消息总线](book/05_message_bus.md) | Pub/Sub 拓扑、动态消息帧、零拷贝内存池、QoS 丢弃策略 |
| 06 | [第 06 章：类型 ID、IDL 与序列化](book/06_serializer.md) | IDL 代码生成器、FNV-1a 哈希校验与二进制内存对齐 |
| 07 | [第 07 章：共享内存 IPC](book/07_ipc_channel.md) | POSIX SHM 环形队列、Robust Mutex 崩溃自愈、大 JSON 分块传输协议 |
| 08 | [第 08 章：统一传输与服务发现](book/08_discovery.md) | UDP 组播、心跳自愈与拓扑内省 |
| 09 | [第 09 章：时钟服务](book/09_clock_service.md) | 真实时钟 vs 仿真步进时钟、统一时间戳 uint64 μs 语义 |

### 第三部：执行与调度
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 10 | [第 10 章：C++20 协程框架 FlowCoro](book/10_coroutine.md) | Task、Awaitable、Select、超时与优雅取消 |
| 11 | [第 11 章：调度器与绑核](book/11_scheduler.md) | Classic FIFO + Choreo DAG + CPU 亲和性 |

### 第四部：录制与回放
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 12 | [第 12 章：Bag、MCAP 与 flowrec](book/12_bag_recording.md) | Bag 格式、标准 MCAP 规范与时序索引无损回放 |

### 第五部：ADAS 算法栈
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 13 | [第 13 章：感知：从点云到目标](book/13_lidar_tracking.md) | DBSCAN 点云聚类与 Kalman 目标跟踪、动静分类 |
| 14 | [第 14 章：定位融合与它的三种沉默](book/14_sensor_fusion.md) | EKF 状态估计、三个位置分支与真车配置下的沉默失败 |
| 15 | [第 15 章：行为决策](book/15_behavior_decision.md) | 8 状态 Behavior FSM（跟车、变道、让行、掉头）与 NOA 导航主动变道 |
| 16 | [第 16 章：Frenet 轨迹规划](book/16_trajectory_planning.md) | 网格枚举候选轨迹、ST 图 + DP 动态规划速度剖面 |
| 17 | [第 17 章：跟踪控制与特殊机动](book/17_tracking_control.md) | 横向三级级联 + LTV MPC + ManeuverTracker 掉头泊车 |
| 18 | [第 18 章：安全包络与降级](book/18_safety_envelope.md) | 仲裁规则、降级阶梯与健康心跳 |
| 19 | [第 19 章：执行器 PWM 与 SocketCAN](book/19_socketcan_actuator.md) | 脉宽映射、看门狗与 CAN 后端 |

### 第六部：仿真、可视化与学习闭环
| 章节 | 专章教程 | 核心主题 |
|---|---|---|
| 20 | [第 20 章：FlowSim 场景与世界](book/20_flowsim_scenario_design.md) | 多 edge 路网拓扑、NPC 交互与 OpenDRIVE 桥接 |
| 21 | [第 21 章：VLA、世界模型与无人车前沿](book/21_vla_world_model_frontier.md) | 行业坐标与 2026 年共识，唯一大量引用仓库外材料的一章 |
| 22 | [第 22 章：可视化与内省](book/22_flowmond_3d_vis.md) | Three.js 前端、航位推算 Dead Reckoning 与 View 模块规范 |
| 23 | [第 23 章：验证关卡与回归评估](book/23_demo_evaluator.md) | Demo Evaluator、分层校验阶梯 L0/L1/L2、参数敏感度扫描 |
| 24 | [第 24 章：端到端学习闭环](book/24_e2e_learning_loop.md) | 数据采集、v3 59 维特征、tiny-MLP/PyTorch 训练、DAgger 与 Promote 门禁 |

完整阅读顺序与每章的源码对照见 [BOOK.md](BOOK.md)。

### 附录

| 文档 | 内容 |
|---|---|
| [A. SocketCAN 与 PWM 执行器落地指南](book/19_socketcan_actuator.md) | RC 智能小车与真车底盘软硬件连接、SocketCAN 与 PCA9685 PWM 驱动 |
| [B. 术语表](GLOSSARY.md) | 全书反复出现的名词一句话定义 |
| [C. 全书索引](INDEX.md) | 按任务 / 主题 / 文件三种方式检索 |