# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# KunAutoDrive — 项目概览

轻量级自动驾驶中间件，核心是一个 Pub/Sub 消息总线 + 调度器 + 传输层。

> **开发流程：** 设计 → 执行 → 测试 → 迭代 → 清理 → 文档。详见 `~/.claude/skills/workflow/SKILL.md`（入口路由）。
> 改完代码后必跑：`/verify` → `/code-review` → `/simplify` → commit → 更新文档。
> **算法升级必先：** `/py-sim-first` — Python 仿真验证 → 参数扫描 → 再移植到 C++。
>
> ⚠️ **`~/.claude/skills/*` 路径未随仓库分发**（本机 2026-09 核查不存在）。读不到时按其
> 自述的方法论执行即可：分层探针 + 值传播验证 + 状态锁死 + 缓存层检查；下方各节的铁律与
> 故障模式表才是仓库内的权威副本。
>
> **行为异常排查**（转向灯反/该停不停/该走不走/刹停到 0/改代码现象不变）→
> 先看 `~/.claude/skills/debugging/SKILL.md`（分层探针 + 值传播验证 + 状态锁死 + 缓存层检查，
> 2026-07 修 8 个连环 bug 的方法论），别凭直觉猜层。
>
> **重构类改动** commit body 必含 `Removed:` 段（详见 `~/.claude/skills/workflow/SKILL.md` 第七节），纯新增/fix/docs 可省略。

## 低维护治理（2026-09）

一次对齐后靠 CI 守门，避免拓扑声明再次漂移：

| 闸门 | 命令 | 挡住什么 |
|------|------|----------|
| topic-contract | `python3 ci/gates/topic_contract_check.py` | `pipeline.json` ↔ `s_inputs`/`s_outputs` 漂移 |
| sensor-wiring | `python3 ci/gates/sensor_wiring_check.py` | sensor 模式下 `lidar_mode`/两处 `lidar_max_range_m` 不一致（静默丢点） |
| zombie-ban | `python3 ci/gates/zombie_ban_check.py` | 退役 Python 仪表盘入口回潮 |
| scenario/clock/map | 已有 | 空场景引用 / 禁 raw clock / 断链地图 |

**纪律：**
- 仪表盘唯一后端：`flowmond`（勿再引入第二套 HTTP server）
- `profile=default` 才是 demo/CI 主路径；`experimental`（cortex）/`hw`（car）启动会打警告
- 不拆 god-node 当日常活；行为靠 `demo_evaluator` 不变量锁
- `docs/PIPELINE_ARCHITECTURE.md` 已废弃，勿再手写第二份拓扑

## 架构

```
sim_world → sensor_model → perception → fusion → planning → control → safety_control → monitor
     ↓            ↓             ↓           ↓          ↓          ↓            ↓            ↓
 vehicle/state  sensor/lidar perception/  fusion/  planning/  control/raw  control/cmd  dashboard
                sensor/gps  obstacles   localization trajectory  _cmd                     JSON
                     ↓             ↓           ↓          ↓          ↓            ↓
              ════════════════ Message Bus ════════════════════
                                    ↓
                             Transport (IPC/TCP) → Discovery → FlowRegistry
                                    ↓
                             flowmond (IPC stats bridge + HTTP/SSE) → DashBoard
```

上图是核心控制链。`config/pipeline.json`（default profile）实际跑 **16 个进程**，
在核心链之外还有 `object_tracker`（感知跟踪）、`navigation`（路由 + 行进方向）、
`behavior_planner`（FSM）、`inference` / `data_recorder` / `learner` / `model_ota`
（学习闭环 Stage 0→2→OTA）、`bev_detection`（BEV 视觉）。节点清单以
`config/pipeline.json` 的 `processes[]` 为准，勿信任何手写拓扑文档。

## 模块职责铁律（架构设计 — 2026-08 掉头死锁 8 环连坏后确立）

> 2026-08 排掉头死锁时发现 8 个环环相扣的坏因,根源全是**职责越界**:
> 仿真替控制做操控决策、安全层替规划做速度决策、控制读错权威数据源。
> 以下职责边界是硬约束,**跨界写逻辑的代码不会被合并**。

| 模块 | 唯一职责 | 禁止做的事 |
|------|---------|-----------|
| **flowsim**(仿真) | 纯被控对象:执行 throttle/brake/steer/gear,物理积分,发布真值 | ❌ 操控决策(自动回正/辅助转向/替驾驶员判断);❌ 依据"合理性"改写控制指令。物理限幅(轮胎极限/执行器速率)可以有,"驾驶策略"不准有 |
| **planning** | 速度与轨迹的**唯一权威**:command_speed、trajectory.points[].v 由它一家说了算 | ❌ 直接输出执行量(throttle/steer) |
| **control** | 纯轨迹跟随:从 trajectory 推 gear/steer/throttle,不质疑轨迹 | ❌ 自己发明目标速度/路径;❌ 绕开 trajectory 读别的源做决策(fallback 巡航是唯一例外且要收敛) |
| **safety_control** | 纯安全闸门:只做"限幅+紧急制动",不理解任务意图 | ❌ 用巡航假设治理机动工况(必须识别机动窗口后豁免);❌ 改写任务性指令(变道/掉头的方向盘) |
| **behavior** | 离散状态机:决定"做什么"(变道/掉头/跟车),不管"怎么做" | ❌ 输出连续控制量;❌ 状态转移表缺行(任何事件×状态组合必须显式可达或显式拒绝) |

跨模块信号约定:
- control→safety 带外信号唯一通道 = `raw.mode` 字符串(如 `"+MANEUVER"` 后缀)
- 行进方向唯一事实源 = flowsim `road/ref_path.reverse`(navigation/behavior/control 全部消费它,不许各自猜)
- 同一功能出现第二份实现(如两处都算 forward_space)= 违规,必须收敛到权威模块

## 关键文件

| 文件 | 作用 |
|------|------|
| `src/core/message_bus.c` | 进程内 Pub/Sub 总线 |
| `src/core/transport.c` | 统一传输抽象（local/IPC/TCP） |
| `src/core/scheduler.c` | 任务调度器（classic/choreo 模式） |
| `src/flowmond.c` | 监控守护进程（HTTP 仪表盘 + IPC 统计/仪表盘桥接 + 自动重连） |
| `src/core/monitor_server.c` | 内嵌 HTTP 服务器（多线程连接、/tools 静态资源、JSON 安全转义、过期缓存自动 fallback） |
| `src/core/stats_bridge.c` | 跨进程 topic 统计 IPC 桥接 |
| `src/core/dashboard_bridge.c` | 跨进程仪表盘 JSON IPC 桥接（分块传输协议） |
| `modules/adas_nodes/sensor_model_node.c` | 传感器模型（LiDAR/GPS/Camera，FOV/遮挡/噪声） |
| `modules/adas_nodes/safety_control_node.cpp` | FlowCoro 协程安全控制（TTC/横向交叉/行人防护） |
| `modules/adas_nodes/behavior_planner_node.cpp` | 8 状态 FSM 行为决策（跟车/变道/停车/让行/掉头） |
| `modules/adas_nodes/navigation_node.c` | 路由步骤 + 行进方向（消费 flowsim `ref_path.reverse`） |
| `modules/adas_nodes/st_graph.c` | ST 图 + DP 速度规划（红灯墙/动态障碍/曲率限速，移植自 speed_planner_sim.py 11/11 PASS） |
| `modules/adas_nodes/maneuver_tracker.h` | 机动跟随器（掉头/泊车：弧长推进 + D/R 挡位 + 倒挡横向反号，header-only 可单测） |
| `tools/flowboard/index.html` | 前端仪表盘（3D+2D+图表+D3 拓扑，ES modules） |
| `tools/flowboard/js/vis/theme/roadStyle.js` | 标线视觉声明式样式表（MARKING 语义 + STYLE 双风格：real 写实 / sr 科技，换肤即换表） |
| `tools/flowboard/js/vis/model/MapData.js` | OSM 权威地图加载 + 走廊过滤 + 按段加载能力（fetchSegmentIndex/loadSegment） |
| `tools/corridor_map.py` | 路线走廊瓦片化 + LOD 标注 + 分段（郑东 76.5MB→12.4MB，`--segments` 生成段） |
| `tools/foxglove_bridge.py` | Foxglove Studio WebSocket 桥接 |
| `ci/evaluators/demo_evaluator.py` | 回归评估器：采样 JSON 并自动评分（碰撞/偏航/停滞/频率） |
| `tools/pipeline_check.py` | 离线管道完整性检查：不启动 demo，秒级验证 9 类 32 项指标 |
| `tools/quick_verify.py` | 交互式调参验证工具：实时仪表盘 + 即时 eval 评分 |
| `ci/evaluators/test_param_regression.py` | 参数回归对比：保存 baseline，改参后自动检测退化 |
| `scripts/demo.sh` | 一键启动脚本 |
| `src/flow_launcher.c` | 配置驱动启动器（读取 pipeline.json，dlopen 加载插件节点） |
| `src/flowctl.c` | CLI 工具（list/inspect/dashboard/param/bag 等子命令） |
| `src/core/flow_registry.c` | 统一元信息注册中心（Task/Topic/Type/Plugin/Schema） |
| `src/core/param_registry.c` | 参数系统（int/float/bool/string，范围校验，hot-reload） |
| `src/core/param_bridge.c` | 参数跨进程通道（AF_UNIX，`flowctl param set` 边跑边调） |
| `src/core/scenario_loader.c` | 场景 JSON 加载器（actor 定义 + ego 配置） |
| `include/platform_compat.h` | 跨平台兼容层（macOS⇄Linux，CMake force-include，仅 APPLE 生效）：pthread 命名签名、robust mutex、condvar 时钟降级 |
| `modules/adas_nodes/flowsim/physics.cpp` | 运动学自行车模型 + 可选线性轮胎二自由度动态模型（pipeline.json `physics_model`，低速<5m/s 退化运动学）；碰撞/护栏/重力见 [FLOWSIM_PHYSICS.md](docs/FLOWSIM_PHYSICS.md)、`/flowsim-physics` |
| `modules/adas_nodes/flowsim/entity.h` | 仿真实体（含 v_x_body/v_y_body/yaw_rate/F_yf/F_yr、垂直速度 vz、crash_cooldown、route_s；EntityPool 边界检查） |
| `scenarios/straight_road.json` | 直道导航场景（默认；另有 curve_road/dense_npc/multi_light/oncoming/城市环线/OSM 城市/泊车/驾考科目一~四等共 23 个场景 + `suite.json` 场景矩阵，契约校验 `tools/scenarioctl.py validate`） |
| `modules/adas_nodes/data_recorder_node.c` | 训练样本采集（Learning Loop Stage 0） |
| `modules/adas_nodes/inference_node.cpp` | tiny-MLP 影子推理（Learning Loop Stage 2） |
| `modules/adas_nodes/tiny_mlp.h` | 纯 C 单隐层 MLP 推理内核 |
| `tools/train_demo_model.py` | 一站式训练入口（调度 `tools/train_e2e/` 各 backend） |
| `tools/train_e2e/{train,torch_train,temporal_train}.py` | tiny-MLP / PyTorch / 时序 训练实现 |
| `tools/modelctl.py` | artifact 管理（list / inspect / diff / promote / ota） |
| `docs/LEARNING_LOOP.md` | 车端学习闭环架构 |
| `modules/adas_nodes/flowrec_node.c` | flowrec：配置化 topic 留存节点（见 `docs/FLOWREC.md`） |
| `modules/adas_nodes/manual_drive_node.c` | 终端 WASD 接管 ego（`--manual` 游戏诊断模式） |
| `modules/adas_nodes/bev_detection_node.cpp` | BEV 视觉检测（`bev_pre.c`/`bev_post.c` + ONNX backend） |
| `modules/pem/pem_log.c` | PEM 记录协议（CRC / fsync / 轮转 / 配额），单测 `test_pem_log.c` |
| `modules/pem/pem_runtime.c` | PEM 运行时双流（`monitor_node` 基础设施流 + `pem_collector_node` 业务流） |
| `tools/pem_dump.py` | PEM 解析（`--jsonl --type business`） |
| `tools/scenarioctl.py` | 场景/suite 契约校验（CI `scenario-file-gate`） |
| `tools/opsctl.py` | 运维入口（与 `flowctl` 分工见文件头） |

