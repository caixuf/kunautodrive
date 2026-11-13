# 仿真测试指南

> **注意：** 当前可用的仿真入口是 `flow_launcher config/pipeline.json`（配置驱动，dlopen 加载插件节点）。
> `tools/carla_bridge.py` 已提供可选 CARLA 适配边界和 capability gate；没有 CARLA
> SDK/服务器时会明确报告 unavailable，不会伪装成已接通。

## 三层仿真体系

```text
Layer 1: Bag / MCAP 回放（零外部依赖）
  → 录制真实或仿真数据 → 回放给算法 → 校验输出与确定性

Layer 2: 6-DOF 闭环仿真 + 城市级高精地图（FlowSim 默认引擎）
  → 6-DOF 簧载悬架动力学 + 载荷转移 + 局部摩擦区 + 16 线 3D OBB LiDAR + IDM/MOBIL 多车流 + 8 张 OSM/城市场景地图

Layer 3: CARLA 6-DOF 双向桥（可选外部后端，适配边界与 capability gate 已就绪）
  → 时钟快照 + 传感器批次 + 控制映射；真实闭环需 CARLA SDK/服务器
```

## Layer 1: Bag 回放测试

```bash
# 1. 录制场景数据
./build/bin/flow_launcher config/pipeline.json --duration 30 &
# 数据自动写入 /tmp/flow_topology.json + bus stats

# 2. 录制到 bag
# (KunAutoDrive 自动通过 bag_writer_attach 录制)

# 3. 回放 + 校验
./build/bin/flowctl bag play recordings/scenario.bag --config config/pipeline.json
```

## Layer 2: FlowSim 6-DOF 闭环仿真

```bash
# 一键启动默认 20 节点全栈闭环仿真（默认加载 scenarios/city_comprehensive.json，240s 城市综合场景）
bash scripts/demo.sh

# 无头/CI 指定场景与时长
bash scripts/demo.sh --scenario scenarios/urban_challenge.json --no-browser 30

# 或使用配置驱动启动器直接运行
./build/bin/flow_launcher config/pipeline.json --duration 30
```

默认场景 `scenarios/city_comprehensive.json` 覆盖城市高架匝道、多层立交、信号灯交叉口、雨雾天气退化与多车交互。物理模型与渲染细节分别见 [FLOWSIM_PHYSICS.md](FLOWSIM_PHYSICS.md) 与 [VIS_3D_RENDERING.md](VIS_3D_RENDERING.md)。

### 生态标准桥接（OpenDRIVE / Lanelet2 / Foxglove）

- **OpenDRIVE 1.4 导出与静态预检**：`tools/json_to_xodr.py` + `tools/xodr_compat_check.py --strict`
- **Lanelet2 导出与几何一致性门禁**：`tools/json_to_lanelet.py` + `ci/gates/lanelet_consistency_check.py`
- **Foxglove Studio WebSocket 桥接**：`tools/foxglove_bridge.py`（端口 `8765`）

### 可选 CARLA 后端

先检查后端能力，不会自动下载或伪造 CARLA：

```bash
python3 tools/carla_bridge.py capabilities --json
python3 tools/carla_bridge.py check
```

适配器只负责 simulator I/O，统一输出 `flowengine.simulator_capabilities.v1`
契约：仿真时钟 `SimulationClock`、传感器 `SensorPacket`/`SensorBatch` 和
`ControlCommand` → `carla.VehicleControl` 映射。规划、限速、安全闸门和评估仍
复用 KunAutoDrive 的 planning/control/safety/evaluator 链路；`check` 在缺少
CARLA Python API 时返回非零，CI 可据此选择性启用外部后端。

## 场景库（38 个场景 + 8 张高精地图）

`scenarios/` 下共包含 **38 个独立场景 JSON**（外加 `scenarios/suite.json` 回归矩阵清单），覆盖以下核心类别：

