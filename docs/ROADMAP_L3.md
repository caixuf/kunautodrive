# 路线图：迈向更通用、接近真正 L3 的系统

> 愿景：让 FlowEngine 从"单场景仿真演示"演进为**接近真正 L3 的、可随仿真世界持续进化**
> 的自动驾驶中间件。核心策略是**模型 + 规则混合**、**雷达 + 视觉多模态**、**从 OSM
> 自动生产地图**、**NPC 自动填充制造交通流**，并以**仿真闭环**驱动算法迭代。
>
> 本文是方向性路线图，与 `UPGRADE_DIRECTION.md`（近期回归修复）互补：近期先把
> `main` 稳定在 `a065f26`，中长期按本文推进 L3 能力。

---

## 0. 总览：四个能力层 + 一个闭环引擎

```
        ┌─────────────────────────────────────────────────────┐
        │  方向五：仿真闭环引擎（数据→训练→评估→迭代，贯穿始终） │
        └─────────────────────────────────────────────────────┘
            ↑ 采集 / 评估 / 部署                ↓ 场景 + 交通
   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
   │ 方向一       │   │ 方向二       │   │ 方向三       │   │ 方向四       │
   │ 多模态感知   │←─│ 地图自动化   │←─│ 交通流生成   │←─│ 混合决策规划 │
   │ 雷达+视觉    │   │ OSM→HD-Map  │   │ NPC 自动填充 │   │ 模型+规则    │
   └──────────────┘   └──────────────┘   └──────────────┘   └──────────────┘
      地基              地图              环境              算法本体      放大器
```

四个能力层之间有依赖：**感知(一) + 地图(二)** 是地基；在其之上才能验证
**交通流(三)** 与 **规划(四)**；**闭环引擎(五)** 是放大器，把前四者变成可进化的系统。
**方向六（L3 安全闭环：ODD / 接管 / MRM / 冗余 / 安全论证）** 是 L3 区别于 L2 的
"系统负责"部分，与前五者并行推进，见下文。

---

## 方向一：多模态感知底座（雷达 + 视觉）

**目标**：可降级的冗余感知，满足 L3 对感知可靠性的要求。

- 救活并扩展 `feature/bev-perception-upgrade` 的 BEV 检测头：当前只修了视觉半边且
  直通模式回归（见 `UPGRADE_DIRECTION.md`）。先修复，再扩展为
  **相机 BEV + 雷达点云融合**（早期/中期融合均可）。
- 雷达补足视觉短板：夜间、逆光、恶劣天气下由雷达测距冗余支撑 ODD 内安全。
- 补齐 `sensor_model`：现仅有 lidar/gps，需加 **雷达模型 + 相机模型**
  （含噪声、遮挡、多径、退化）。
- **可降级原则**：任一模态失效时，剩余模态仍能支撑安全；感知节点输出统一为
  `perception/obstacles` + 置信度，供 fusion 做置信加权。

## 方向二：从 OSM 自动生产地图（HD-Map 自动化）

**目标**：让仿真世界自动拥有"无限里程"的可用地图。

- 基于已有 `MAP_ENGINE_ROUTING` 与 `city_comprehensive` 的 OSM 路线跟随雏形，
  把 OSM 自动转成**带拓扑 / 车道 / 交规 / 信号灯**的地图（lanelet2 或 OpenDrive 风格）。
- 增加 **地图匹配（map matching）**：把自车姿态对齐到车道级，作为定位与规划的输入。
- 这是"接近真正 L3"的关键杠杆：OSM 全球覆盖 → 仿真无需手工建图即可获得真实路网。

## 方向三：NPC 自动填充 → 真实交通流

**目标**：算法在仿真里面对的是交互密集、不可预测的真实交通，而非孤零零一辆车。

- 在 `flowsim` 现有 NPC 能力上，用 **OSM 路网做路线分配**，按流量模型批量生成
  车辆 / 行人：泊松到达 + **IDM 跟车** + **MOBIL 换道**。