> 深入教程见 `docs/book/` 目录（23 篇 = `00_preface` + 01~22，分 5 卷：微内核与系统编程 /
> 执行流与高级调度 / ADAS 算法栈 / 仿真验证与学习闭环 / 真车部署，覆盖 OOP in C、插件系统、
> 消息总线、IPC、Bag、Clock、Serializer、State Machine、Discovery、Fusion、Coroutine、
> Demo Evaluator、E2E Learning Loop、Dead Reckoning、SocketCAN Actuator、FlowSIM 场景设计）。
> 索引见 `docs/BOOK.md`；全量文档导航见 `docs/README.md`，代码索引 `docs/CODE_WIKI.md`；
> vis 模块设计见 `docs/VIS_MODULE_GUIDE.md`。

## 构建与运行

```bash
# ── 构建 ────────────────────────────────────────────────────────
bash build.sh release                # Release（默认，等价 cmake --build build）
bash build.sh debug                  # Debug
bash build.sh test                   # 跑 ctest（需先构建）
bash build.sh bench                  # benchmark（进程内总线）+ benchmark_tcp（127.0.0.1 loopback）
./build/bin/benchmark_tcp            # localhost loopback TCP；数字不是 NIC/线缆，也不是进程内总线
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# ── 运行 ────────────────────────────────────────────────────────
bash scripts/demo.sh [duration]           # 启动演示（默认 15s）
bash scripts/demo.sh --no-browser 15      # 不自动开浏览器
./build/bin/flow_launcher config/pipeline.json --duration 30   # 绕过脚本直接起
./build/bin/flowctl list                  # 运行中拓扑/参数/仪表盘
```

`scripts/demo.sh` 常用开关（未列出的见 `scripts/demo.sh` 头部注释）：

| 开关 | 作用 |
|------|------|
| `--scenario <json>` | 指定场景；未指定时 patch 临时 pipeline.json 指向 `DEFAULT_SCENARIO` |
| `--manual` | 游戏诊断模式：基底换成 `pipeline_manual.json`，终端 WASD 直接驾驶 ego |
| `--multi` | fork+exec 多进程模式（默认 dlopen 单进程） |
| `--record` | 录 bag |
| `--replay <bag>` | `flow_launcher --replay-bag` 回放 scene/frame + vehicle/state |
| `--route <id>` / `--start-s <m>` / `--start-d <m>` | 从 route 指定弧长/横向偏移起跑 |
| `--skip-services` | 跳过 flowmond 等旁路服务（配 `FLOW_SKIP_SERVICES=1`） |

环境变量：`FLOWENGINE_SCENARIO`（默认场景）、`FLOW_LOG_DIR`、`FLOW_TOPOLOGY_FILE`
（默认 `/tmp/flow_topology.json`）、`FLOW_LAUNCHER_STDOUT/STDERR`。

### Pipeline profiles（`config/` 是配置驱动启动的唯一真相源）

| 文件 | profile | 用途 |
|------|---------|------|
| `config/pipeline.json` | `default` | 仿真/算法开发/可视化主路径，**CI 与 demo 都用它**（16 节点，不写 PEM） |
| `config/pipeline_car.json` | `hw` | RC 小车真车模板（GPS/IMU/激光雷达/执行器 + `pem_collector`） |
| `config/pipeline_cortex.json` | `experimental` | cortex 变体，启动会打警告 |
| `config/pipeline_manual.json` | `experimental` | `--manual` 游戏诊断模式基底 |
| `config/pipeline_windows.json` | `default` | Windows 原生单进程管线 |
| `config/product.json` | — | 产品身份/安装布局（`schema_version`/`plugin_dir`/`default_vehicle`），非管线 |

`profile != default` 启动会打警告；`topic_contract_check.py` 强制
`pipeline*.json` ↔ `NodePlugin` 的 `s_inputs`/`s_outputs` 对齐。

### 测试

```bash
# CI 等价的快速套件（排除 benchmark/manual/stability/integration 标签）
ctest --test-dir build --output-on-failure --timeout 60 -LE "benchmark|manual|stability|integration"

# 单个测试（-R 按名字正则匹配）
env -u LD_LIBRARY_PATH ctest --test-dir build --output-on-failure -R pem_runtime_smoke
env -u LD_LIBRARY_PATH ctest --test-dir build/modules/adas_nodes --output-on-failure -R pem_log_protocol
env -u LD_LIBRARY_PATH ctest --test-dir build -R vis_coord -V   # -V 看单测内部输出
```

`env -u LD_LIBRARY_PATH` 是必需的：IDE 终端注入的旧 libstdc++ 目录会让二进制启动即报
`version not found`（`scripts/demo.sh` 的 `sanitize_ld_path` 同因）。标签含义：
`benchmark`(性能) / `manual`(需人工) / `stability`(长稳) / `integration`(起真实管线，慢)。

### Windows

Windows 原生单进程插件管线受支持（CI 有 `build-windows-mingw` job）：
`config/pipeline_windows.json` + `scripts/demo.ps1`，交叉编译用
`cmake/mingw-w64-x86_64.cmake`，运行时验收用 `tools/win_runtime_verify.py`。
平台差异统一收在 `include/compat_win/` 与 CMake `if(WIN32)` 分支，**Linux 行为零变化**。

### 游戏诊断模式（`--manual`）

```bash
bash scripts/demo.sh --manual            # 终端 WASD 直接驾驶 ego
```

打开 FlowBoard 左上角「接管车辆」，`WASD`/方向键驾驶；`L/Q/E/H` 灯光测试，
`S`/空格刹车灯，偏离道路按 `R` 或点「回到车道」安全归位。HUD 实时显示转向输入、
车身航向、实际移动方向及三者偏差 —— 这是定位**车轮 kingpin / 死推 / 物理积分
不一致**三类渲染-物理脱节的第一线工具。

仿真约定 `steer > 0` 为左转（ENU heading/y 增大）。游戏模式是**诊断工具**，
不改变规划/控制/安全模块的职责铁律；接管期间 `manual_drive_node.c` 发布 ControlCmd。

