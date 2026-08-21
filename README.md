# KunAutoDrive

> 面向自动驾驶与机器人的仿真优先中间件：C11 核心、C++20/FlowCoro 节点、
> 配置驱动插件管线，以及可观测、可回放、可评估的数据闭环。

[![CI](https://github.com/caixuf/kunautodrive/actions/workflows/ci.yml/badge.svg)](https://github.com/caixuf/kunautodrive/actions)
![License](https://img.shields.io/badge/license-MIT-blue)
![C](https://img.shields.io/badge/C-11-555555)
![C++](https://img.shields.io/badge/C++-20-659ad2)

## 演示截图

<!--
  截图约定：把真实截图放到 docs/screenshots/dashboard_3d.png（16:9、宽 >= 1280px、
  单张 < 2MB），内容为 bash scripts/demo.sh 后浏览器 http://localhost:8800 的
  3D 仪表盘实拍（建议含车辆在场景中巡航/变道），不要用示意图或占位图。
-->

<img src="docs/screenshots/dashboard_3d.png" alt="KunAutoDrive 3D 仪表盘" width="100%">

## 先看这里

KunAutoDrive 当前是**仿真优先、可复现的实验与集成平台**：感知、融合、规划、
控制和学习算法应先在仿真中运行、观察、评分和回放，再逐步迁移到实车。项目
提供 RC 小车部署 profile；乘用车量产认证并非当前承诺范围。

| 你想做什么 | 从这里开始 |
|---|---|
| 运行 3D 自动驾驶演示 | [快速开始](#快速开始) |
| 用键盘接管车辆、检查轮向/车身方向 | [游戏诊断模式](#游戏诊断模式) |
| 部署到 RC 小车 | [车辆部署](#车辆部署) |
| 记录并分析 PEM 车端数据 | [PEM 数据闭环](#pem-数据闭环) |
| 修改算法并做回归 | [验证与 CI](#验证与-ci) |
| 了解系统组成 | [架构](#架构) 与 [文档导航](#文档导航) |

## 快速开始

```bash
git clone https://github.com/caixuf/kunautodrive.git
cd kunautodrive

# 构建并运行默认仿真；浏览器打开 http://localhost:8800
bash scripts/demo.sh

# CI/无图形环境常用形式
bash scripts/demo.sh --no-browser 15
```

默认管线是 `config/pipeline.json`，由 `flow_launcher` 加载各个 `.so` 节点。
它用于仿真和仪表盘开发，**不会写 PEM**。

常用选项：

```bash
bash scripts/demo.sh --scenario scenarios/curve_road.json --no-browser 30
bash scripts/demo.sh --record
bash scripts/demo.sh --multi
```

## 游戏诊断模式

运行 demo 后，打开 FlowBoard 左上角的**接管车辆**：

- 使用 `WASD` 或方向键直接控制车辆；再次退出即可恢复自动驾驶。
- 游戏 HUD 内的「灯光测试」按钮可切换近光、左/右转向灯和双闪；也可用
  `L/Q/E/H` 操作，`S/↓` 或空格测试刹车灯。
- HUD 同时展示转向输入、车身航向、实际移动方向和它们的偏差，用于定位车轮、
  死推和物理积分不一致的问题。
- 偏离道路时按 `R` 或点击**回到车道**，将车辆安全放回最近车道中心。

仿真约定 `steer > 0` 为左转（ENU heading/y 增大），`steer < 0` 为右转；
游戏模式是诊断工具，不改变规划、控制和安全模块的职责边界。

## 架构

```text
flowsim -> sensor_model -> perception -> fusion -> planning -> control
   -> safety_control -> monitor -> flowmond -> FlowBoard / PEM / evaluator
                         |
                         +-> data_recorder -> train -> inference -> OTA
```

| 层 | 关键能力 |
|---|---|
| 核心 | Pub/Sub 消息总线、local/IPC/TCP transport、调度器、时钟、参数、注册表 |
| 节点 | FlowCoro 协程节点、插件加载、健康检查、服务发现、状态机 |
| ADAS | FlowSim、传感器模型、感知/融合、行为、轨迹与速度规划、控制、安全闸门 |
| 可视化 | `flowmond`、FlowBoard 3D/2D、拓扑 JSON、IPC 与文件桥接回退 |
| 数据闭环 | Bag/MCAP、评估器、训练/影子推理、PEM 车端遥测、模型 OTA |

详细模块职责和数据流见
[可视化架构](docs/VISUALIZATION_ARCHITECTURE.md)、
[监控架构](docs/MONITORING_ARCHITECTURE.md)和
[数据闭环](docs/DATA_CLOSED_LOOP.md)。

## 车辆部署

当前可实际部署的 profile：

| Profile | 配置 | 用途 |
|---|---|---|
| `simulation` | `config/pipeline.json` | 默认仿真、算法开发和可视化 |
| `rc_car` | `config/pipeline_car.json` | GPS/IMU/激光雷达/执行器的 RC 小车模板 |

生成部署包：

```bash
bash scripts/deploy.sh --package
```

RC 小车接口、接线、标定和上车前检查见
[RC 小车硬件清单](docs/RC_CAR_HARDWARE_CHECKLIST.md)。量产 profile 中未实现的
车型仅是显式占位，部署工具会拒绝将它们作为可运行目标。

### Windows

Windows 原生单进程插件管线受支持。安装 CMake、Ninja、WinLibs MinGW-w64
（POSIX + UCRT，GCC 11+）和 Python 后：

```powershell
git clone https://github.com/caixuf/kunautodrive.git
cd kunautodrive
git submodule update --init --depth 1 third_party/esmini
powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -Duration 30
```

Windows 的完整运行时验收与部署说明见
[硬件部署指南](docs/HARDWARE_DEPLOYMENT.md)；Linux 到 Windows 的交叉编译
使用 `cmake/mingw-w64-x86_64.cmake`。

## PEM 数据闭环

`config/pipeline_car.json` 的 production 模式会启动两条 PEM 流：

- `monitor_node`：系统、topic、health 和降级事件等基础设施记录。
- `pem_collector_node`：通过 FlowCoro 异步回调采集 GPS/定位里程、行驶时长、
  地区迁移和降级事件。

两条流都具备 CRC、关键事件 `fsync`、按时间/大小轮转和目录配额保留。解析：

```bash
python3 tools/pem_dump.py /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --jsonl --type business /tmp/kunautodrive_pem_business_*.pem
```

PEM 不是“配置了就相信”：`pem_runtime_smoke` 会启动仿真 production 管线，确认
实际写出并成功解析 `trip:ci_simulation`，且作为 integration CI 门禁运行：

```bash
env -u LD_LIBRARY_PATH ctest --test-dir build --output-on-failure -R pem_runtime_smoke
```

字段契约、扩展回调方式和保留策略见[数据闭环](docs/DATA_CLOSED_LOOP.md)。

## 验证与 CI

修改前先选择最小有效验证；算法、管线和前端分别有独立门禁。

```bash
# 秒级离线管线检查
python3 tools/pipeline_check.py

# 运行时行为回归
python3 ci/evaluators/demo_evaluator.py --duration 45 --interval 0.5

# 多场景矩阵
python3 ci/evaluators/scenario_regression.py --baseline

# 节点/PEM 定向测试
env -u LD_LIBRARY_PATH ctest --test-dir build/modules/adas_nodes --output-on-failure -R pem_log_protocol

# FlowBoard 改动必须执行
npm run vis:check:all
```

完整测试分层和算法验证模式见
[算法验证模式](docs/ALGORITHM_VERIFY_PATTERN.md)；CI 定义见 `.github/workflows/ci.yml`。

## 开发入口

```bash
# 构建核心
bash build.sh release

# 启动配置驱动管线
./build/bin/flow_launcher config/pipeline.json --duration 30

# 查询运行中拓扑、参数和仪表盘
./build/bin/flowctl list
./build/bin/flowctl param list
```

新增节点应复用 `clock_service.h`、cJSON、`node_pump()` 和参数注册系统；
不要在节点中手写 JSON、裸用单调时钟或忙等执行器。代码入口和 API 速查见
[代码索引](docs/CODE_WIKI.md)与[API 速查](docs/API_QUICK_REFERENCE.md)。

## 文档导航

完整的中文模块导航、权威文档归属和教程入口见 [docs/README.md](docs/README.md)。
其中 [flowrec](docs/FLOWREC.md) 是配置化 topic 留存节点的独立说明。

## 技术专著与实战教程（book/）

《KunAutoDrive：从零构建高性能自动驾驶系统与仿真引擎》（中间件内核 FlowEngine 实战专著），完整 5 大卷索引见 [docs/BOOK.md](docs/BOOK.md)：

| 卷号 | 专卷主题 | 包含章节 |
|------|---------|---------|
| **第一卷** | **微内核与系统编程底座** | [01 语言面向对象](docs/book/01_oop_in_c.md) · [02 dlopen 插件化](docs/book/02_plugin_system.md) · [03 进程内消息总线](docs/book/03_message_bus.md) · [04 共享内存 IPC](docs/book/04_ipc_channel.md) · [05 数据持久化 Bag](docs/book/05_bag_recording.md) · [06 统一时钟服务](docs/book/06_clock_service.md) · [07 类型安全序列化](docs/book/07_serializer.md) |
| **第二卷** | **执行流与高级调度** | [08 反射式状态机](docs/book/08_state_machine.md) · [09 服务发现与拓扑](docs/book/09_discovery.md) · [10 FlowCoro 协程框架](docs/book/10_coroutine.md) · [11 DAG 混合调度器](docs/book/11_scheduler.md) |
| **第三卷** | **ADAS 算法栈从理论到实现** | [12 点云聚类与跟踪](docs/book/12_lidar_tracking.md) · [13 多传感器融合 EKF](docs/book/13_sensor_fusion.md) · [14 行为决策与 NOA](docs/book/14_behavior_decision.md) · [15 轨迹与速度规划](docs/book/15_trajectory_planning.md) · [16 跟踪控制与 MPC](docs/book/16_tracking_control.md) · [17 协程安全包络](docs/book/17_safety_envelope.md) |
| **第四卷** | **仿真验证、学习闭环与运维** | [18 FlowSim 场景设计](docs/book/18_flowsim_scenario_design.md) · [19 端到端学习闭环](docs/book/19_e2e_learning_loop.md) · [20 flowmond 3D 监控](docs/book/20_flowmond_3d_vis.md) · [21 黑盒回归评估体系](docs/book/21_demo_evaluator.md) |
| **第五卷** | **真车部署与硬件落地** | [附录 A SocketCAN 与 PWM 执行器落地指南](docs/book/22_socketcan_actuator.md) |

---

## 核心技术文档

> 完整文档索引见 [docs/README.md](docs/README.md) —— 按主题分组的全部文档 + 22 篇专著专章入口。

| 文档 | 主题 |
|-----|-------|
| [Quick Start](docs/QUICK_START.md) | 30 分钟教程 |
| [Technical Design](docs/TECHNICAL_DESIGN.md) | 架构设计 |
| [API Quick Reference](docs/API_QUICK_REFERENCE.md) | C API 参考 |
| [Simulation Guide](docs/SIMULATION_GUIDE.md) | 仿真测试指南 |
| [Visualization Architecture](docs/VISUALIZATION_ARCHITECTURE.md) | flowmond + vis/ 模块树（Layer + ViewRegistry + Qt 对象树）|
| [Vis Module Guide](docs/VIS_MODULE_GUIDE.md) | vis/ 模块接口契约 + 设计 AI 提示词模板 |
| [Monitoring Architecture](docs/MONITORING_ARCHITECTURE.md) | flowmond + stats bridge |
| [Pipeline Architecture](docs/PIPELINE_ARCHITECTURE.md) | Pipeline 设计 |
| [Algorithm Stack](docs/ALGORITHM_STACK.md) | 算法总览（各模块真实算法 × 文件对照） |
| [Algorithm Integration](docs/ALGORITHM_INTEGRATION.md) | 算法集成指南 |
| [Planning Speed Upgrade](docs/PLANNING_SPEED_UPGRADE_DESIGN.md) | ST 图 + DP 速度规划设计 |
| [FlowBoard Contract](docs/FLOWBOARD_CONTRACT.md) | 仪表盘数据契约 |
| [FlowBoard Scene Contract](docs/FLOWBOARD_SCENE_CONTRACT.md) | scene 数据契约 |
| [FlowSim 仿真指南](docs/SIMULATION_GUIDE.md) | flowsim 仿真模式对照 |
| [Hardware Deployment](docs/HARDWARE_DEPLOYMENT.md) | 硬件部署 |
| [RC Car Hardware Checklist](docs/RC_CAR_HARDWARE_CHECKLIST.md) | RC 小车硬件落地清单 |
| [Learning Loop](docs/LEARNING_LOOP.md) | 仿真内学习闭环 |
| [Troubleshooting 3D Dashboard](docs/TROUBLESHOOTING_3D_DASHBOARD.md) | 3D 仪表盘故障排查 |

---

## 许可证

MIT License. 产品与仓库名称为 **KunAutoDrive**。`FLOWENGINE_*`、`flowengine_core` 和 `libflowengine_*` 等保留为 ABI/API 兼容名称，不应作为普通重命名目标。