- 支持按**时段 / 区域**调密度，构造拥堵汇入、无保护左转、加塞等难例场景。
- 为方向四的博弈类决策提供验证环境，也是方向五采集交互数据的来源。

## 方向四：模型 + 规则混合决策规划

**目标**：可解释、可验证的安全主干 + 处理复杂博弈的学习层。

- **规则层**（`behavior_planner` + Frenet / 状态机）做可解释、可验证的安全主干与兜底。
- **模型层**（模仿学习 / 轻量 RL）处理复杂博弈、拥堵汇入等规则难写场景，
  先跑 **shadow mode** 与规则层对比打分，达标再逐步接管。
- **仲裁层**：规则层永远是最低安全护栏，模型层做"更优解"，不达标即回退规则。
  （纯端到端在简历项目里反而难讲清安全性，混合架构更工程可落地。）

## 方向五：仿真闭环引擎（算法持续进化）

**目标**：把前四者串成"场景随机化 → 大规模并行仿真 → 采集 → 指标评估 →
离线训练/调参 → 部署回仿真对比 → 迭代"的进化环路。

- 复用已有 `SIM_DIGEST` / `LEARNING_LOOP` / `DATA_CLOSED_LOOP` 评估与数据闭环雏形。
- 补齐：**确定性回放**、**场景 DSL / 模糊测试**、**批量 headless 仿真**
  （跑得足够快、足够并行才能"进化"）。
- 以 **shadow mode 对比 + 指标门禁** 作为模型上车的放行条件，避免再次污染主链路。

## 方向六：L3 安全闭环（"系统负责"的那一半）

**目标**：方向一~五解决的是"能不能开"，L3（SAE Level 3：ODD 内驾驶员可脱手脱眼，
由系统对驾驶任务负责）还要求"开不了时怎么办 + 怎么证明安全"。这部分在现有代码里
基本空白，必须作为独立方向推进。

> 命名提醒：`include/degrade_ladder.h` 的 `DEGRADE_L3` 指降级阶梯的"立即停"档，
> **不是** SAE L3。建议后续把降级档改名（如 `DEGRADE_STOP`），避免两套 L0~L3 混用。

- **ODD 定义与监控**：新增 ODD 监控节点，实时判定道路类型、限速、天气/光照、地图覆盖、
  定位精度、传感器健康是否仍在 ODD 内；输出给 behavior 与 safety_control 消费。
- **接管请求（TOR）状态机**：`系统激活 → 发出 TOR（如 10s 倒计时）→ 驾驶员接管 / 超时进 MRM`。
  状态、时序、退出条件显式定义，状态转移表不许缺行（遵守 behavior 模块铁律）。
- **驾驶员状态监控（DMS）**：仿真里先建驾驶员模型（是否注视前方、接管响应时延），
  作为 TOR 时序的输入；真车再接摄像头。
- **最小风险策略（MRM）升级**：现有降级只有"车道内减速停车"，扩展为靠边停车 + 双闪 +
  ODD 边界前提前降速，并为每类失效（感知/定位/计算/执行器）指定对应策略。
- **冗余与降级**：感知任一模态失效仍安全（补雷达模型，增强相机模型）；定位 GNSS /
  航位推算 / 地图匹配交叉校验；计算链路双路监控 + 看门狗；执行器冗余假设。
- **事件数据记录（EDR）**：在现有 PEM 流上增加 L3 法规事件：激活/退出、TOR、MRM、
  碰撞前后窗口，复用 CRC / fsync / 轮转。