### PEM 数据闭环（`profile=hw` / production）

`config/pipeline_car.json` 的 production 模式起**两条 PEM 流**，都具备 CRC、
关键事件 `fsync`、按时间/大小轮转、目录配额保留：

| 流 | 节点 | 记什么 |
|----|------|--------|
| 基础设施流 | `monitor_node` | 系统、topic、health、降级事件 |
| 业务流 | `pem_collector_node` | GPS/定位里程、行驶时长、地区迁移、降级事件 |

```bash
python3 tools/pem_dump.py /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --jsonl --type business /tmp/kunautodrive_pem_business_*.pem
bash scripts/pem_smoke.sh
```

PEM 不是"配置了就相信"：`pem_runtime_smoke` 会真起 production 管线，确认实际写出
并解析回 `trip:ci_simulation`，作为 integration 门禁跑。字段契约/扩展回调/保留策略见
`docs/DATA_CLOSED_LOOP.md`；**默认 `pipeline.json` 不写 PEM**。

> **平台：** Linux（主力/CI）与 macOS 原生均可 `bash scripts/demo.sh` 跑通。
> 平台差异由兼容层 `include/platform_compat.h`（force-include，仅 APPLE 生效）
> + CMake `if(APPLE)` 分支收口，**所有改动都在 `#ifdef __APPLE__` / `if(APPLE)`
> 内，Linux 行为零变化**。macOS 弱化项（仅影响非默认路径）：`--multi` robust-mutex
> 崩溃自愈降级、benchmark 不构建、CAN/I2C dry-run、无线程 CPU 亲和 —— 详见 README。

仪表盘: `http://localhost:8800`
3D 桥接: `ws://localhost:8765`

## 验证

> **架构思想（上百 commit 沉淀）**：每个节点发布 debug JSON topic →
> monitor 聚合到唯一公共数据源 `/tmp/flow_topology.json` → 仪表盘 / 评估器 /
> trace_incident / flowctl 全部消费同一份数据，**黑盒评分 + 分层验证 + 门禁
> 有效性自检**。完整方法论见 `~/.claude/skills/verification/SKILL.md`。

```bash
# 分层验证阶梯（改完按此走）：
python3 tools/pipeline_check.py                      # L0 秒级离线：管道断没断
python3 ci/evaluators/demo_evaluator.py --duration 45 --interval 0.5  # L1 45s 行为回归
python3 ci/evaluators/scenario_regression.py --baseline               # L1.5 场景矩阵（改前 --update-baseline）
python3 ci/evaluators/test_param_regression.py                        # L2 调参 A/B（改前 --save-baseline）
python3 tools/quick_verify.py                       # L3 交互热调参（set/eval）
python3 tools/auto_tune_mpc.py                      # L3 自动扫参/自标定
python3 tools/trace_incident.py                     # 事故逐层追溯（碰撞/出路沿复盘）
```

评估器采样 `/tmp/flow_topology.json`，自动检查：拓扑完整性、topic 频率、碰撞、路沿偏离、停滞、变道次数、偏航抖动、NPC 瞬移。WARN 是已知问题可忽略，FAIL 必须修复。
门禁有效性由 liveness gate（死信号 FAIL）+ require（无法判定≠通过）+ test_evaluator_gate.py（门禁自测）兜底——**门禁抓不住已知故障 = 它的 PASS 不可信**。

**frontend 改动另走 `npm run vis:check:all`（6 个门禁），C 链门禁覆盖不到它。**
CI 的 C 侧 gate 全量在 `.github/workflows/ci.yml`：`scenario-file-gate`（`tools/scenarioctl.py validate`）、
`clock-service-gate`（禁 `modules/` 裸 `clock_gettime`）、`topic-contract-gate`、
`sensor-wiring-gate`、`zombie-ban-gate`、`map-connectivity-gate`、`build-release`/`build-asan`（ctest）、
`build-windows-mingw`（交叉编译）、`integration-test`。本地复现任一 gate 直接抄它的 `run:` 行。

另有两条**只跑仿真**的等价门禁（在 `long-tests` job 内，需已构建）：
- 默认编排（ground_truth 直通）：`python3 ci/evaluators/scenario_regression.py --baseline`
- **sensor 编排（真点云链路）**：`FLOW_PIPELINE=config/pipeline_sensor.json python3 ci/evaluators/scenario_regression.py --workers 1 --only <场景>`
  （CI 里对 urban_challenge / dense_npc / lane_change_traffic 三个场景各跑一次；不传 `--baseline`——
  sensor 编排与 ground_truth 基线的行为差异不算回归，它只断言各场景自身门禁 PASS）

## 编码规范（统一 API — 2026-07 重构后强制执行）

> 以下 API 是本项目的**唯一合法入口**。禁止绕过它们直接调用底层函数。

### JSON 序列化/反序列化 → `cjson/cJSON.h`

```c
#include <cjson/cJSON.h>

// 序列化（替代手拼 snprintf）
cJSON* root = cJSON_CreateObject();
cJSON_AddNumberToObject(root, "speed", v);
cJSON_AddStringToObject(root, "mode", "highway");
char* s = cJSON_PrintUnformatted(root);   // topic publish 用 compact
// char* s = cJSON_Print(root);           // 文件写入用 formatted
transport_publish(t, "topic", (const uint8_t*)s, (uint32_t)strlen(s) + 1);
free(s);
cJSON_Delete(root);

// 反序列化（替代 strstr + sscanf）
cJSON* root = cJSON_Parse((const char*)msg->data);
if (root) {
    cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "speed");
    if (cJSON_IsNumber(j)) double v = j->valuedouble;
    cJSON_Delete(root);
}
```

- ❌ **禁止** `snprintf("{\"key\":%d}", ...)` 手拼 JSON
- ❌ **禁止** `strstr(msg, "\"key\":")` + `sscanf` 手写解析
- ✅ **必须** 用 cJSON 序列化/反序列化所有 topic 消息和文件
- 参考：`src/core/scenario_loader.c`（已有成熟 pattern）

### 时间 → `clock_service.h`

```c
#include "clock_service.h"

uint64_t now = clock_now_us();               // CLOCK_MONOTONIC，仿真/回放模式下可注入
uint64_t real = clock_now_realtime_us();     // CLOCK_REALTIME，始终真实墙钟（训练样本、仪表盘）
```

- ❌ **禁止** `static uint64_t monotonic_us(void) { clock_gettime(...); }` 重复造轮子
- ❌ **禁止** `clock_gettime(CLOCK_MONOTONIC, &ts)` 裸调用
- ✅ **必须** 用 `clock_now_us()`；需绝对时间用 `clock_now_realtime_us()`

### 参数解析 → cJSON_Parse

```c
// 替代 strstr(params_json, "\"target_speed\":")
cJSON* p = cJSON_Parse(params_json);
cJSON* j = cJSON_GetObjectItemCaseSensitive(p, "target_speed");
if (cJSON_IsNumber(j)) cfg.target_speed = j->valuedouble;
cJSON_Delete(p);
```

- ❌ **禁止** `strstr(params_json, "\"param_name\":")` + `sscanf` 手写参数解析
- ✅ **必须** 用 `cJSON_Parse(params_json)` 解析所有节点参数

### 调参 → `flowctl param set`（不要改常量重编译）

```bash
flowctl param list                              # 运行中进程的实时参数
flowctl param get control.mpc_r_ddelta
flowctl param set control.mpc_r_ddelta 2.0      # 下一帧生效，不用重启
```

新增一个可调参数，**三处都要通，只做注册等于没做**：

1. `params_json` 里加 `cJSON_GetObjectItemCaseSensitive` 解析分支
   （漏了这步，pipeline.json 里那行就是死字符串）
2. `param_register_*` 的默认值用 `g.<字段>` 而非硬编码字面量
   （用字面量会把上一步解析到的值盖掉）
3. 逐帧 tick 里 `param_get_float` 重读
   （漏了这步，注册了也改不动，只能重启）

- ❌ **禁止** 为了试一个值去改代码常量重新编译
- ✅ 整段 run 的聚合指标 A/B（avg_speed / flip_rate）用 `tools/auto_tune_mpc.py` 或 `ci/evaluators/scenario_regression.py`，
  它每个取值重启一次是必需的 —— 要可比就得从 x=0 起跑干净的 run

### 节点线程 → `node_pump()`

```c
flowcoro::rt::RtExecutor ex{{ .pin_cpu=-1 }};
ex.spawn(ct.run(), "node_name");
node_pump(ex, [] { return (bool)g.should_stop; });
ex.shutdown();
```

- ❌ **禁止** 裸 `while (!g.should_stop) ex.run();` —— `run()` 是非阻塞 tick，
  这样写 100% 忙等自旋占满一个核
- ❌ **禁止** 靠 `RtExecutor::Config` 的 `idle_sleep_us` 限速 —— 它只被
  `run_blocking()` 读取，而 `run_blocking()` 零调用者

### 重构/替代 → 同一 commit 删旧

