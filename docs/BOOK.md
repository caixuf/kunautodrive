# KunAutoDrive 技术全书

> **本书是什么**：这不是一份文档清单，而是一本按阅读顺序组织的书。它把 `docs/` 下
> 46 篇分散的文档重组成九卷 + 附录，每卷有导读、每章有定位，读者可以按「卷」逐章读，
> 也可以按需求直接查「索引」。
>
> **本书不是什么**：本书不复制各文档的正文，只负责**组织、串联与导读**。每一章正文
> 都在对应的权威文档里，本书给出的是「为什么读、先读什么、读的时候带着什么问题」。

---

## 总序：写给谁，为什么写

KunAutoDrive 是一个轻量级自动驾驶中间件。它做三件事：

1. **进程内 Pub/Sub 消息总线 + 调度器** —— 让 15+ 个节点像搭积木一样组合成一条
   从传感器到执行器的完整链路；
2. **传输层（local / IPC / TCP）+ 发现 + 注册中心** —— 让节点能跨进程通信、被
   监控、被调参、被回放；
3. **一整套 ADAS 参考实现** —— 仿真、感知、规划、控制、安全、可视化、学习闭环，
   从代码到硬件部署都有真实可跑的东西。

这本书回答三个问题：

- **它怎么跑起来**（卷一）：十分钟从零到看到 3D 仪表盘。
- **它怎么被造出来的**（卷二 ~ 卷七）：中间件的地基、算法链、仿真世界、地图、
  可视化、学习闭环——每块的设计哲学与实现。
- **它怎么上真车**（卷八）以及**怎么保证不倒退**（卷九）：硬件部署与验证体系。

### 这本书的写法约定

| 约定 | 含义 |
|------|------|
| **卷一~卷九** | 逻辑分卷。每卷以「导读」开篇，说明这卷讲什么、前置要求、阅读顺序 |
| **章** | 每章对应一篇既有文档（教程或参考文档）。章首一句话说明「这一章解决什么问题」 |
| **附录** | API 速查、术语表、全书索引，供查询不供通读 |
| **「路线」** | 书末提供多条阅读路线：新人线 / 架构师线 / 算法线 / 可视化线 / 部署线 |

### 核心心智模型（全书反复出现）

```
sim_world → sensor_model → perception → fusion → planning → control → safety_control → monitor
     ↓            ↓             ↓           ↓          ↓          ↓            ↓            ↓
 vehicle/state  sensor/lidar perception/  fusion/  planning/  control/raw  control/cmd  dashboard
                sensor/gps  obstacles   localization trajectory  _cmd                     JSON
                     ↓             ↓           ↓          ↓          ↓            ↓
              ════════════════ Message Bus ════════════════
                                    ↓
                             Transport (IPC/TCP) → Discovery → FlowRegistry
                                    ↓
                             flowmond (IPC stats bridge + HTTP/SSE) → DashBoard
```

贯穿全书的几条「铁律」（详见各章，但先记住）：

- **规划是速度与轨迹的唯一权威**；控制只做轨迹跟随；安全层只做限幅与紧急制动；
  仿真只是被控对象。职责越界是最难修的 bug 源头。
- **行进方向唯一事实源 = flowsim `road/ref_path.reverse`**，谁都不许自己猜方向。
- **算法升级必须先 Python 仿真验证，再移植 C++**——改算法不许直接改 C++ 靠编译迭代。
- **改完代码必跑验证**：`pipeline_check` → `demo_evaluator` → 场景回归 → 参数回归。

---

## 全书目录

### 卷一 · 认识 KunAutoDrive（快速入门）

> **导读**：本卷是唯一建议所有人从第一页读到最后一页的卷。目标是让你**亲手把它跑
> 起来**，并建立「这个项目由哪些零件组成」的整体地图。读完本卷，你应该能回答：
> demo 里每一个方框是干什么的？数据从哪来到哪去？改一个参数怎么生效？

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 1.1 项目概览与运行 | [根 README](../README.md) | 一键 demo 怎么起、端口是什么、架构总图 |
| 1.2 代码地图 | [CODE_WIKI.md](CODE_WIKI.md) | 15 个节点都在哪、控制子系统怎么读代码 |
| 1.3 仿真指南 | [SIMULATION_GUIDE.md](SIMULATION_GUIDE.md) | 三层仿真体系、场景库、场景矩阵回归怎么跑 |

### 卷二 · 核心中间件：地基是怎么打的

