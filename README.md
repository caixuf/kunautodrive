# KunAutoDrive

> 面向自动驾驶与机器人的仿真优先中间件：C11 核心、C++20/FlowCoro 节点、
> 配置驱动插件管线，以及可观测、可回放、可评估的数据闭环。

[![CI](https://github.com/caixuf/kunautodrive/actions/workflows/ci.yml/badge.svg)](https://github.com/caixuf/kunautodrive/actions)
![License](https://img.shields.io/badge/license-MIT-blue)
![C](https://img.shields.io/badge/C-11-555555)
![C++](https://img.shields.io/badge/C++-20-659ad2)

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
- HUD 同时展示转向输入、车身航向、实际移动方向和它们的偏差，用于定位车轮、
  死推和物理积分不一致的问题。
- 偏离道路时按 `R` 或点击**回到车道**，将车辆安全放回最近车道中心。

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

## 兼容性与许可证

产品与仓库名称为 **KunAutoDrive**。`FLOWENGINE_*`、`flowengine_core` 和
`libflowengine_*` 等保留为 ABI/API 兼容名称，不应作为普通重命名目标。

MIT License。