| 类别 | 代表场景 | 测试目标 |
|------|----------|----------|
| 城市综合闭环 | `city_comprehensive.json`（默认）、`urban_challenge.json` | 高架匝道高程、立交、信号灯、雨雾、多车交互全要素 |
| OSM 城市场景 | `osm_lujiazui_v2.json`、`osm_munich.json`、`beijing_guomao.json`、`osm_zhengdong.json` | 真实 OSM 车道级路网、多路线规划与城市路口穿行 |
| 泊车与窄域 | `valet_parking.json`、`auto_parking.json`、`parallel_parking_exam.json` | Hybrid A* + Reeds-Shepp 正/倒车换挡闭环 |
| UN R157 (ALKS) | `r157_cut_in.json`、`r157_cut_out.json`、`r157_hard_brake.json`、`r157_construction_zone.json`、`r157_sensor_failure.json` | 旁车切入/切出、前车急刹、施工区收窄、传感器失效 |
| L3 安全闭环 | `l3_odd_exit_takeover.json`、`l3_odd_exit_timeout_mrm.json`、`wet_road_evasion.json` | ODD 退出 → TOR 倒计时 → MRM 真停、湿滑摩擦区避险 |
| 极端博弈与感知 | `unprotected_left_turn.json`、`congestion_merge.json`、`bev_shadow.json`、`pedestrian_flow.json` | 无保护左转、拥堵汇入、BEV 影子精度对比、行人横穿 |

## 测试指标

```bash
# 每个场景跑完，自动收集:
flowctl topic stats control/cmd     # 控制指令统计
flowctl param get control.max_speed # 参数状态
grep "collision" scenario.log       # 碰撞检测
grep "timeout\|miss" scenario.log   # 超时/丢帧
```

## 快速开始

```bash
# 1. 最简单: 一键 demo (含全链路节点)
bash scripts/demo.sh

# 2. Bag 回放 (需要预先录制的 bag)
./build/bin/flow_launcher config/pipeline.json --bag recordings/test.bag --duration 10
./build/bin/flowctl bag play recordings/test.bag --config config/pipeline.json

# 多进程链路验证（launcher 父进程通过 IPC 注入 replay）
./build/bin/flowctl bag play recordings/test.bag --config config/pipeline.json --multi

# 3. 接入算法测试
./build/bin/flow_launcher config/pipeline.json
```

## FlowSim 3D 场景仿真与三大前端入口

`flow_launcher` 的 `monitor` 节点会导出真实 3D 场景到 `/tmp/flow_topology.json`（及 `dashboard_bridge` POSIX SHM IPC）的 `metrics.scene` 字段，FlowBoard 仪表盘据此渲染真实三维场景，而非占位点。

包含的真实仿真要素：

- **6-DOF 簧载悬架动力学**：`flowsim` 支持二阶弹簧阻尼悬架（`pitch` / `roll`）、纵向载荷转移 $\Delta F_z$、道路高程坡度补偿与局部路面摩擦系数区 `friction_zones`（如湿滑 $\mu=0.35$）。标定方法见[标定指南](CALIBRATION_GUIDE.md)与[物理文档](FLOWSIM_PHYSICS.md)。
- **IDM + MOBIL 多 OD 智能交通流**：前车、对向来车、横穿行人及多路线交通流具备让行与变道交互状态机。
- **16 线 3D OBB LiDAR 与雨雾退化**：对障碍物 3D 包围盒与地面环带做射线投射，支持雨雾天气下的点云噪声与有效测距退化。

运行后可访问三大前端入口（复用同一 `tools/flowboard/js/vis/` 52 模块 Three.js r160 PBR 引擎）：
- 实时全栈驾驶舱：`http://localhost:8800`
- 38 场景离线展厅：`http://localhost:8800/tools/flowboard/showcase.html`
- OSM 高精大图预览器：`http://localhost:8800/tools/flowboard/map_preview.html?map=osm_lujiazui_v2&route=main`

一键运行（业务节点 + flowmond 仪表盘 + 浏览器）：

```bash
./scripts/demo.sh
# 或手动:
./build/bin/flowmond --html-path tools/flowboard/index.html &
./build/bin/flow_launcher config/pipeline.json --duration 3600 &
open http://localhost:8800
```

`scene` 数据结构与坐标系约定详见
[FlowBoard Scene 契约](FLOWBOARD_SCENE_CONTRACT.md)。
完整的离线问题根因分析与鲁棒性设计详见
[book/23_demo_evaluator.md](book/23_demo_evaluator.md)。

## 场景矩阵回归（仿真即测试）

把「场景库 × 评估指标」做成一条命令的批量回归，用于替代实车路测的"验证"职能。

场景库由 `scenarios/` 目录下的多份场景 JSON（直道/弯道/密集 NPC/多灯/对向/泊车/
驾考科目一~四/城市挑战等）+ `scenarios/suite.json` 场景矩阵组成。批量回归套件由
`ci/evaluators/scenario_regression.py` 驱动。