> **导读**：本卷是「再造一个中间件」的完整教程，也是本书篇幅最大的卷。建议按 2.1 →
> 2.13 顺序读，因为每章都建立在前一章之上：先学会用 C 写 OOP（2.1），才能理解插件
> 系统（2.2）和消息总线（2.3）；有了进程内总线，才能谈跨进程 IPC（2.4）……最后
> 2.14~2.16 给出整层的地图与参考。**前置**：卷一；会读 C。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 2.1 OOP in C | [tutorials/01_oop_in_c.md](tutorials/01_oop_in_c.md) | 没有 class 怎么实现封装/继承/多态 |
| 2.2 插件系统 | [tutorials/02_plugin_system.md](tutorials/02_plugin_system.md) | dlopen 怎么加载一个节点、插件接口长什么样 |
| 2.3 消息总线 | [tutorials/03_message_bus.md](tutorials/03_message_bus.md) | Pub/Sub 的订阅匹配、QoS、线程模型 |
| 2.4 IPC 通道 | [tutorials/04_ipc_channel.md](tutorials/04_ipc_channel.md) | 跨进程共享内存/Unix socket 怎么传消息 |
| 2.5 Bag 录制回放 | [tutorials/05_bag_recording.md](tutorials/05_bag_recording.md) | 数据怎么录、怎么回放给算法 |
| 2.6 时钟服务 | [tutorials/06_clock_service.md](tutorials/06_clock_service.md) | 为什么所有节点必须走 clock_now_us |
| 2.7 序列化 | [tutorials/07_serializer.md](tutorials/07_serializer.md) | 消息怎么编解码、跨语言/跨版本 |
| 2.8 状态机 | [tutorials/08_state_machine.md](tutorials/08_state_machine.md) | 状态机框架怎么用、转移表为什么必须完备 |
| 2.9 服务发现 | [tutorials/09_discovery.md](tutorials/09_discovery.md) | 节点怎么互相找到对方 |
| 2.10 数据融合 | [tutorials/10_fusion.md](tutorials/10_fusion.md) | 多传感器定位融合怎么组织 |
| 2.11 协程 | [tutorials/11_coroutine.md](tutorials/11_coroutine.md) | FlowCoro 怎么免锁调度、node_pump 为什么必须用 |
| 2.12 航位推算 | [tutorials/14_dead_reckoning.md](tutorials/14_dead_reckoning.md) | GPS 丢帧时怎么外推位姿 |
| 2.13 监控架构 | [MONITORING_ARCHITECTURE.md](MONITORING_ARCHITECTURE.md) | flowmond 怎么聚合指标、IPC/文件双链路怎么桥接 |
| 2.14 API 速查 | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) | 每个中间件 API 的签名与用法（工具书） |
| 2.15 平台抽象 | [PLATFORM_ABSTRACTION.md](PLATFORM_ABSTRACTION.md) | macOS/Linux 差异怎么被一层收口 |

### 卷三 · ADAS 算法链：从传感器到执行器

> **导读**：本卷回答「一辆会自己开的车，代码是怎么分工的」。先读 3.1 看 15 节点全链路
> 和 topic 流向，再读 3.2 看每个模块**实际用的算法**（真实现，不是论文），3.3 讲怎么
> 把新算法接进链路，3.4 讲**验证模式**（Python 仿真先行），3.5 和 3.6 是规划与控制两个
> 重头戏的深入。**前置**：卷二（尤其 2.2、2.3）。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 3.1 Pipeline 架构 | [PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md) | 15 节点各自输入输出、控制闭环、关键参数 |
| 3.2 算法栈 | [ALGORITHM_STACK.md](ALGORITHM_STACK.md) | 每个模块用什么算法、在哪个文件、关键约束 |
| 3.3 算法集成 | [ALGORITHM_INTEGRATION.md](ALGORITHM_INTEGRATION.md) | 第三方算法库怎么接成插件节点 |
| 3.4 算法验证模式 | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) | Python 仿真先行 + C++ 移植的完整流程 |
| 3.5 速度规划（ST 图 + DP） | [PLANNING_SPEED_UPGRADE_DESIGN.md](PLANNING_SPEED_UPGRADE_DESIGN.md) | 红灯墙/动态障碍/曲率限速怎么进速度剖面 |
| 3.6 控制标定 | [CALIBRATION_GUIDE.md](CALIBRATION_GUIDE.md) | 运动学/动力学模型差异、PID/横向参数怎么标 |