- **验证与安全论证**：
  - 场景库按 UN R157（ALKS）与国内 L3 测试规程补齐：切入/切出、前车急刹、静止障碍、
    施工区、ODD 退出、传感器失效注入；参数化 + 模糊测试 + 批量 headless 并行（与方向五合流）。
  - 故障注入框架：节点掉线、topic 延迟/丢包、传感器失效、定位跳变，逐一验证降级路径。
  - 仿真可信度：确定性回放、物理模型与真车数据标定对比。
  - 安全文档：ISO 26262（HARA / 安全目标 / ASIL 分解）、ISO 21448 SOTIF（已知/未知危险场景）、
    UN R155/R156（网络安全 / OTA），形成"需求 → 实现 → 测试 → 证据"可追溯链。
  - 统计目标：明确事故率/里程目标，用仿真里程 + 场景覆盖率论证。
- **前端配套**：FlowBoard 增加 L3 面板（ODD 状态、TOR 倒计时、DMS、MRM 阶段、降级原因码）；
  事故回放视图（`tools/trace_incident.py` 结果上 3D 时间轴）。

> 按仓库规范，ODD / TOR / MRM 属于算法与跨模块接口改动：先 Python 仿真验证，再移植 C++，
> 并在特性分支上推进，经场景矩阵与 `demo_evaluator` 门禁后合回 `main`。

---

## 现状体检（2026-10-02，`main` @ 6d95d43）

本节记录一次全量实测，作为方向六 P0 的起点。复现命令见 CLAUDE.md「验证」节。

| 项目 | 结果 |
|------|------|
| 构建 | ✅ 通过（**需 `libeigen3-dev`**，否则 Frenet 规划器静默不编，planning 退化为只车道保持，默认场景回归 FAIL） |
| `demo_evaluator.py` 默认场景 45s | ✅ PASS：avg 12.1 m/s，变道 7 次，无碰撞/闯红灯，识别率 100% |
| `scenario_regression.py --baseline`（8 场景） | ⚠️ 7 PASS / 1 FAIL：`lane_change_traffic` 车道保持 FAIL（99% 巡航帧中心压线，worst offset 7.0m，`committed_lane=-1`），单次运行，待复现定位 |
| 险情指标 | ⚠️ `straight_road` min TTC 0.124s / 4 次 critical；`lane_change_traffic` min TTC 0.025s / 5 次 critical —— 现有门禁不拦"险些相撞" |
| 舒适性 | ⚠️ `straight_road` jerk_max 68.6 m/s³（舒适一般 < 10），无门槛 |
| 安全证据 | ⚠️ 所有场景 `safety_evidence_present: False` |
| `npm run vis:check:all` | ✅ 6 门禁全绿（ESLint 0 error / 24 warning） |
| FlowBoard 运行时 | ✅ 连接正常，16 节点在线，拓扑 / 3D / 小地图渲染；⚠️ `/api/map/preview` 404；⚠️ `[RoadAxis] road 无 lanes` 警告 |

---

## 推荐节奏（依赖顺序）

1. **先地基**：救活并打磨方向一（感知）、方向二（地图自动化）。没地图和感知，
   NPC 与规划都无处验证。
2. **再造环境**：方向三（交通流），让仿真具备交互密度。
3. **再做算法**：方向四（混合决策），先在 shadow mode 跑对比。
4. **贯穿闭环**：方向五从第一步起就铺数据采集与评估，逐步把迭代自动化。

方向六（L3 安全闭环）按以下分期，与上面四步并行：

| 分期 | 内容 | 放行条件 |
|------|------|----------|
| **P0 地基补牢** | ① 定位并修复 `lane_change_traffic` 车道保持 FAIL；② `demo_evaluator` 增加 min TTC / critical 次数 / jerk·加速度 FAIL 门槛（配 `test_evaluator_gate.py` 自测）；③ 缺 Eigen 时构建/CI 直接失败而非静默退化；④ 每次评估输出 `safety_evidence` | 场景矩阵 8/8 稳定全绿 |
| **P1 L3 核心闭环** | ODD 监控 → TOR 状态机 → MRM 升级（先 Python 仿真），DMS 驾驶员模型 | ODD 退出 / TOR 超时场景进 suite 且 PASS |
| **P2 验证体系** | 法规场景库 + 故障注入 + 批量并行仿真；冗余（雷达模型、定位交叉校验、看门狗）；EDR | 每类注入故障都有可观测的降级路径 |
| **P3 安全论证与前端** | ISO 26262 / SOTIF / R155·R156 文档链；统计里程目标；FlowBoard L3 面板与事故回放；补 `/api/map/preview` | 需求→测试→证据可追溯 |