- 任何「重写 / v2 / 换实现」类改动，新实现与被替代的旧实现必须在**同一个 commit** 内，旧的物理删除
- ❌ 禁止 `_v2` / `_v3` / `_new` / `_old` / `_bak` 后缀文件并存超过 1 个 commit
- ❌ 禁止新增与现有入口同名不同实现的第二份脚本（如已有 `tools/train_e2e/train.py`，禁止再添加第二个 `tools/train/train.py` 做相同事情——必须删旧）
- ✅ 确实要暂留旧路径 → 文件头第一行加注释：
  ```python
  # @deprecated superseded-by=tools/train_e2e/train.py remove-by=2026-08-21
  ```
  ```c
  // @deprecated superseded-by=modules/adas_nodes/inference_node.cpp remove-by=2026-08-21
  ```
  无 `remove-by` 日期视为违规。

可 grep 性：
- `git diff HEAD~1 --name-only | grep -E '_v[0-9]+\.|_new\.|_old\.|_bak\.'` 出现多个同名前缀 → 违规
- `git grep -l "@deprecated" -- ':!*.md' | xargs -I{} grep -L "remove-by" {}` → 缺日期违规
- 任何重构类 commit 的 `git show --stat HEAD` 必须显示 `D <旧文件>` 至少一行

### 错误码 → `error_codes.h`（modules 中待推广）

```c
#include <error_codes.h>
return ERR_INVALID_PARAM;  // 替代 return -1
```

### 违反以上规范的代码不会被合并。

## Python 仿真先行（算法升级必走流程）

> 任何算法模块升级（控制/规划/感知/行为决策），**必须先 Python 仿真验证**，
> 再移植到 C++。禁止直接写 C++ 代码靠编译→跑 demo 迭代——那太慢了。

### 为什么

| 对比维度 | Python 仿真 | C++ demo 跑 |
|---------|------------|-------------|
| 单次迭代 | ~0.1s（纯 Python 标准库，无编译） | ~30s（编译+启动+45s 评估） |
| 参数扫描 | 自动扫千组，`--tune-*` 搜最优 | 靠手改跑几十次，肉眼对比 |
| 场景覆盖 | 6 场景一键 `--run-all`，~3s 出结果 | 每个场景改配置重跑 |
| 故障隔离 | 只测模块自身，无上下游干扰 | 全链路噪音，难定位根因 |
| 可复现性 | 确定性仿真，完全一致 | 随机种子/线程调度导致波动 |

### 工具

| 工具 | 用途 |
|------|------|
| `tools/control_sim.py` | 横向/纵向控制 + 6 场景仿真（Stanley/MPC 控制器） |
| `--scene-*` | 单场景执行（curve/emergency/stop_go/obstacle/merge/cutin） |
| `--run-all` | 一键全量 6 场景验证 |
| `--tune-*` | 自动参数扫描找最优（curve/emergency/stop_go/merge/cutin） |
| `--uturn` / `--tune-uturn` | 掉头仿真 + 参数扫描 |

### 6 场景覆盖

- **curve** — 弯道跟随（常曲率，测试 Stanley/MPC 弯道跟踪精度）
- **emergency** — 紧急制动（前方固定障碍物，测试纵向刹停）
- **stop_go** — 跟停再起步（前车停→走，ACC 跟车）
- **obstacle** — 障碍物避让（变道绕行，测试横向+纵向协同）
- **merge** — 匝道汇入（加速车道汇入主路，测试速度匹配+横向融合）
- **cutin** — 加塞处理（旁车突然切入，TTC 预警+减速避让）

### 工作流

```
[Python 仿真]                  [C++ 移植]
    │                              │
    ▼                              │
  --run-all 全量 PASS              │
    │                              │
    ▼                              │
  --tune-* 扫描最优参数            │
    │                              │
    ▼                              │
  确定控制器类型/参数               │
    │                              │
    └─────── 移植到 C++ ──────────►│
                                   │
                                   ▼
                           编译 → 跑 demo
                            │
                            ▼
                         demo_evaluator.py
                            │
                            ▼
                         pipeline_check.py
```

### 故障排查

| 现象 | 根因 | 排查方法 |
|------|------|----------|
| Python 仿真 PASS 但 C++ 无效 | 物理模型不一致（step_bicycle 积分参数/执行器延迟/饱和限幅差异） | 逐帧对比 Python vs C++ 的 steer/v/heading 轨迹 |
| 参数扫描全 FAIL | 控制器类型错误（Stanley 参数下 MPC 不可用） | 检查 `--mpc` / `stanley_params` 参数传递 |
| 弯道场景 FAIL | 横向偏移计算错误（x 近似 s 不对） | 确认 `run_curve_scenario` 用圆心距离法 |
| 紧急制动 FAIL | 制动距离过短/速度过快 | 减速速 20m/s 需 60m+ 制动距离；加提前量 |
| 多场景不稳定 | 同一组参数不能同时满足所有场景 | 取交集或引入场景自适应参数切换 |

### 必须遵守

- ❌ **禁止** 任何算法模块升级直接写 C++ 代码
- ✅ **必须** 先 `--run-all` 全量 PASS，再 `--tune-*` 扫最优参数，最后移植到 C++
- ✅ 移植后必须跑 `demo_evaluator.py` 验证 C++ 行为与 Python 仿真一致
- ✅ 新增场景必须注册到 `--run-all` 并记录到本 skill

### 违反以上规范的代码不会被合并。

## 3D 渲染门禁（与 C 侧对称 — 2026-07 重建）

> C 链路有 `demo_evaluator.py` 运行时门禁（碰撞/频率/拓扑回归），3D 渲染
> 此前零等价物。以下门禁补上这条缺口，防止"undefined ref / 漏括号 / 漏调用"
> 类回归反复复发。

### 可执行门禁：`npm run vis:check`

```
npm run vis:check
```

等价于顺序执行：

1. **全量模块加载** `node --import ./tests/support/three-preload.mjs tests/vis_module_load.test.mjs`
   — 逐一 import 每个 `js/vis/**` 模块，抓语法错、顶层 ReferenceError、import 路径错误
2. **ESLint no-undef** `npx eslint -c tools/flowboard/eslint.config.mjs tools/flowboard/js/vis/`
   — no-undef 为 error（不通过阻断合并），no-unused-vars 为 warn（标出"定义了但未调用的函数"）
3. **单帧 tick 冒烟** `node --import ./tests/support/three-preload.mjs tests/vis_render_tick.test.mjs`
   — 用 THREE shim 构 SceneDirector，喂 3 帧（平路/高架/多车道），各调一次 tickAnimation()，任何抛错即 FAIL

- ❌ 任何 `tools/flowboard/**` 改动，`npm run vis:check` 红了**不合并**
- ❌ 禁止在无渲染门禁覆盖时对 flowboard 做结构性重构（拆层/换架构）；结构重构与覆盖它的 gate 必须**同 commit**
- ✅ 故障模式表里的 3D 条目，凡能转成 tick 冒烟用例的**必须转成测试**，禁止让文档替代测试

### 门禁覆盖范围

| 门禁 | 覆盖率 | 抓什么 |
|------|--------|--------|
| `vis_module_load.test.mjs` | 全部 `js/vis/**/*.js`（约 50 个模块） | 语法错、顶层 ReferenceError、import 路径 |
| `eslint no-undef` | 全部 `js/vis/` | 未定义变量引用（如 `VIADUCT_VIS_LENGTH` 未导入） |
| `eslint no-unused-vars` | 全部 `js/vis/` | 定义了但未调用的函数（如 `followEgo` 漏调） |
| `vis_render_tick.test.mjs` | director + 全部 view | tickAnimation 运行时抛错、store 数据完整性 |
| `vis:check:junction` / `vis:audit` / `vis:check:smoke` | 路口标线、路线审计、渲染冒烟 | 路口分叉对齐、标线类型错、渲染链路断裂 |

### 与 C 侧门禁的对称性

```
C 侧: 改 pipeline 节点 → demo_evaluator.py (45s 真跑) → FAIL 阻断
3D 侧: 改 flowboard/** → npm run vis:check (~3s) → FAIL 阻断
```

### 坐标约定门禁（有牙，可 grep — 2026-07 收敛）

> 所有位置/朝向/高度/尺度转换的**唯一合法入口**是 `js/vis/math/Coord.js` 的纯函数。
> 禁止在任何 view 里手写裸 -y 翻转、裸 atan2 求朝向、裸 position.set 配魔法数。

#### 唯一合法纯函数

| 函数 | 替代 | 用途 |
|------|------|------|
| `worldToThree(x,y,z)` | 手写 `[x, z, -y]` | ENU→THREE 坐标映射 |
| `headingToRotationY(h)` | 裸赋值 `rotation.y = h` | heading→THREE rotation.y |
| `directionToRotationY(dx,dz)` | 裸 `Math.atan2(dx,dz)` | 2D 方向→rotation.y |
| `forwardENU(heading)` | 裸 `Math.cos(h), Math.sin(h)` | heading→ENU 单位前向向量 |
| `offsetAlongNormal(px,pz,nx,nz,d)` | 裸 `px + nx*d` | 中心线法线偏移 |
| `tangentToNormal(tx,tz)` | 裸 `l=sqrt(...); -tz/l, tx/l` | 切线→单位法线 |
| `placeOnRoad(spine,s,lateralOffset)` | 手算插值+法线偏移 | 沿路参数→{pos,rotY,height} |