### 卷四 · 仿真世界：FlowSim 与场景设计

> **导读**：仿真卷单独成卷，因为它是「算法验证的地基」——所有算法升级都先在仿真里
> 验证。4.1 讲被控对象（车辆物理模型），4.2 讲仿真里的几何/运动不变式（digest +
> invariant，专抓「车飞出路面」类 bug），4.3 手把手设计一个场景。**前置**：卷三。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 4.1 FlowSim 物理 | [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) | 运动学自行车/动力学模型切换、碰撞/护栏/重力 |
| 4.2 Sim Digest 与 Invariant | [SIM_DIGEST.md](SIM_DIGEST.md) | 静态/动态 digest 编码什么、invariant 断言抓什么 |
| 4.3 场景设计 | [tutorials/16_flowsim_scenario_design.md](tutorials/16_flowsim_scenario_design.md) | 场景 JSON 怎么定义 actor、NPC、红绿灯 |

### 卷五 · 地图引擎：城市从哪来

> **导读**：本卷讲「地图数据怎么从 OSM/DSL 变成车能开的路」。5.1 是总纲（DSL 单一枢纽
> 架构 + 五条铁律），5.2 讲 .kmap → map.json 的工具链与 A* 路由，5.3 讲 esmini/自研
> 的职责边界，5.4 讲道路标线。**前置**：无硬前置，但建议先读卷一。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 5.1 地图生成模块 | [MAP_GENERATION_MODULE.md](MAP_GENERATION_MODULE.md) | DSL 分层、五条铁律、适配层怎么接 OSM |
| 5.2 地图引擎与路由 | [MAP_ENGINE_ROUTING.md](MAP_ENGINE_ROUTING.md) | map.json/routes.json/.kmap 契约、A* 路由、经验坑 |
| 5.3 边界分析 | [map_engine_boundary.md](map_engine_boundary.md) | esmini 拥有什么、自研 MapEngine 该负责什么 |
| 5.4 道路标线 | [ROAD_MARKINGS_MODULE.md](ROAD_MARKINGS_MODULE.md) | 虚线段/实线/双黄怎么生成与渲染 |

### 卷六 · 可视化与仪表盘：让数据可见

> **导读**：本卷讲前端 3D 渲染层。6.1 是总架构（vis/ 分层、坐标约定、帧契约），6.2 讲
> 渲染经验（性能降档、贴地、相机），6.3 讲车道渲染管线，6.4 讲怎么新增一个 View，6.5/6.6
> 是前后端契约，6.7 是故障排查。**前置**：卷一即可，会看 JS 更好。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 6.1 可视化架构 | [VISUALIZATION_ARCHITECTURE.md](VISUALIZATION_ARCHITECTURE.md) | vis/ 目录分层、坐标唯一事实源、帧契约 |
| 6.2 3D 渲染经验 | [VIS_3D_RENDERING.md](VIS_3D_RENDERING.md) | 性能降档、车轮贴地、相机跟随等沉淀 |
| 6.3 3D 车道管线 | [VIS_3D_LANE_PIPELINE.md](VIS_3D_LANE_PIPELINE.md) | 车道线/路面/标线从数据到 mesh 的管线 |
| 6.4 vis 模块接入 | [VIS_MODULE_GUIDE.md](VIS_MODULE_GUIDE.md) | 新增一个 View 的步骤与规范 |
| 6.5 FlowBoard 数据契约 | [FLOWBOARD_CONTRACT.md](FLOWBOARD_CONTRACT.md) | /api/topology 归一化输出、source 三态 |
| 6.6 FlowBoard 场景契约 | [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) | road_network schema、场景帧字段定义 |
| 6.7 仪表盘故障排查 | [TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md) | 3D 黑屏/挂死/卡顿怎么查 |

### 卷七 · 数据闭环与学习：车会越开越好

> **导读**：本卷讲「采集 → 训练 → 影子推理 → 晋级 → OTA」的车端学习闭环。7.1 是总纲
> （一页看懂四阶段），7.2 讲数据怎么采（flowrec），7.3 讲数据闭环链路，7.4 是 E2E 训练
> 的逐步教程。**前置**：卷三（尤其 3.1 的 learning loop 节点）。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 7.1 学习闭环 | [LEARNING_LOOP.md](LEARNING_LOOP.md) | 四阶段架构、数据/模型契约、promote 门禁、OTA |
| 7.2 数据采集节点 | [FLOWREC.md](FLOWREC.md) | flowrec 怎么配置化留存节点 |
| 7.3 数据闭环链路 | [DATA_CLOSED_LOOP.md](DATA_CLOSED_LOOP.md) | PEM 与车端数据采集链路 |
| 7.4 E2E 学习闭环教程 | [tutorials/13_e2e_learning_loop.md](tutorials/13_e2e_learning_loop.md) | 从零采集到训练到部署的完整步骤 |