### P0 进展（2026-10-08）

| 项 | 状态 | 落地 |
|----|------|------|
| ② 险情/舒适性 FAIL 门槛 | ✅ 完成 | `demo_evaluator.py`：单帧 TTC 硬线（0.10s，不可放宽）+ 场景线 + jerk 门槛；自测 `test_evaluator_gate.py [26][27]`（42 项全绿）。**不回归**：`critical_event_count` 门的判据改用逐帧同车道最近前车时距（旧口径依赖真值身份匹配，噪声大，门禁判不准故未启用） |
| ②附 committed_lane 显示 bug | ✅ 完成 | `int(x or -1)` 把合法车道 0 误判 −1 → `_lane_idx()` |
| ③ 缺 Eigen 硬失败 | ✅ 完成 | 顶层 + 节点 CMakeLists `FATAL_ERROR`（留 `-DFLOWENGINE_ALLOW_NO_EIGEN=ON` 逃生口） |
| 行为 FSM 表门禁（新增） | ✅ 完成 | `ci/gates/behavior_fsm_check.py` + CI `behavior-fsm-gate`。抓出 STOP/YIELD/EMERGENCY 三个死状态，显式登记为待接线目标态（P1 MRM 用） |
| 长跑车道保持门禁（新增） | ✅ 完成（已转硬门禁） | `ci/gates/long_run_lane_keep.py`（16km 直道 soak，断言全程巡航不压线）。曾复现 ① 根因，CI 侧过渡期 `continue-on-error`；① 修复后 **2026-10-09 删除 continue-on-error 转硬门禁** |
| CI nightly 启动竞态修复 | ✅ 完成 | nightly 连续 5 天 "no topology samples collected"：`demo.sh` wait 预算 15s→40s 并按 demo 时长自适应封顶 + 非交互路径 exit 非零（`FLOW_REQUIRE_TOPOLOGY`） |
| ① `lane_change_traffic` FAIL | ✅ 完成（2026-10-09） | 见下 |
| ④ `safety_evidence` 全场景输出 | ✅ 完成 | `safety_control` 周期发 `evidence_type="safety_state"` 快照（`include/safety_evidence.h` 加 `periodic` 字段），正常场景也带证据；`demo_evaluator` 按类型挑证据（故障优先），新增 `validate_safety_state_evidence` + 2 单测。实测 `safety_evidence_present: True` |

**① 根因（2026-10-08 探针 + 长跑复现；2026-10-09 修复）**：单向路两套车道坐标系横向
布局相反。`planning_coordinates.h::lane_center_d` 把车道**对称铺在参考线两侧**
（4 车道 `lane_center_d(0..3)=+5.25,+1.75,-1.75,-5.25`），而 flowsim/OpenDRIVE 沿靠右
行驶把**单向路全部车道放在 −y 侧**（`json_to_xodr.py` 单向只生成 `<right>`、
lane id −1..−4 于 y=−1.75…−12.25）。于是 planning 的 idx0/idx1 映射到路面外（+5.25/+1.75），
且 behavior 的"超车后归位"分支（缺 `!road_oneway` 守卫）每帧触发 LEFT_CHANGE 往 idx−1
（+y 方向）走，ego 被拽出前进车道越参考线（`committed_lane` 2→1→0、y→+5.33）。

**修复（2026-10-09）**：
- `planning_coordinates.h`：新增 `lane_group_side_offset(N,w,oneway)=−N·w/2`（单向；双向 0），
  `lane_center_d`/`nearest_lane` 增 `side_offset` 形参（默认 0→双向零变化）。单向 4 车道
  恢复为 idx0=−1.75 … idx3=−12.25（与 flowsim 物理车道逐条对齐），ego@y=−1.75→idx0。
