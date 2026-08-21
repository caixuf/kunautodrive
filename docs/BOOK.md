# 《KunAutoDrive：从零构建高性能自动驾驶系统与仿真引擎》

> **这是什么**：一本从微内核底座、调度系统、ADAS 规控算法栈到仿真闭环与真车部署的**工业级自动驾驶全栈技术专著**。
>
> **怎么读**：按系统分卷组织。每一章均包含**理论数学建模、工业级架构剖析、生产级核心代码实现、常见故障 Postmortem 与练习思考**。
> 正文全部位于 `docs/book/` 目录下，本页为全书总序与完整目录索引。

---

## 全书总序

[**前言：这本书教你造一辆会自己开的车**](book/00_preface.md) —— 深入探讨为什么需要构建高性能 C11/C++20 自动驾驶微内核、KunAutoDrive 的核心设计哲学、五条职责铁律以及贯穿全书的仿真优先与数据闭环体系。

---

## 全书目录（5 大卷 · 22 章）

### 第一卷：微内核与系统编程底座 (Core & OS Primitives)

| 章节 | 章节名称 | 核心主题与深度解析 |
|---|---|---|
| 01 | [第 01 章：C 语言面向对象与微内核架构](book/01_oop_in_c.md) | C11 标准首成员内存对齐保证、vtable 虚函数分发、生命周期链与内存安全 |
| 02 | [第 02 章：dlopen 插件化系统与微内核解耦](book/02_plugin_system.md) | ABI 门禁校验、RTLD_LOCAL 符号隔离、依赖注入与生命周期状态机 |
| 03 | [第 03 章：高性能进程内消息总线](book/03_message_bus.md) | Pub/Sub 拓扑、64KB 动态消息帧、Free-List 零拷贝内存池、QoS 丢弃策略 |
| 04 | [第 04 章：跨进程共享内存通信](book/04_ipc_channel.md) | POSIX SHM 环形队列、Robust Mutex 崩溃自愈、大 JSON 分块传输协议 |
| 05 | [第 05 章：数据持久化引擎](book/05_bag_recording.md) | Bag v2 格式、标准 MCAP 规范与时序索引无损回放 |
| 06 | [第 06 章：确定性统一时钟服务](book/06_clock_service.md) | 物理真实时钟 vs 仿真步进时钟、统一时间戳 uint64 μs 语义 |
| 07 | [第 07 章：零反射类型安全序列化层](book/07_serializer.md) | FNV-1a 编译期哈希 Type ID、IDL 代码生成器与跨语言序列化 |

### 第二卷：执行流与高级调度 (Scheduler & Coroutines)

| 章节 | 章节名称 | 核心主题与深度解析 |
|---|---|---|
| 08 | [第 08 章：反射式状态机与白盒诊断](book/08_state_machine.md) | 转移矩阵、Guard 守卫条件、Entry/Exit 钩子、环形历史追踪器 |
| 09 | [第 09 章：去中心化服务发现与拓扑自愈](book/09_discovery.md) | UDP 组播信标（239.255.0.100:5500）、全网拓扑图与自动 SHM 管道建立 |
| 10 | [第 10 章：C++20 协程通信框架 FlowCoro](book/10_coroutine.md) | Awaitable 原语（next_for、select、ask）、CAS 原子恢复守卫 |
| 11 | [第 11 章：DAG 任务流与混合调度器](book/11_scheduler.md) | Kahn 拓扑排序、CPU 核心亲和性绑定、RateControl 周期控制与微秒级抖动压制 |

### 第三卷：ADAS 算法栈从理论到实现 (Algorithms & Pipeline)

| 章节 | 章节名称 | 核心主题与深度解析 |
|---|---|---|
| 12 | [第 12 章：点云聚类与卡尔曼目标追踪](book/12_lidar_tracking.md) | 3D 点云 DBSCAN 密度聚类、3D Bounding Box、Kalman Filter 与匈牙利数据关联 |
| 13 | [第 13 章：多传感器融合与状态估计](book/13_sensor_fusion.md) | 非线性运动学模型、扩展卡尔曼滤波（EKF）、异步 GPS/IMU 观测更新与协方差稳定 |
| 14 | [第 14 章：离散行为决策状态机与 NOA 导航变道](book/14_behavior_decision.md) | 8 状态 FSM（巡航/跟车/变道/避让/掉头/急停）、RSS 责任敏感安全模型与导航路由驱动 |
| 15 | [第 15 章：笛卡尔-Frenet 轨迹采样与速度规划](book/15_trajectory_planning.md) | Frenet 坐标系映射、五次多项式横向采样、S-T 图动态规划与凸二次规划（QP）速度优化 |
| 16 | [第 16 章：跟踪控制与特殊机动控制律](book/16_tracking_control.md) | Stanford Stanley 几何横向控制律、时变线性模型预测控制（LTV MPC）与 ManeuverTracker |
| 17 | [第 17 章：安全包络、看门狗与急停闸门](book/17_safety_envelope.md) | 独立 Guardian 节点架构、多级 TTC 碰撞时间分级与径向避让走廊豁免机制 |

### 第四卷：仿真验证、学习闭环与运维 (Sim, Learning & Ops)

| 章节 | 章节名称 | 核心主题与深度解析 |
|---|---|---|
| 18 | [第 18 章：FlowSim 场景设计与高保真路网仿真](book/18_flowsim_scenario_design.md) | 场景 JSON DSL、多 Edge 路网拓扑与智能 NPC 状态机 |
| 19 | [第 19 章：端到端学习闭环与模型准入](book/19_e2e_learning_loop.md) | 59 维多帧时序特征提取、DAgger 在线迭代自举与 Promote 准入门禁 |
| 20 | [第 20 章：系统可观测性与 3D 数字孪生前端](book/20_flowmond_3d_vis.md) | flowmond 守护进程、Three.js 模块化 View 与前端死推算插值平滑 |
| 21 | [第 21 章：黑盒自动化回归评估体系](book/21_demo_evaluator.md) | L0/L1/L2 三层测试金字塔、黑盒场景回归器与网格参数敏感性扫描 |

### 第五卷：真车部署与硬件落地 (Hardware Deployment)

| 章节 | 章节名称 | 核心主题与深度解析 |
|---|---|---|
| 22 | [附录 A：真车部署与硬件落地指南](book/22_socketcan_actuator.md) | Linux SocketCAN 内核网卡驱动、MCP2515 SPI-CAN、PCA9685 I2C-PWM 与硬件级 E-Stop |

---

## 附录与权威规范

| 文档 | 说明 |
|---|---|
| [GLOSSARY.md](GLOSSARY.md) | 系统全栈术语表与规范词汇 |
| [INDEX.md](INDEX.md) | 全书任务、主题与文件三维检索索引 |
| [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) | 核心 C11/C++20 API 接口速查手册 |
| [README.md](README.md) | KunAutoDrive 权威设计与架构规范导航 |