#### grep 强制违规检测

```
npm run vis:check:grep
```

等价于 `node tests/vis_grep_enforce.mjs`，扫描 `js/vis/view/` 目录：

- 裸 `z: -(n[2])` → 应走 `worldToThree`
- 裸 `Math.atan2(...)` → 应走 `directionToRotationY`
- 裸 `Math.sin/Math.cos`（坐标计算用）→ 应走 `forwardENU` / `offsetAlongNormal`
- 裸 `.position.set(...)` 配魔法数 → 应走 `placeOnRoad`

命中即 FAIL（注释豁免：行内含 `Coord.` 或 `// Coord` 注释）。

#### 纯函数 property-test

```
npm run vis:check:invariant
```

等价于 `node --import ./tests/support/three-preload.mjs tests/vis_coord_property.test.mjs`，验证：

- `worldToThree`: ENU +y(北) → THREE -z 轴映射 golden 表
- `headingToRotationY`: heading=0(东/+x)→车头 forward 指 +x
- `forwardENU`: 单位向量长度=1，方向正确
- `directionToRotationY` 与 `headingToRotationY` 一致性
- `tangentToNormal`: 法线与切线正交，单位长度
- `placeOnRoad`: 任意 s，|height−groundHeightAt(s)|<ε（永不浮空/埋地）；|lateral|≤半路宽⇒点在路面内

#### 全量门禁

```
npm run vis:check:all
```

等价于 6 个门禁全绿：`vis:check`（模块加载 + ESLint + tick 冒烟）、
`vis:check:invariant`（坐标 property-test）、`vis:check:grep`（裸数学检测）、
`vis:check:junction`（路口标线/分叉对齐/标线类型）、`vis:audit`（路线审计）、
`vis:check:smoke`（渲染冒烟）。改前端只想快跑时 `npm run vis:check` 是子集，**合并前必须全量**。

- ❌ 坐标/朝向/高度/尺度只准走 `Coord.*` / `placeOnRoad` 纯函数；view 里裸 `-y`/`atan2`/`position.set`+魔法数 = 违规
- ❌ 每个坐标纯函数必须有 property-test
- ❌ flowboard 改动跑 `npm run vis:check:all` 必须绿；几何变更同 commit 更新 golden + PR 附截图
- ✅ 残余诚实说："低/丑/材质光照"这种纯观感 invariant 测不了，只能人 diff（puppeteer 无头跑一帧存图，人肉审 golden 图）。但"位置关系"——功能性几何错——100% 可数值化。收敛约定 + invariant，是把"黑屏后人肉手修"变成"提交前红灯拦下"。

### 违反以上规范的代码不会被合并。

## NPC 智能 — IDM + 边界权限门（2026-07）

> NPC 行为使用「IDM 纵向跟车 + 边界权限门」。

### 架构

```
每帧 NPC tick:
  1. 找同车道前车 (find_lead) → IDM 期望速度
  2. 纵向积分 + 位置更新（Frenet 车道跟随 + road_pos 推进）
  3. 红绿灯/让行期间停车（StopForTL/Yield 状态保持当前车道）
```

### MOBIL 变道

MOBIL 变道已禁用（`#if 0`）。完整代码保留在 `npc_ai.cpp` 中作为参考，可通过将 `#if 0` 改为 `#if 1` 重新启用。当前演示场景中 NPC 各守其道不变道。

| 参数 | 默认值 | 含义 |
|------|--------|------|
| IDM 跟车 | base=5.0, time=1.5 | 安全间距 = base + v × time |
| 加速/减速 | 1.5 / 3.5 m/s² | 平稳加减速 |

### 关键文件

| 文件 | 作用 |
|------|------|
| `modules/adas_nodes/flowsim/npc_ai.h` | NPC AI 配置（NpcAiConfig）+ API |
| `modules/adas_nodes/flowsim/npc_ai.cpp` | IDM + MOBIL + 边界权限门实现 |
| `modules/adas_nodes/flowsim/scene_events.cpp` | 红绿灯/ETC 事件调度 + NPC 响应 |
| `modules/adas_nodes/flowsim/route.cpp` | 中央有序 route（NPC 车道跟随骨架） |

## 仿真基础层 — Digest + Invariant（2026-07）

> 静态/动态 digest 编码空间关系为数值，invariant 断言在提交前红灯拦下功能性几何错。

### 帧契约

```
frame: THREE  | up: +Y | 单位: m | ENU→THREE: [x, z, -y] | ego_centered: true/false
```

### 静态 digest

几何变更时（road network 加载后）建一次，包含：
- 车道（id、中线采样点、宽度、边界类型、行驶方向、限速、s 范围）
- 车道线（虚线段/实线/双黄，位置+段长+间距）
- 可行驶区（路面多边形/路沿 polyline）
- 红绿灯（位置、朝向、管辖车道、相位）

### 动态 digest

每帧 dump ego + 每个 NPC：
- pos [x,y,z]、bbox [L,W,H]、heading、vel [vx,vy]、speed
- yaw_rate、accel、lane_id、lateral_offset、s、rotationY

### Invariant 检查

| 类型 | 检查项 | 抓的 bug |
|------|--------|----------|
| 静态 | 车道宽 ∈ [2.5,4.0]m | 车道宽度配置错 |
| 静态 | 边界类型自洽（同向分隔=虚线、对向=双黄/实线、外沿=实线） | 边界类型配置错 |
| 静态 | 虚线段长 ~3m、间距 ~6–9m | 标线参数错 |
| 静态 | 可行驶区闭合 | 多边形不闭合 |
| 静态 | 路面高程连续 | 高度阶跃跳变 |
| 静态 | 红绿灯朝向 · 车道方向 < 0 | 红绿灯背对来车 |
| 静态 | 没有物体堆在 (0,0,0) | 路网未初始化 |
| 静态 | 每条 lane 中线落在可行驶多边形内 | 可行驶区与车道不一致 |
| 空间 | \|z − roadHeight(x,y)\| < ε | 浮空/埋地 |
| 空间 | \|lateral_offset\| ≤ 半路宽 | 飞出路面 |
| 空间 | rotationY == headingToRotationY(heading) | ENU→THREE 符号翻错 |
| 空间 | 0 ≤ speed ≤ 1.5×限速 | 超速/呆滞 |
| 空间 | bbox ≈ 标准尺寸 | 尺度错 |
| 空间 | 两 actor bbox 不重叠 | 穿模/重叠 |
| 运动方向 | dot(forward, vel) > cos(30°) | 车头≠前进方向（横着/倒着开） |
| 运动方向 | dot(forward, lane_dir) > cos(45°) | 与车道方向不一致 |
| 时序 | Δpos ≈ vel × dt | 瞬移/teleport |
| 时序 | \|Δpos\| ≤ v_max × dt | 超速瞬移 |
| 时序 | \|Δheading\| ≤ yaw_max × dt | 朝向瞬变 |
| 时序 | accel ∈ [−8, +4] m/s² | 运动学不可行 |

### 关键文件

| 文件 | 作用 |
|------|------|
| `modules/adas_nodes/flowsim/sim_digest.h` | Digest 数据结构 + invariant API |
| `modules/adas_nodes/flowsim/sim_digest.cpp` | 实现：digest 生成 + invariant 检查 |
| `modules/adas_nodes/flowsim_node.cpp` | 集成点：静态 digest 建一次，动态 digest + invariant 每 20 帧 |

### 违反以上规范的代码不会被合并。

## 常见故障模式