```bash
# 列出套件里将运行的全部场景（不启动 demo）
python3 ci/evaluators/scenario_regression.py --dry-run

# 跑整个套件，输出 PASS/FAIL 矩阵报告
python3 ci/evaluators/scenario_regression.py

# 首次录制回归基线（落到 tests/baseline/）
python3 ci/evaluators/scenario_regression.py --update-baseline

# 跑套件并与基线做数值对比，退化即 FAIL（用于回归门禁）
python3 ci/evaluators/scenario_regression.py --baseline

# 只跑单个场景（按文件名去后缀匹配）
python3 ci/evaluators/scenario_regression.py --only ghost_pedestrian

# 使用 4 个隔离 worker 并发跑场景（每个 worker 独立拓扑/日志）
python3 ci/evaluators/scenario_regression.py --workers 4
```

在不改变既有场景格式的前提下，`scenarioctl` 可以生成可复现变体，并从失败
结果重跑原场景：

```bash
python3 tools/scenarioctl.py generate \
  --template scenarios/lane_change_traffic.json \
  --count 20 --seed 20260811 --speed-scale 0.9 \
  --position-jitter-m 3 --output-dir /tmp/scenario_variants

python3 tools/scenarioctl.py replay \
  --result /tmp/flow_bad_cases/<run_id>/lane_change_traffic.json \
  --output /tmp/lane_change_replay.json
```

生成文件保留原有 FlowSim 字段，只增加 `generation` 元数据；每个变体的 seed
独立可复现。重放命令从结果的 `run.scenario_file` 读取原场景，并再次调用同一个
`demo_evaluator.py`，不会绕过现有安全门禁。

每次矩阵运行会在 `--results-dir` 下生成 `run_manifest.json`，记录套件哈希、
Git revision、worker 数、通过/失败/回归数量和每个场景的结果路径。并发模式会为
每个 worker 创建独立 pipeline、拓扑快照、日志和 PID 文件，并禁用 dashboard/
Foxglove 服务，避免共享端口和临时文件互相干扰。失败或数值回归的完整
结果默认归档到 `/tmp/flow_bad_cases/<run_id>/`，便于直接交给事故分析和
场景重放；可用 `--no-archive` 关闭复制：

```bash
python3 ci/evaluators/scenario_regression.py \
  --results-dir /tmp/flow_regression \
  --archive-dir /tmp/flow_bad_cases
```

底层由 `ci/evaluators/demo_evaluator.py` 逐场景执行；后者新增两个可组合参数：

- `--scenario <path>`：临时把 `config/pipeline.json` 的 `sim_world.scenario_file`
  指向该场景（运行后自动还原）。
- `--json-out <path>`：把 `{scenario, result, failures, warnings, summary}`
  写成机器可读 JSON，供回归矩阵聚合。

数值回归阈值支持两种门：`min_ratio`（当前值 ≥ 基线 × 比例）与
`max_abs_increase`（当前值 ≤ 基线 + 增量）。判定逻辑见
`ci/evaluators/scenario_regression.py::compare_summary()`。

### 统一评测报告与标准轨迹指标

不重新运行仿真即可汇总结果协议：

```bash
python3 tools/eval_report.py \
  --input /tmp/flow_regression --json
```

开环预测可通过独立 JSON 注入，格式为
`{"predictions":[{"prediction":[{"x":0,"y":0}], "ground_truth":[...]}]}`：

```bash
python3 tools/eval_report.py \
  --input /tmp/flow_regression \
  --predictions /tmp/open_loop_predictions.json --json
```

报告输出场景通过率、按 `run.mode` 分组的通过率、标准开环 ADE/FDE 和 MPI。
ADE/FDE 只有同时提供等长预测
轨迹与真值轨迹才会是 `computed`；MPI 只有生产者同时提供数值 `mpi` 和
`mpi_definition` 才会汇总，否则明确标记 `unavailable`，不会把闭环车道误差
冒充开环预测指标。

CI 或岗位演示需要把标准指标作为硬门禁时，显式追加：

```bash
python3 tools/eval_report.py \
  --input /tmp/flow_regression \
  --predictions /tmp/open_loop_predictions.json \
  --require-open-loop --require-mpi
```

门禁要求对应指标状态为 `computed`；`unavailable` 或 `invalid` 都返回非零状态。
