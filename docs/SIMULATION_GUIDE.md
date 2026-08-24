# 仿真测试指南

> **注意：** 当前可用的仿真入口是 `flow_launcher config/pipeline.json`（配置驱动，dlopen 加载插件节点）。
> `tools/carla_bridge.py` 已提供可选 CARLA 适配边界和 capability gate；没有 CARLA
> SDK/服务器时会明确报告 unavailable，不会伪装成已接通。

## 三层仿真体系

```
Layer 1: Bag 回放 (零依赖, 现在就能跑)
  → 录制数据 → 回放给算法 → 校验输出

Layer 2: 2D 运动学模拟 (轻量, 验证控制/规划)
  → 单车模型 + 简单传感器 + 场景定义

Layer 3: CARLA (可选外部后端, 适配边界已就绪)
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

## Layer 2: 2D 模拟器

```bash
# 内置场景: 直道 + 前车 + 行人横穿
bash scripts/demo.sh

# 或使用配置驱动启动器指定场景
./build/bin/flow_launcher config/pipeline.json
```

内置场景定义见 `scenarios/straight_road.json`（4 车道直路：同向慢车 + 对向来车 + 行人 + 红绿灯）。

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

## 场景库

| 场景 | 难度 | 测试目标 |
|------|------|---------|
| 直道巡航 | ⭐ | ACC 纵向控制 |
| 弯道保持 | ⭐ | 横向控制 + 车道线检测 |
| 前车切入 | ⭐⭐ | 感知跟踪 + AEB |
| 行人横穿 | ⭐⭐ | 检测 + 紧急制动 |
| 高速变道 | ⭐⭐⭐ | 规划 + 预测 + 控制 |
| 无保护左转 | ⭐⭐⭐⭐ | 完整决策链路 |
| 鬼探头 | ⭐⭐⭐⭐⭐ | 感知极限测试 |

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

## e2e 内置 3D 场景仿真

`flow_launcher` 的 monitor 任务会导出真实 3D 场景到 `/tmp/flow_topology.json`
的 `metrics.scene` 字段，FlowBoard 仪表盘据此渲染真实三维场景（自车、
障碍物包围盒、LiDAR 点云），而非随机占位点。

包含的真实仿真要素：

- **运动学双轮自行车模型**：自车采用简化的运动学自行车模型（`step_bicycle`），
  由 `heading += steer * v / L` 驱动横摆，`x += v * cos(heading), y += v * sin(heading)`。
  适合公路巡航场景的轨迹追踪验证，EPS 转向执行器含一阶低通滤波模拟转向机惯性。
  运动学 / 动力学模型选择、适用边界和标定方法见[标定指南](CALIBRATION_GUIDE.md)。
- **ACC 纵向控制**：依据与同车道前车的间距动态限制目标速度，间距越近
  目标速度越低，触发真实的减速/刹车行为。
- **动态障碍物**：前车、对向来车、过街行人，具备运动学与循环边界，
  使场景持续有内容。
- **LiDAR 点云**：对障碍物表面 + 地面环带做光线投射后下采样，坐标位于
  自车系。

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
[book/12_demo_evaluator.md](book/12_demo_evaluator.md)。

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