| 现象 | 根因 | 位置 |
|------|------|------|
| 仪表盘/3D 一直 "Waiting for data"，curl 却有数据 | 仪表盘 JSON 是 cJSON_Print 多行格式，SSE 单 `data:` 帧发送被 EventSource 按行丢弃，浏览器只收到 45 字节前缀。已在发送前压平为单行，详见 [排查文档](docs/TROUBLESHOOTING_3D_DASHBOARD.md) | `monitor_server.c` handle_sse |
| 3D 场景整屏黑（curl 有数据、console 报 `Unexpected token 'export'`） | MVC 重构（c5e4ba9）拆 Controller 层时 `_renderFrame` 相机块漏闭合一个 `}`，scene3d.js 顶层 `export` 被当块内语句、整模块编译失败不执行 → `init3DScene` 未导出。已补回 | `scene3d.js:2159` 附近 |
| 仪表盘所有请求挂死（端口在监听） | 终端对前台 demo.sh 按了 Ctrl+Z，整个进程组 `T (stopped)`。Ctrl+C 结束或后台运行 | `scripts/demo.sh` |
| 车速降到 0 后永久卡死 | ROAD_GUARD 低速恢复条件要求 `|y|>=road_center_limit`，但车可在任意 `2.1<|y|<2.5` 停下。改为只要 `speed<2.5` 就给小油门 | `control_node.cpp:534` |
| 红灯停稳后灯转绿也不走（评估器双峰：要么跑完 x≈306，要么卡死 x≈180） | planning 用 `spd_out[0]`（≈当前车速）覆盖 `command_speed`，停稳后 `v=0 → target=0 → 油门=0 → v=0` 自维持闭锁；TL override 只置 0 从不恢复，解不开。改为取 max | `planning_node.cpp:787` |
| MPC 输出每帧翻符号（bang-bang），调 `r_ddelta` 无效 | 求解器内部 `max_steer=0.35` 而外部限幅 ≈0.027，差 12.9 倍。解从不触及自身约束边界，平滑项全部失效。改为 solve 前 `mpc_set_max_steer()` 注入真实限幅 | `control_node.cpp:1372` |
| 8 个节点线程各占满一个核 | 裸 `while(!stop) ex.run();` 忙等；`idle_sleep_us` 只被零调用者的 `run_blocking()` 读取。改用 `node_pump()` | `coroutine_task.h` node_pump |
| 变道冲出车道 | Stanley heading 阻尼硬编码 0.5，pipeline.json 的 `lat_kd_heading` 未生效 | `control_node.cpp:548` |
| NPC 瞬移 | 障碍物回收逻辑放入 100m 外（设计如此，非 bug） | `flowsim/npc_ai.cpp:204` |
| NPC/车飞出路面、不在车道上、坐标飞到几千米外 | flowsim NPC 用 `step_bicycle(steer=0)` 世界系直线积分、不跟道路几何，路一拐弯就直线冲出路网。已改为中央 `Route`（把各 road 连成有序主路）+ Frenet 沿车道推进 + 到头回收 | `npc_ai.cpp` step_npc_vehicle / `flowsim/route.cpp` |
| 感知降频 | DBSCAN 点云过多时聚类耗时超过 deadline | `perception_node.cpp` |
| 车身左右晃动（1-2Hz 极限环） | 历史根因：`road_pos.world()` 每帧把 ego.heading 重置为道路切线，control 的 `v_lat_damp` 失效（heading_err≈0），退化为纯 P。**96447a9 起已改为两模式都自由积分 heading**：运动学模式由 step_bicycle 积分，靠 `sin(dh)` 负反馈闭环 + cte/heading 项 + 低通 + 死区稳住（曾尝试自由积分导致斜行后回滚，后加 sin(dh) 反馈再启用）；动力学模式由轮胎侧偏力积分。故 `is_dynamic` 分支对两模式一视同仁，只做 heading 归一化 | `flowsim_node.cpp` 主循环 ego 段 |
| steer 打到 0.25 硬限幅导致抖动 | 运动学自行车模型下 heading 漂移可达 0.8 rad，steer 限幅过紧导致控制器累积误差撞 clamp。修复：`lc_lat_accel_max` 从 2.4→4.5，`steer_min_clamp` 从 0.016→0.030 | `control_node.cpp:1245-1253` `pipeline.json:198` |
| 内部巡航 fallback 输出大 steer | `internal_cruise_control` 用 `road_h - heading` 全量前馈，运动学模型下 heading 漂移可达 0.8 rad，公式输出 0.8 被 clamp 到 0.25。修复：改用 `heading_err*0.3 + yaw_damp + lat_err*0.03`，cap 降到 0.15 | `flowsim_node.cpp:1007-1027` |
| 控制遇到慢车不减速、油门全开撞前车 | 积分饱和 anti-windup 逻辑错误：刹车分支 `g.integral < 0` 应为 `g.integral > 0`，`-= error*dt` 应为 `+=`。加速阶段积分累积到 +500，进入减速后 P term 不足以抵消 I term，油门全开撞车。修复：error 翻负时直接清零正积分 | `control_node.cpp:550-554` |
| behavior planner 一直不进 FOLLOW | `worthwhile = blocked && (best_gap < min_gap)` 是反逻辑——等 gap 小于 15.6m 才觉得"值得超车"。高速 8m/s 接近速度下只剩 ~2s，变道来不及。修复：改为 `best_gap > min_gap`，阈值提至 base=25m，mult=2.0 | `behavior_planner_node.cpp:516` |
| 管道检查 topics 列表缺 perception/obstacles | `monitor_node.c` 的 `TopicStats tstats[16]` 只能装 16 个 topic，第 17 个静默丢弃。扩到 64 | `monitor_node.c:647` |
| HTTP 返回 JSON 在 64KB 被截断 | `monitor_server.c` 的 `MONITOR_HTTP_BUF_SIZE 65536` 不够装含 samples 的完整拓扑 JSON。扩到 131072 (128KB) | `monitor_server.c:46` |
| 转向灯左右颠倒（变道打右灯亮左边/打左灯亮右边） | 车辆模型所有 `*_L` 件放在 z=+0.82（THREE 右手系车头朝 +X 时 +z=几何右），`*_R` 在 z=-0.82；前端 `_setVehicleLights` 按名字点 FL/RL → 左灯请求点亮几何右灯。修复：gen_models.py 全部 L/R 件 z 互换 + 重生成 gltf + `--validate` 对称性门禁（生成物中心 z 符号必须与名字一致，防复发） | `tools/flowboard/gen_models.py`（L/R 部件 z 号） |
| 变道减速甚至全刹、超车没意义 | behavior 变道中 P5 分支 `else if (blocked) target_speed = lead_speed`：变道转移首帧 blocked=1 且前车停着（等红灯）→ target=0，后续 blocked=0 帧无分支重置 → 锁死 → planning command_speed=0 → 全刹（实测 spd=10.7→0.0 brk=1.00）。修复：删除该分支，防追尾改由 planning TTC + safety_control 近场 TTC 兜底 | `behavior_planner_node.cpp` 变道分支 |
| 超车/归位后立刻在红灯前刹停（无效变道） | 归位/超车决策不看目标车道前方红绿灯，切回内侧道（灯只管辖 y_lane=-1.75）即刹停。修复：behavior 订阅 road/traffic_lights，`lane_ahead_stop_light()` 检查目标车道前方 60m 内非绿灯则不归位/不变入 | `behavior_planner_node.cpp` `lane_ahead_stop_light` |
| 多车道高速上遇对向车刹停到 0（会车让行过度保守） | planning 会车让行 + safety_control 对向 TTC 都用 `\|dy\|>2.0` 判"对向车"，把对向**任意车道**（含 2+ 车道外，横向 10.5m）都当迎头威胁 → 巡航压到 0.4×~1.0 全刹。修复：两者都加横向相邻上界（planning `1.5×路宽`、safety `6.0m`），只把相邻车道的对向车当真威胁 | `planning_node.cpp` 会车让行 / `safety_control_node.cpp` `min_oncoming_ttc` |
| 改了模型/JS 但浏览器还显示旧效果（转向灯仍反） | `monitor_server.c` 把 `/tools/flowboard/models/*` 标 `Cache-Control: immutable`（1 年），模型文件改后浏览器永远用旧缓存不重拉。修复：models/ 改 `no-cache` + 前端模型 URL 加 `?v=` 缓存破坏版本号（改了模型就 bump） | `src/core/monitor_server.c` `cache_control_for_path` / `tools/flowboard/js/models.js` 模型 URL |
| 红灯不停直接闯（planning override 设 0 无效） | planning 红灯 override 在 `spd_out` 生成**之后**才设 `command_speed=0`，但轨迹 `points[].v` 读的是旧 `spd_out`（`spd_out = v0*(1-t)+command_speed*t` 已按巡航速度算完）→ control 拿到的还是巡航速度。一直被"跟停红灯前停着的车"掩盖。修复：override 触发时**同步重建 spd_out**（当前速度→0 减速斜坡），轨迹末点归零 control 才真正刹停 | `planning_node.cpp` 红灯 override（~line 1209） |
| 变道纠结：超车后立刻归位回慢车道、再超再归位（必现） | 归位只查目标车道 gap（空/远），不查目标车道**车速**——超车到快车道后切回慢车道，立刻又被慢车堵、再变道再归位，120s 必现 3 个来回。修复：归位加"目标车道可用"条件——空旷（无前车）或前车速度 ≥ 0.7×巡航才归位，否则留在快车道巡航 | `behavior_planner_node.cpp` 归位分支（~line 774） |
| 右转/支路场景 control 跟踪坏轨迹（偏出路沿） | flowsim ref_path 用 lane centerline（含当前车道偏移）→ planning 再加 target_lane_offset → 双重偏移。修复：ref_path 优先用 route centerline（d=0=道路中心），planning 用 project_to_reference_path 把 ego 投影到 map_ref 参考线 Frenet 弧长，control 直接取 trajectory 前视点的全局 y 替代 road_center + lane_d 混拼 | `flowsim_node.cpp` publish_ref_path / `planning_node.cpp` project_to_reference_path / `control_node.cpp` target_path_y |
| 掉头返程方向误判（travel_dir 反复翻转、route 步骤重放把 ego 拽离对向车道绕圈） | navigation 用 dx/heading 猜测行进方向：三把方向掉头（2s+3s+0.5s）中 heading 扫过 ±x 两个半平面各驻留 >0.8s，8 帧去抖压不住 → travel_dir 中途翻 +1/-1 → next_idx 归零重放 enter_noa/prepare_u_turn、A* hop 把返程中的 ego 拽回前进车道。修复：**方向唯一事实源 = flowsim 权威 `road/ref_path.reverse`**（`u_turn_active` 掉头 finalize 置位、整段返程保持、永不抖动），navigation `on_ref_path→on_return` 据此翻 travel_dir，behavior 返程锁 committed_lane 抑制变道，control target_lane_center 乘 cos(ref_road_heading) 返程符号自洽 | `flowsim_node.cpp` publish_ref_path（reverse 字段）/ `navigation_node.c` on_ref_path / `behavior_planner_node.cpp` on_return / `control_node.cpp` target_lane_center |
| 算法评估挂死（demo_evaluator/scenario_regression 无输出不结束） | 三层因果：① demo.sh 行为监控管道 `{ tail -F \| grep } &` 的 EXIT trap 只 kill 子 shell，tail/grep 孙子泄漏（tail -F 永不退出）；② f412132 日志改到 `$LOG_DIR/launcher.log` 后泄漏 tail 命令行不再匹配启动清扫 `pkill -9 -f flow_launcher`（旧路径含该串会被顺带清理 → 旧版自愈）；③ 泄漏 grep 继承 fd2=评估器捕获管道写端 → `proc.stdout.read()` 等 EOF 永不返回。修复：tail 加 `--pid=$$`（GNU，BSD 降级）+ 启动清扫补 pkill、评估器 read() 改 select 限时读取、SIGTERM/INT killpg 防孤儿、scenario_regression 加超时 | `scripts/demo.sh` tail 块 / `ci/evaluators/demo_evaluator.py` / `ci/evaluators/scenario_regression.py` |
| 掉头时车辆屁股横移/甩动（3D 观感，物理模型本身正确） | 后端 step_bicycle 中心参考是对的（含 half_wb·yaw_rate 切向项），但前端死推算 `_advanceState` 外推只用 `speed·(cos,sin)`，丢切向项（掉头时切向速度 ≈1.2m/s = 车速 34%），SSE 5Hz 下每包中心横向偏 ~0.23m → 渲染"中心直线漂移 + 车身旋转"解耦 = 屁股横移。修复：`updateDeadReckon`/`updateEntityDeadReckon` 喂世界系 vx/vy（step_bicycle 已含切向项），`_advanceState` 有 vx/vy 时按世界速度外推，无则回退旧公式；配套收严 UTurnPlanner Phase 4 退出容差 0.25→0.10 rad 减少返程残差、planning wheelbase 2.8→2.7 与 physics 一致、off_rails 退出日志加 fold/lat 残差字段 | `tools/flowboard/js/vis/core/DeadReckon.js` _advanceState / `app.js` sync2DTarget / `SceneDirector.js` updateEntityDeadReckon / `planning_node.cpp` Phase 4 / `flowsim_node.cpp` [OFFRAILS] exit 日志 |
| S 弯不跟弯（curve_road 场景车沿 y=-1.75 直开，heading 恒 0） | **四层连环**（2026-08-04 排查）：① `json_to_xodr.py` roads_from_road_network 只认 curvature_profile/length_m，完全忽略 `road_network.edges[].nodes` → 生成直道 XODR → ref_path 全 y=0；② 即便 XODR 弯了，control 横向目标 `target_path_y` 是轨迹 0.5s 前视点绝对 y，弯道上该点比 ego 当前位置高 → lat_error 虚高 → 车往弯内侧漂 ~3m；③ `scene_pub.cpp` `ROAD_NODES_PER_EDGE=8` 固定采样 → 前端 CR 过 8 点严重过冲、评估器弦长偏离真值 ~14m → 车在车道里被判 road departure；④ demo_evaluator 逆行/横向摆动检查假设直路（y<0 朝东 / 绝对 y 范围<4.5m），S 弯 ego y 合法扫过 ±103 却 heading 恒朝东 → 误报 WRONG-WAY/lateral excursion。修复：nodes 折线 → 三次 Hermite（端点切线 = 相邻 chord 平均，**不用 CR**——coarse 节点 CR 过冲生成 R≈19m 发卡弯，a_lat≈21m/s² 拐不过来；Hermite min R≈546m 可跟）密采样 5m → 逐段 line；planning map_ref 分支 kappa 从恒 0 改为切线中心差分恢复前馈；control 横向目标改用 query_ref_at **本地**参考（离 ego 最近轨迹点），cruise_lane_y = 本地 road_c + lane_d·cos(h)，只有本地查询失败才回退前视点；scene_pub 节点数按长度自适应（~25m 一点，8..128）；curve_road.json nodes 由 13 粗点重采样为 194 平滑点（前端 CR 与物理 Hermite 偏差 5.7m→<0.8m）；评估器弯道用局部 road_heading / road_signed_offset 替代绝对 y 启发 | `tools/json_to_xodr.py` build_polyline_road / `planning_node.cpp` frenet_to_cartesian / `control_node.cpp` query_ref_at 覆盖块+cruise_lane_y / `flowsim/scene_pub.cpp` road_nodes_per_edge / `scenarios/curve_road.json` / `ci/evaluators/demo_evaluator.py` |
| demo_evaluator 报 road departure 但车明明在车道里 | 评估器 `_road_network_cross_track` 用 topology `scene.road_network.edges[].nodes` 的弦长算路沿，而该 nodes 是 `scene_pub.cpp` 固定 8 点粗采样 → 长弯道上弦长偏离 Hermite 真路 ~14m → 车被误判出路沿（实测 curve_road 3000m S 弯 -7.44m）。修复：节点数按长度自适应 ~25m 一点（8..128） | `flowsim/scene_pub.cpp` road_nodes_per_edge / `ci/evaluators/demo_evaluator.py` |
| 浏览器 3D 卡成 PPT（后端 pub/frame_time/CPU 全稳定，只前端卡） | **无 GPU 环境（WSL/云 VM）浏览器用软件 WebGL**（SwiftShader/llvmpipe），默认 `medium` 性能档开着 Bloom+SMAA 后处理 + 2048 阴影 → 软件逐像素计算极慢；PHM 自动降级要 3×1s 窗口 <30fps 才降一档（6-9s 才到 low），期间已在看 PPT。另：轨迹 ribbon 每帧 5 层、其中 3 层 AdditiveBlending（外层辉光/内亮核心/流动条）按屏幕面积软件混合，最贵。修复：`isSoftwareRenderer()` 读 `WEBGL_debug_renderer_info` 匹配 SwiftShader/llvmpipe/softpipe → 检测到**直接 low 档启动**（禁后处理/阴影/DPR=1），不等自动降级；档位同步到 `store.perfTier`；TrajectoryView 低档跳过辉光+流动条两层装饰性加法混合，只留主体光带+内亮核心（加法层 3→1） | `tools/flowboard/js/vis/core/Renderer.js` isSoftwareRenderer / `main.js` 软件检测+_syncPerfTier / `view/TrajectoryView.js` lowTier 裁剪 |
| **浏览器内存飙到数 GB（渐进式卡顿真凶，2026-08-04 实测 5GB）** | LabelView._ensureLabel 用 speed 判断"文本变了才换纹理"，而车速**每帧都在变**（20.01→20.02…）→ 几乎每帧换纹理 → 每帧 `makeLabelTexture` 新建 CanvasTexture：① 旧纹理被替换后**从不 dispose**（GPU+JS 双泄漏）；② `CanvasTextureFactory._cache` 按 speed 作 key **每帧新增条目无界膨胀**（256×64 纹理 ×60fps ≈ 3.8MB/s → 数分钟到数 GB）。修复：LabelView 换纹理前 dispose 旧 map；makeLabelTexture 加 noCache 路径（label 车速高变 key 缓存命中率≈0，缓存只积累死条目）+ `_CACHE_MAX=300` 有界保护 | `tools/flowboard/js/vis/view/LabelView.js` _ensureLabel / `utils/CanvasTextureFactory.js` makeLabelTexture |
| 掉头返程幽灵刹车 + 无同向防撞（撞旁边车） | **safety 层系统性用世界 +x 坐标，掉头返程（向西）全部失效**：`min_oncoming_ttc` 用世界 obs_v<−2 判"迎面"→ 返程同向车（vx 也<0）被误判迎头 → head-on TTC 硬刹（幽灵刹车，实测返程 #4200 突然 brk=1.00 MRM）；`min_vehicle_ttc` 用世界 dx=obs_x−ego_x → 返程前车在 −x（dx<0）被 skip → 同向防撞完全失效 → 追尾 entity14（实撞）。planning 的会车让行已在 1221fad 用方向投影修复，safety 漏了。修复：safety 4 函数（min_vehicle_ttc / min_oncoming_ttc / nearest_same_lane_gap / nearest_vehicle_lateral_cross_risk）全部改沿车头方向投影（ahead=ex·cos h+ey·sin h、lat=|−ex·sin h+ey·cos h|、along_v=obs_v·cos h+obs_vy·sin h）；前进方向（heading=0）投影退化为原世界坐标零回归 | `safety_control_node.cpp` min_oncoming_ttc / min_vehicle_ttc / nearest_same_lane_gap / nearest_vehicle_lateral_cross_risk |
| 掉头撞路沿护栏 | 掉头距离兜底触发 `v≤7` 在车仍以 6.8 m/s 进弯时就触发（safety 幽灵刹车打断接近减速所致）→ 进弯太快 → 掉头弧外甩，车头角点（中心+半车长~2.2m）甩出路沿 7.0m。修复：兜底触发速度 7→5 m/s，车须减速到 5 才触发（实测进弯 4.8，OFFRAILS exit y=3.24 路内） | `behavior_planner_node.cpp` uturn 距离兜底触发 |
| sensor 模式改了生产者量程却"没效果"（远处目标一个都不出） | **量程门有两份实现**：`sensor_model` 与 `perception` 各有一个 `lidar_max_range_m`，消费者默认 **60m**，把生产者发来的 >60m 的点**静默**全丢（无日志）。实测 60→120 同步后锥内识别率 37.0%→86.9%（本 Phase 收益最大的一处）。属 CLAUDE.md 铁律"同一功能第二份实现=违规" | `perception_node.cpp:495` 默认值 / `config/pipeline_sensor.json` 必须两处同值 |
| 相邻车道两台车被聚成一个 width>15m 的"车"，中心落在两车之间 | ①测距噪声写成 `σ=0.08·range`（33m 处 2.6m，糊过 1.5m 车间空隙）；②DBSCAN `eps=2.0 > 相邻车道空隙 1.5m`（车道中心距 3.5m、车宽 2m）。修复：`σ = noise_std_m + noise_rel·range`（0.002/m）、`dbscan_eps` 收到 1.2 | `lidar_scan.c` 噪声模型 / `config/pipeline_sensor.json` 的 `dbscan_eps` |
| 点云永远 z=0、装在 1.8m 高却看不见 1.5m 高的车 | 旧观测模型只发射**单层方位射线**（elevation 恒 0），AABB 求交也是 2D、不检查 z。改为"方位 × 仰角层"3D 射线 + 3D AABB；仰角层间距须 ≤ 目标在 R 处的张角（车 1.5m 在 120m 处 ≈0.7° → 至少 32 层/22°） | `lidar_scan.c` `channels` / `sensor_model_node.c` |
| sensor 模式"预警提前量"恒为 0（且伪造 critical event） | `perception_clusters_to_obstacles` 用 `frame_id*100+ci` 当障碍物 id → 每帧都变，跨帧跟踪失效。ground_truth 模式用 `vehicle/state` 的 `oid%d` 已修，sensor 模式无 oid 可用，需给簇做跨帧关联或用 tracker 的已确认航迹 id | `perception_points.c` / `ci/evaluators/demo_evaluator.py` 预警提前量 |
| 行为决策整块静默 30s（该掉头不掉、该变道不变），而车仍在动 → 所有量活性门禁全绿 | `behavior_planner on_raw_obstacles` 收到**空**障碍物列表时直接 `return`（把"空"当"感知没就绪"）→ `has_obs` 恒 0 → `run()` 前置守卫跳过**整个决策块**（含路端掉头触发 `UTURN_TRIGGER`）。sensor 编排下 ego 背对视锥起步（前方空路）必现 → 车开过路网尽头冲出路面（评估器 road departure 6.80m/74 帧）；`curve_road` 在 GT 编排里也一直静默（committed 基线 `behavior_state: "NA"`）。修复：空列表=**合法结果** → `obs_count/raw_obs_count 归零 + has_obs=1`；配套门禁 `behavior planner never decided`（全程 NA→FAIL / 过半→WARN），且在**未修 C++ 的 A/B** 里首次抓到 curve_road | `behavior_planner_node.cpp` `on_raw_obstacles` / `demo_evaluator.py` 行为活性判据 |
| 幽灵刹车：本车道前方 80m 内"有静止障碍物"，实际空无一物（起步被压到 3.7 m/s） | `planning on_perception_obstacles` 每帧清零 128 槽后只填 `list.count` 个，而 TTC 跟车 / 窄路会车 / Frenet 注入 / ST 图四处循环**扫满 128 槽** → 未用槽 `(0,0)` 被当成"世界原点有障碍物"（同一份数据在 `forward_space` 里另有一条 `(0,0) continue` 豁免，一个文件两套有效性语义）。实测 `TTC follow: gap=20.0 -> 3.7 m/s`（ego 起点 x=20 朝 −x，原点恰在前方 20m 同车道），两种编排都命中。修复：引入权威计数 `g.obs_count`（与 `truth_obs_count` 对称），四处循环改 `i < g.obs_count`，删豁免 | `planning_node.cpp` `on_perception_obstacles` + TTC/会车/Frenet/ST 四处循环 |
| 同一份代码重跑一遍，场景判定在 PASS/FAIL 之间摇摆（门槛附近抖） | 两条判据的"参考量"与它要守的策略不是同一套尺度：① 跟车 `min_forward_gap` 用**整段中位速度**算期望间距（注释却写"判据随车速伸缩"）→ 低速逼近/排队停下的帧被高速帧的尺度误判 FAIL（实测最差帧 `gap=4.96m @ ego 1.0 m/s`，该速度下期望 6.5m 安全；三次实测 28 帧误报、逐帧判据 0 帧违规）；② `max_duration_s` 用**墙钟跨度**（含 demo.sh 启动/收尾与机器负载，同场景 59.9→63.3s 漂移），而仿真钟只有 59.3s。修复：①逐帧用该帧车速（与 behavior 的 `acc_standoff + acc_time_headway·|v|` 同式同参）；②改按 `metrics.scene.t_us` 的仿真跨度判，墙钟超而仿真没超记 WARN | `demo_evaluator.py` `min_forward_gap` 判据 / `_sim_timestamps` + `max_duration_s` 判据 |
| 真点云写点越界（潜在崩溃） | 旧射线循环 `cloud.points[cloud.count++]` **无容量守卫**；`n_rays=240 < 2048` 时侥幸没炸，提高射线数/密集场景（42 actor）即越界写。修复：写满即停 + `dropped` 上报 | `lidar_scan.c` `lidar_scan_generate` |