### 卷八 · 真车与硬件：从仿真到实车

> **导读**：本卷把代码搬上真车。8.1 是完整部署指南（仿真/真车双 pipeline、硬件准备、
> 调试），8.2 是 RC 小车清单，8.3 是 SocketCAN 执行器教程。**前置**：卷一 + 卷三；
> 有 Linux 开发机。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 8.1 真车硬件部署 | [HARDWARE_DEPLOYMENT.md](HARDWARE_DEPLOYMENT.md) | 部署目录、双模式对比、CAN/PWM 调试、FAST-LIO2 |
| 8.2 RC 小车清单 | [RC_CAR_HARDWARE_CHECKLIST.md](RC_CAR_HARDWARE_CHECKLIST.md) | 整车清单、接线、验收项 |
| 8.3 SocketCAN 执行器 | [tutorials/15_socketcan_actuator.md](tutorials/15_socketcan_actuator.md) | CAN 总线怎么驱动油门/转向 |

### 卷九 · 工程实践与验证：怎么保证不倒退

> **导读**：本卷讲「质量门禁」。9.1 是评估器（demo_evaluator）怎么给整条链评分，
> 9.2 讲参数回归（改参数后如何检测退化）。这套门禁是本书反复强调「改完必跑」的依据。
> **前置**：卷一。

| 章 | 正文 | 读完后你能回答 |
|----|------|----------------|
| 9.1 演示评估器 | [tutorials/12_demo_evaluator.md](tutorials/12_demo_evaluator.md) | 黑盒评分怎么采、碰撞/偏航/停滞怎么判 |
| 9.2 验证体系总览 | [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) | 分层验证阶梯（L0~L3）全貌 |

### 附录

| 内容 | 正文 |
|------|------|
| A. API 速查 | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |
| B. 术语表 | [GLOSSARY.md](GLOSSARY.md) |
| C. 全书索引 | [INDEX.md](INDEX.md) |

---

## 阅读路线

按角色选一条线，读完你就能胜任对应工作：

### 路线一 · 新人线（0 → 跑通 → 理解链路）
卷一全读 → 3.1 → 3.2 → 2.3（消息总线）→ 2.2（插件）→ 9.1（评估器）→ 4.1（物理模型）。
**产出**：能独立跑 demo、改场景、看懂每个节点的输入输出。

### 路线二 · 架构师线（系统全貌 + 设计决策）
2.3 → 2.4 → 2.9 → 2.11 → 2.13 → 5.1 → 5.2 → 6.1 → 3.1 → 3.2。
**产出**：能回答「加一个新节点/新 topic/新模块，改动面在哪」。

### 路线三 · 算法线（规划/控制/行为）
3.2 → 3.4 → 3.5 → 3.6 → 4.1 → 4.2 → 9.1。
**产出**：能按「Python 仿真先行 → 移植 C++ → 评估器验证」流程改算法。

### 路线四 · 可视化线（前端 3D）
6.1 → 6.3 → 6.4 → 6.6 → 6.2 → 6.7。
**产出**：能新增 View、修渲染 bug、通过 `npm run vis:check:all`。

### 路线五 · 部署线（真车）
卷一 → 8.1 → 8.2 → 8.3 → 2.12（航位推算）→ 7.1（影子推理）。
**产出**：能把系统部署到真车并调试。

---

## 维护约定

本书是「组织层」，不承载模块事实。修改约定如下：

- **改运行时行为** → 更新对应模块的权威文档；本书与教程只补原理和示例。
- **改 API / JSON 字段** → 更新对应契约文档（`road_network` 只在
  [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) 定义）。
- **新增文档** → 在对应卷的表里登记一行（章号 + 一句话定位），保持目录不脱节。
- **删除文档** → 同步删掉本目录条目与[索引](INDEX.md)里的引用。
- 本页的「阅读路线」随项目阶段演进，必要时在提交说明里注明为何调整。