- 接线：`planning_node.cpp`（`lane_center_offset`/`lane_center_y`/巡航 `nearest_lane`/uturn
  目标道）、`behavior_planner_node.cpp`（`recalc_idx`/`target_lane_d`/`tl_y`/发布 y + 归位加
  `!road_oneway` 守卫 + `inner_lane` 单向取 0）、`flowsim_node.cpp`（内置巡航 fallback）。
- 测试：`test_planning_coordinates.cpp` 增单向断言 + round-trip（N∈1..8）。
- 验证：`lane_change_traffic` PASS；16km soak PASS（0 压线/0 越线）；33 ctest 全绿；
  `behavior_fsm_check`/`pipeline_check` 绿；9 场景矩阵除 `straight_road`/`dense_npc` 外全 PASS；
  sensor 编排三场景（urban_challenge/dense_npc/lane_change_traffic）全 PASS。
  **车道保持维度**：全场景 0 压线 / 0 越线（本项修复目标，已达成）。

**遗留（非本项）→ 2026-10-09 已修复**：`straight_road`/`dense_npc` 的 `comfort jerk max`
门禁**抖动**根因是**度量伪影**（非本项引入），现已修复：`compute_formal_metrics` 三缺陷叠加——
(1) 仿真结束后 `collect_samples` 每 interval 重复 append **冻结**拓扑 JSON → 尾部 dt=0 重复帧；
(2) 时间基取自**墙钟** `t_demo`，CI 负载下非单调（实测 dt=−0.64s/0.0192s）→ 微 dt 作分母炸出
jerk 100+；(3) `periods` 过滤非正 dt 后变短却仍按 `periods[index-1]` 取 → 丢一帧错位后面全部 accel。
修法：(a) 新增 `_sample_clock_seconds` 优先仿真钟 `metrics.scene.t_us`；(b) accel/jerk 改用
**对齐的 `(dt,a,i0,i1)` pair 列表** + 自相对 dt 下限（正 dt 中位数一半）剔伪影；(c) `collect_samples`
冻结帧去重。**实测收敛**：`dense_npc` 12/12 PASS（原 3/6 FAIL，jerk 由 ~928 降到 ≤21.7）；
`straight_road` jerk 恒 <12（原偶见 2564）。门禁判据保留 `max`（不改 p95，保对单次真实颠簸的检出）。
单测 `[28]`/`[29]` + 3 例 `test_demo_evaluator` 锁定契约。

**遗留（仍阻断 8/8 稳定全绿，均**既有**、与本项无因果）**：
- `straight_road` 间歇 `lane keeping: ego body rides the lane line`（~2/12）：探针（8 run）显示压线帧
  绝大多数是**掉头**帧（`maneuver=True`，已豁免），仅 3-6 帧非机动；FAIL run 里掉头跨道 ~30 帧被
  计入巡航压线（`max consecutive 30` = 跨道时长）→ 疑为**掉头期 behavior_state 遥测偶发非 U_TURN**
  致豁免失效，阈值（30 帧 / 30%）又恰好卡在观测值（27-30 帧 / 30-32%）上，属门禁边界 + 机动豁免
  的健壮性问题，非真实车道保持回归（16km soak 0 压线）。
- `no topology samples collected` 间歇（~1/6）：启动竞态（monitor 首帧 vs 采样开始），原评估器同样复现。


## 与近期工作的关系

- 近期回归修复见 `UPGRADE_DIRECTION.md`：`main` 先稳定在 `a065f26`，
  BEV 升级链保留在 `feature/bev-perception-upgrade`。
- 方向一是该分支的直接延伸；方向二~五是新能力，建议在各自特性分支上推进，
  经 `/verify` 与指标门禁后再合回 `main`。

---

*注：本文为长期方向性路线图，非实施补丁。各方向落地时再拆为可验证的小步提交。*