## 最新 tag

`v0.1.0` — 创始版本，8 节点全链路稳定运行

---

# 可视化架构

详见 [可视化架构](docs/VISUALIZATION_ARCHITECTURE.md)。
渲染层经验（车轮 kingpin 转向/贴地/SU7 车牌/相机跟随/性能降档/map preview）见
[3D 渲染沉淀](docs/VIS_3D_RENDERING.md)，改渲染代码先 `/vis-3d-rendering`、`/su7-rendering`。

前端 3D 渲染使用 `vis/` 架构：

```
tools/flowboard/js/vis/
├── main.js           — 入口（含 _applySceneStyle 风格上下文：BEV→sr / 透视→real）
├── core/             — 核心渲染框架（CameraRig, Lighting, Constants, Layer, Renderer[含
│                       isSoftwareRenderer], SkyEnv, ViewRegistry, AssetFactory, DeadReckon,
│                       PerfMonitor, WorkerBridge）
├── director/         — 场景导演（SceneDirector, FrameValidator）
├── view/             — 3D 视图（VehicleView, RoadView, GroundView, ViaductView, BarrierView,
│                       TreeView, TrafficLightView, ETCGateView, StreetlightView, ConnectorView,
│                       VehicleLights, EffectView, TrajectoryView, BuildingView, ConstructionView,
│                       RoadFacilityView, StreetFurnitureView, PerceptionView, LabelView, StatsView,
│                       JunctionDetect）
├── math/             — 坐标/几何工具（Coord.js — 唯一事实源, Curve, GeometryMerge, RoadHeight,
│                       MapProjection, Trajectory）
├── theme/            — 设计 token 单一事实源（tokens.js）+ 标线视觉样式表（roadStyle.js: MARKING/STYLE）
├── model/            — 数据模型（MapData.js 权威地图+走廊+按段加载, TopologyModel, RoadAxis）
├── store/            — 场景状态（SceneStore）
├── utils/            — CanvasTextureFactory（有界缓存，防纹理泄漏）
├── worker/           — TopologyWorker（拓扑解析 offload）
└── MinimapHUD.js     — 左上角小地图 HUD
```

数据链路由 C 监控守护进程 **flowmond**（`src/flowmond.c` → `build/bin/flowmond`）提供：

```
monitor_node → 10Hz 写 /tmp/flow_topology.json → flowmond :8800 → 浏览器 (文件桥接回退)
monitor_node → stats_bridge / dashboard_bridge → IPC SHM → flowmond :8800 → 浏览器 (IPC 桥接)
```

- 前端 `tools/flowboard/index.html` 由 flowmond 通过 `--html-path` 加载并托管。
- `modules/adas_nodes/flowmond_node.cpp` 是 flowmond 的 `NodePlugin` 包装版，可作节点插件在 pipeline 内运行。
- 启动：`./build/bin/flowmond --html-path tools/flowboard/index.html`（或通过 `scripts/demo.sh`，已改为调用 flowmond）。

| 组件 | 端口 | 说明 |
|------|------|------|
| `flowmond` (C) | 8800 | HTTP 仪表盘：IPC 桥接（首选）+ 文件桥接（`/tmp/flow_topology.json` 回退）+ 自动重连 |
| `foxglove_bridge.py` | 8765 | Foxglove Studio 3D 桥接 |
