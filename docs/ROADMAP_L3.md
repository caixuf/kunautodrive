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
| **P1 L3 核心闭环** | ODD 监控 → TOR 状态机 → MRM 升级（先 Python 仿真），DMS 驾驶员模型 | ODD 退出 / TOR 超时场景进 suite 且 PASS → ✅ **2026-10-11 达成**（见下 P1 进展） |
| **P2 验证体系** | 法规场景库 + 故障注入 + 批量并行仿真；冗余（雷达模型、定位交叉校验、看门狗）；EDR | 每类注入故障都有可观测的降级路径 → ⏳ **2026-10-11：已登记 6 类达成**（两刀，见下 P2 进展；雷达模型/EDR 待做） |
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
- `straight_road` 间歇 `lane keeping: ego body rides the lane line`（~2/12）：**真实掉头后横向落位 bug**，
  非门禁伪影。探针实证（FAIL run）——掉头后巡航 `ego.y≈−3.13`（漂到 −3.24），`laneid=−1`、
  `lane_match_offset≈−1.38`，而车道 −1 中心 = −1.75 → **自车真实停在偏 −1.38m 处、车身骑 −1/−2 线**
  （正是门禁注释里"规划目标 −1.75 而自车停在 −3.3"那类故障，门禁**判对了**）。PASS run 收敛到
  −1.75（offset≈−0.22）。**已证既有**：pre-P0① worktree（4737b5f 重建）跑 straight_road 同样复现
  lane-keeping FAIL，与本项/P0① 均无因果。

  **根因（2026-10-10 定位，已坐实）**：`behavior_planner_node.cpp:1506-1509` 掉头完成判据
  **只看 heading、不看横向落位**——`if (fabs(fabs(hn) - uturn_target_h) < 0.15) ev = BEH_EV_COMPLETED`。
  实测日志 `uturn COMPLETED (h=-0.15) → CRUISE`：车头一进入 ±0.15 rad 窗口即宣告掉头完成，**当时车在
  哪个横向位置就是哪个**。掉头是**多把方向**（前弧→倒车→前弧，实测耗时 14.8s），heading 对齐发生在
  横向摆位过程中/末段，落位残差随执行方差而变化。planning 侧另有一个**半车道宽**的完成容差
  （`planning_node.cpp:898` `|y - target_lane_center_y| < 1.75`）同样偏松。

  **✅ 已修复（2026-10-10，由 251d8ed 收口）**：落位残差**本是摆位问题，但被 Frenet 横向走廊的自锁
  放大成压线**——planning 的 Frenet 横向走廊无车道边约束（`max_road_width ≈ 6.0m` ≈1.7 车道宽），
  ego 一旦偏到带沿就输出**恒定偏移直线**且全程不收敛 → control 忠实跟随 steer=0 → 每帧重规划又得同
  一偏移 → **骑线自锁**。`251d8ed` 加"安全带"（`safe_band = lane_width/2 − ego_half_width − guard`，
  带内放行保车道内避障、带外强制 `d→0`）切断自锁。本项探针（掉头完成只看 heading → 落位残差）是
  该自锁的**触发源**，两者合起来解释完整因果链。
  **复验（本次）**：`straight_road` 直跑 **12/12 PASS**（原 ~2/12 FAIL）；全矩阵（9 场景）连跑
  **2 次全 PASS**；`dense_npc` 12/12 PASS（jerk 修复）。**P0 放行条件「8/8 稳定全绿」达成**。
  注：16km 直道 soak 无掉头故 0 压线；**未改门禁阈值**（未掩盖真实落位缺陷）。
- `no topology samples collected` 间歇（~1/6）：启动竞态（monitor 首帧 vs 采样开始），原评估器同样复现。


### P1 进展（2026-10-11）

两节点本体（`odd_monitor_node.c` 无状态 ODD 分类器 / `tor_manager_node.c` 5 状态
10 转移 TOR FSM + 内嵌 DMS + MRM 权威）与管线接线已由 `76071e6` / `ad015c9` 落地，
Python 先行仿真在 `tools/tor_mrm_sim.py`。**本轮补的是放行条件本身**——原先
"ODD 退出 / TOR 超时场景进 suite 且 PASS"这一条**没有任何场景与判据承载**：
`suite.json` 里 0 个 L3 场景，`demo_evaluator` 里 0 条 ODD/TOR 断言。

| 项 | 状态 | 落地 |
|----|------|------|
| L3 行为判据（新增） | ✅ 完成 | `demo_evaluator.py::_evaluate_l3_expectations`：场景 `pass_criteria.l3_expect` 声明 `odd_exit` / `tor_requested` / `driver_takeover` / `tor_timeout_mrm` / `mrm_stop`。**遥测缺失 = INCONCLUSIVE ≠ PASS**（沿用 `require()` 语义）——`odd_monitor`/`tor_manager` 掉线不会静默变绿。7 条单测 `L3ExpectationGateTest` |
| ODD / TOR 遥测透传 | ✅ 已具备 | `monitor_node` 透传 `metrics.odd` / `metrics.tor`（`76071e6`）；评估器逐帧提取 `odd_in_odd` / `odd_violations` / `tor_state` / `tor_countdown_s` / `mrm_active`，并在 summary 输出 `l3_*` 事实键供回归对比 |
| ODD 退出 → 接管场景 | ✅ 完成 | `scenarios/l3_odd_exit_takeover.json` + 进 `suite.json`。`odd` 块在 t=5s 注入能见度 1000→80m（**分类器真判** out，非强制标记）；DMS `will_takeover=true`/延迟 1.5s → `TOR_REQUESTED` → `DRIVER_TAKEOVER`，**不得**进 MRM。实测 PASS：`l3_odd_violations_seen=['visibility']`、`l3_tor_states_seen=[ACTIVE, DRIVER_TAKEOVER, OFF, TOR_REQUESTED]` |
| ODD 退出 → 超时 → MRM 场景 | ✅ 完成 | `scenarios/l3_odd_exit_timeout_mrm.json` + 进 `suite.json`。同注入，DMS `will_takeover=false` → 10s 倒计时耗尽 → `MRM` → 车道内停稳。**同时校验请求与执行**：只看 `tor.state=MRM` 会漏掉"发了 MRM 但 control 没降速"，故 `mrm_stop` 额外要求 `mrm.active` 出现且速度收敛到 1 m/s 以下。实测 PASS：`l3_mrm_active_frames=12` |
| Python 仿真进门禁 | ✅ 完成 | `tools/tor_mrm_sim.py --run-all` 注册为 ctest `tor_mrm_sim_tools`（退出码 = 失败断言数）。此前它是**孤儿工具**——CI/ctest 零调用，铁律要求的"Python 先行"没有守门 |

**本轮抓出并修掉的真实缺陷（均由新判据暴露，非既有 flake）**：

1. **`tor_manager` 车速字段名错**（🔴 功能性，MRM 等于没接）。
   `flowsim` 的 `vehicle/state` 车速字段是 **`spd`**（`flowsim_node.cpp` vstate 发布），
   全仓其余消费者（control / planning / perception / behavior / monitor / sensor_model /
   object_tracker）**都读 `spd`**；唯独 `tor_manager_node.c` 读 `"speed"` → `g.speed` 恒为 0
   → MRM 的停稳判据 `|speed| < 0.1` **立刻就真** → 车还在 20 m/s 时宣告 `MRM_COMPLETE`
   并退回 `OFF`，随后重新加速。实测时间线（修复前）：

   ```
   t=15.6  tor=MRM            mrm.active=True   v=20.03   ← MRM 进入，车仍在 20 m/s
   t=17.3  tor=OFF            mrm.active=False  v=11.39   ← 1.7s 后"停稳完成"，其实没停
   t=23.6  tor=OFF            ...               v=10.33   ← 重新加速
   ```

   修法：以 `"spd"` 为准、保留 `"speed"` 作兼容回退。**这正是"门禁抓不住已知故障 =
   它的 PASS 不可信"的实例**——若只断言 `tor.state` 出现过 `MRM`，本缺陷会全绿通过。

2. **`demo.sh` 的 `scenario_file` sed 漏 patch 新节点**（🔴 接线，L3 场景必然 FAIL）。
   `odd_monitor` / `tor_manager` 的 `params` 写成 compact 形式
   `"scenario_file":"..."`（冒号后**无空格**），而 `demo.sh` 的 sed 模式
   `\\\"scenario_file\\\": \\\"...` 强制要求一个空格 → 这两节点永远拿不到
   `--scenario` 指定的场景，其 `odd` / `driver` 注入块**静默失效**。
   同一类漏洞也在 `demo_evaluator.pipeline_scenario_override` 的白名单里
   （只含 flowsim/planning/navigation/control）。修法：sed 改 `: *`（零或多个空白），
   白名单补两节点。

**验证**：34 ctest 全绿（新增 `tor_mrm_sim_tools`）；`test_demo_evaluator` 47 项全绿
（新增 7 条 L3 判据测试）；9 条静态 gate 全绿；两 L3 场景 `demo_evaluator` 实跑 PASS；
`npm run vis:check:all` 45 项全绿。

**P1 放行条件「ODD 退出 / TOR 超时场景进 suite 且 PASS」→ 达成**（两场景进 `suite.json`
且各自 PASS）。

**遗留（转 P2）**：DMS 目前是 `scenario` 里写死的模型参数（`driver` 块），尚无
"驾驶员注视/分心随仿真时间变化"的动力学；TOR 倒计时期间 `safety_control` 的双闪只在
`mrm_request` active 时点亮，TOR 阶段（尚未确认接管）的 HMI 提示尚未分级；
MRM 只有"车道内减速停车"一种，靠边停车 / ODD 边界前提前降速属 P2。


### P2 进展（2026-10-11，第一刀：故障注入框架 + 降级路径可观测性）

P2 的放行条件是「**每类注入故障都有可观测的降级路径**」。开工时盘点发现它的
三个前提**都不成立**：

1. **没有通用注入手段**：唯一注入是 `safety_control` 节点内的 `raw_cmd_timeout`
   （`include/safety_fault_injection.h`），只能造"控制指令超时"一类；节点掉线 /
   topic 丢包 / 传感器失效**无法注入**。
2. **降级状态不可观测**：`degrade` 只写 PEM（`monitor_node.c` 的 `pem/degrade_event`），
   拓扑 JSON 里**没有任何 degrade 字段** → 门禁无从断言"是否走到了预期降级路径"。
3. **注入从未在 CI 里跑过**：`fault_injection` 参数在 `config/` 与 CI 里**零出现**，
   即"能力存在但从未被验证"。

| 项 | 状态 | 落地 |
|----|------|------|
| 通用故障注入 | ✅ 完成 | 新增 `include/fault_inject.h` + `src/core/fault_inject.c`。**单点收口**在 `transport_publish()`（全仓每个节点发布都经此，`publish_loaned` 的 ROUTE_LOCAL 快路径也单独补了判定，否则会绕过）。env 驱动（`FLOW_FAULT_INJECT="<topic>:<after_ms>[:<duration_ms>];..."`），默认关闭，热路径未启用时只多一次 bool 读（同 message_bus 的 `remap_active` 手法）。`--multi` 模式的 `flow_node_host` 也初始化（否则该模式静默无注入）。9 条单测 `test_fault_inject.c` |
| 降级路径可观测 | ✅ 完成 | `metrics.degrade` 导出 level / **reason_name 稳定字符串** / active / 逐节点心跳健康（name/age_ms/timed_out/lost）。新增 `degrade_reason_name()` 作为原因码→名字的**单一事实源**（原先只在 `degrade_supervisor_summary()` 里转成人类文本，评估器无法按名断言）。另导出 `metrics.fault_inject.dropped` 实测丢弃计数，让门禁能区分**「注入没生效」与「降级路径没走到」**——这两者混在一起会把基础设施问题记成 L3 缺陷 |
| Python 先行矩阵 | ✅ 完成 | `tools/fault_degrade_sim.py`：故障类 → 期望降级路径的表（单一事实源）+ supervisor 递进语义 + raw_cmd **moving 守卫**（车停稳时不得升 L3，否则"停→L3→起步→L3"无限拉锯）自检；`--check-matrix` 强制每类都声明可观测落点（`degrade`/`control_mode`/`topic_freq`/`inject_ack`），空落点判 FAIL |
| 端到端门禁（有牙） | ✅ 完成 | `ci/gates/fault_inject_gate.py`：对每类故障**真起管线、真注入**，从 `metrics.degrade`/`control_debug`/`fault_inject` 读回实测值与矩阵比对。CI `integration-test` job 每次 push 跑 |
| 4 类故障全部可观测 | ✅ 达成 | `localization_loss`→L1/fusion_timeout + ODD `loc_stale`；`planning_loss`→control `DATA_TIMEOUT` fallback；`perception_loss` / `raw_cmd_timeout`→注入确认 + topic 频率归零（这两类的**正确**行为就是不升降级，见下） |

**本轮抓出并修掉的真实缺陷（由新门禁暴露）**：

- 🔴 **`control` 的 DATA_TIMEOUT fallback 在拓扑里完全不可见**。
  `control_node.cpp` 的 planning 掉线分支在发布 `control/debug` **之前** `continue`
  （line 635），而 `control/debug` 是 monitor 拿 `control_debug` / `driver_mode` 的
  唯一来源 → 注入 `planning/trajectory` 后 `control_debug.mode` 观测为空集，
  **降级路径存在但不可观测**。修法：在该分支补一条最小 debug（只带本分支实际持有的
  量，不复用常规块——那块的局部变量在 fallback 路径上不存在），频率与常规块一致（2Hz）。
  修后实测 `control_debug.mode=['DATA_TIMEOUT']`。
  这正是 P2 放行条件要拦的东西：**"有降级逻辑" ≠ "降级可被验证"**。

**一个需要说清的判据设计**：`perception_loss` 与 `raw_cmd_timeout` 的期望等级是
**L0**（不升降级），这是**正确的**降级语义而非门禁放过：
- 感知障碍丢失 → 安全层按"无目标"巡航（不该因为"没看见障碍"就降级），靠下游 TTC 兜底；
- `raw_cmd` 丢失 → 车随即停稳，`safety_control` 的 **moving 守卫**抑制 L3（刻意设计，
  见 `safety_control_node.cpp` 注释），行驶中失联的 L3 路径由 `safety_fault_evidence`
  单测覆盖。
  故这两类的落点是 `inject_ack` + `topic_freq`，而非 `degrade` 等级。

**一个实测澄清（反直觉，已写进门禁注释）**：永久注入一个 topic 时，该 topic 会
**整个从 `metrics.topics` 消失**（不是显示 0 Hz）——因为注入点在 `message_bus_publish`
之前，消息从未进过 bus，`pub_count` 恒 0，于是 stats 表里没有这个条目。
A/B 实测：干净跑 43 个 topic（含 `perception/obstacles`），注入后 41 个（不含）。
故门禁对"topic 缺席"的判定改为：**有丢弃计数时，缺席即为注入生效的强证据**。

**验证**：35 ctest 全绿（新增 `fault_inject_tests` + `fault_degrade_sim_tools`）；
`ci/gates/fault_inject_gate.py` **4/4 类可观测**；9 条静态 gate 全绿；
`demo_evaluator` 默认场景 PASS（无回归）；`scenario_regression --baseline` 除
`dense_npc` 一次既有启动竞态（重跑即 PASS，见 P0 遗留）外全 PASS。

**P2 放行条件（第一刀）「每类注入故障都有可观测的降级路径」→ 对已登记的 4 类达成。**
**（第二刀起扩到 6 类，见下。）**

**P2 遗留（下一刀）**：注入类别仍需扩展到**冗余**维度——雷达传感器模型缺失
（`sensor_model` 只有 lidar/gps/camera，全仓无 radar）、EDR 的 L3 法规事件
（激活/退出、TOR、MRM、碰撞前后窗口）尚未进 PEM 业务流。另外 `perception_loss`
的可观测落点目前只到"注入生效 + topic 归零"，**"感知降级后系统行为是否仍安全"
尚无专门判据**（需配合 TTC/碰撞门禁构造一个"盲开仍不撞"的场景）。


### P2 进展（2026-10-11，第二刀：定位冗余交叉校验）

第一刀把「每类注入故障都有可观测的降级路径」落了地，但**"冗余"这一项还是空的**。
盘点定位链路发现三处真实缺口：

1. **双源无互校**：`fusion_node` 有 GPS + LiDAR/SLAM 双源，但两者之间**没有任何
   一致性检查**；只有一个 `diverged` 单标志（χ² 连续失败 >10 或协方差 trace>1e9
   才置位）→ 中间地带（两源互相矛盾、观测被大量拒绝）完全不可见。
2. **诊断量算了不发**：`EkfFusion.gated_count` 被算出来却**从未发布**——源码注释
   自己写了"fusion_node 应把它发布出去"，是个自认的 TODO。
3. **死原因码**：`DEGRADE_REASON_LOCALIZATION` 在 `degrade_ladder.h` 里定义、
   在 `degrade_reason_name()` 里有名字，但**全仓没有任何地方 set 它**。

| 项 | 状态 | 落地 |
|----|------|------|
| 交叉校验规则（Python 先行） | ✅ 完成 | `tools/localization_integrity_sim.py`：位置差分速度 vs 另一源速度、位移方向 vs 另一源航向、观测拒绝率、协方差 trace 四项判定 + 去抖。`--check-rules` 强制「单源陈旧**不得**算 critical」——冗余生效不等于定位丢失 |
| 交叉校验实现 | ✅ 完成 | `include/localization_integrity.h` + `src/core/localization_integrity.c`（纯函数、无时钟依赖，可完全单测）。11 条单测 `test_localization_integrity.c` |
| 死码激活 | ✅ 完成 | `all_sources_lost`（两源全丢）去抖 1s 后置 `L2/localization_lost` —— `DEGRADE_REASON_LOCALIZATION` **从死码变活码** |
| 诊断量发布 | ✅ 完成 | `fusion/localization` 增 `loc_trustworthy` / `loc_critical` / `loc_reasons[]` / `gated_count` / `gated_ratio`；monitor 透传为 `metrics.localization` |
| ODD 消费完整性 | ✅ 完成 | `odd_monitor` 此前只看**消息龄期 + diverged**；现消费 fusion 的交叉校验裁决，新增 `loc_untrustworthy` violation（ODD 是"在不在域内"的权威，定位不可信属于域外） |
| 新故障类：GPS 单源丢失 | ✅ 完成 | `gps_loss_redundancy`：断言 `loc_reasons=['gps_stale']` 且 **`loc_trustworthy=true`、不降级**。这与"定位丢失"是两回事，门禁把两侧都钉住 |
| 新故障类：全源丢失 | ✅ 完成 | 注入 `sensor/gps` + `sensor/lidar` → L2/`localization_lost` + `loc_trustworthy=false` + ODD `loc_untrustworthy` |

**本轮抓出并修掉的真实缺陷（由新门禁暴露，且**都是我这一刀自己引入/漏掉的**）**：

- 🔴 **`fusion/localization` 有两个发布点，只给一个加了完整性字段**。
  正常路径与"无-lidar 降级路径"各发一次；我只加了前者 →
  真车配置（`lidar_driver` 未使能）这类场景的完整性在遥测里缺席。
  与上一轮 control 的 DATA_TIMEOUT fallback 漏发 debug **是同一类缺陷**。
- 🔴 **全体源静默时 fusion 直接 `continue`，一帧都不发**。实测注入两源后
  fusion 日志 `0 fused frames`、`fusion/localization` 频率归零 —— 本节点
  **自己的输入丢失在遥测里完全不可见**。讽刺的是紧邻的代码注释正是在警告
  这个"最贵的失败形态"，但旧实现只在"无 lidar 但有 gps/pose"这条路上兑现了。
  修法：全丢也照发一帧，如实报 `all_sources_lost`（仍是纯惯性递推，状态连续）。
- 🔴 **monitor 的 `s_inputs` 与 `pipeline*.json` 漏同步**。我只在 C 代码里
  `transport_subscribe` 了 `fusion/localization`，三份 pipeline 的 monitor
  subscribe 列表都没加 → 订阅在跑但拓扑里恒无该块。**`topic_contract_check.py`
  当场抓住**（它比对 JSON subscribe ↔ `s_inputs`）——这正是该闸门存在的意义。
- 🔴 **`odd_monitor` 的定位健康项与 fusion 的结论脱节**。ODD 只看消息龄期 +
  `diverged`，即使 fusion 已判 `loc_trustworthy=false`，ODD 仍认为"定位健康"。

**验证**：39 ctest 全绿（新增 3 项：`localization_integrity_tests` /
`localization_integrity_sim_tools` / 既有项）；`ci/gates/fault_inject_gate.py`
**6/6 类可观测**；9 条静态 gate 全绿；`scenario_regression --baseline` 见下。

**P2 放行条件「每类注入故障都有可观测的降级路径」→ 对已登记的 6 类达成。**


### P2 进展（2026-10-11，第三刀：EDR — L3 法规事件记录）

P2 要求「**EDR**：在现有 PEM 流上增加 L3 法规事件：激活/退出、TOR、MRM、
碰撞前后窗口，复用 CRC / fsync / 轮转」。盘点发现 PEM 里此前只有
`degrade_transition` **一类**事件，L3 特有的四类**一个都没有**。

| 项 | 状态 | 落地 |
|----|------|------|
| 事件分类（Python 先行） | ✅ 完成 | `tools/l3_edr_sim.py`：五类事件（`l3_activation` / `l3_tor` / `l3_mrm` / `l3_odd_exit` / `l3_collision`）的**单一事实源** —— 每类声明来源 topic、触发条件、8 个字段名、是否 critical。`--check-taxonomy` 强制「每类都有触发条件 + 字段 + 关键标记 + 说明」，且 `derive_events()` 自检**边沿检测语义**（稳态不记、跃迁各记一次） |
| C 侧实现 | ✅ 完成 | `include/l3_edr.h` + `src/core/l3_edr.c`：状态码映射 + 8 槽字段清零辅助。单测 `test_l3_edr.c`（状态码未知必须 -1、不得伪造 OFF；事件名统一 `l3_` 前缀且唯一） |
| monitor 边沿检测 | ✅ 完成 | monitor 的 PEM 循环里对 `tor/state`、`odd/state` 做**跃迁检测**（稳态不写，否则 20Hz 重复事件会淹掉取证信息），五类事件全部 `critical=true` 走 fsync |
| 端到端门禁（有牙） | ✅ 完成 | `ci/gates/l3_edr_gate.py`：真起管线、真写 PEM、**读懂事件**，断言 L3 场景集齐四类 + 全部 fsync + 无未声明事件名。CI `integration-test` job 跑 |

**本轮抓出并修掉的真实缺陷**：

- 🔴 **EDR 的事件源订阅被关在 `if (!g.embedded_mode)` 里，而写 PEM 恰恰只在
  embedded（production）模式** —— 两个条件**永不同时成立**，于是 `l3_*` 事件
  一条都记不下来。实测证据：production 管线跑 L3 场景 30s，PEM 里只有
  `degrade_transition`。这也解释了为什么此前"PEM 有事件框架"却没有任何 L3 事件。
  修法：`odd/state` 与 `tor/state` 两条订阅移出该分支（体量极小，两种模式都订）。

- 🔴 **`l3_edr.c` 漏加进 CMake 的 `CORE_SOURCES`**，但 `plugin_symbol_check.py`
  仍报"OK (36 plugins)"。`nm` 显示 `libmonitor_node.so` 里
  `l3_edr_tor_state_code` / `l3_edr_values_init` 是 **undefined** ——
  `dlopen` 时会直接 'symbol lookup error' 弄死 launcher。
  **这是该闸门已知的覆盖盲区**（它只查"插件里定义但未解析的项目符号"，
  漏了"插件引用而核心库未编译进去"这一向）。已补进 CMake 并复验 `nm` 无 undefined。

**顺带说明一个设计约束（已写进门禁注释）**：EDR 要求"碰撞前后各 N 秒可回溯"，
本项只记碰撞**本身**这一条 —— 前后窗口由 PEM **持续记录**的周期性 topic
（`vehicle/state` 等）提供。故 PEM 必须持续记录这些量，不能只在事件时刻记。
碰撞事件的 `pre_window`/`post_window` 字段用于标记该窗口在 PEM 时间轴上的范围。

**验证**：40 ctest 全绿（新增 `l3_edr_tests` + `l3_edr_sim_tools`）；
`ci/gates/l3_edr_gate.py` 实测 PEM 内
`{l3_odd_exit:3, l3_activation:3, l3_tor:2, l3_mrm:4}` 且全部 critical/fsync；
`ci/gates/fault_inject_gate.py` 6/6 类仍全绿；9 条静态 gate 全绿。

**P2 遗留**：雷达传感器模型仍缺失（`sensor_model` 只有 lidar/gps/camera，
全仓无 radar）——这是 P2 冗余维度最后一块；另有"感知降级后系统行为是否仍安全"
的专项判据（需构造"盲开仍不撞"场景）。


### P2 进展（2026-10-11，第四刀：看门狗覆盖面）

P2 的「冗余（…、**看门狗**）」这一项此前只是**名义上存在**：`degrade_ladder` 的
supervisor 有完整的递进策略（L1/L2/L3 + 去抖 + 自动恢复），但**只有 3 个节点上报
心跳**（`control` / `fusion` / `planning`）——安全闸门 `safety_control` 自身、
整条感知链（`sensor_model` / `perception` / `object_tracker`）、决策层
`behavior_planner` 掉线时**没有任何机制发现**。

| 项 | 状态 | 落地 |
|----|------|------|
| 心跳覆盖 3 → 8 节点 | ✅ 完成 | 新增 `safety_control_node` / `perception_node` / `sensor_model_node` / `object_tracker_node` / `behavior_planner_node` 五个上报点。**`safety_control` 是最后一层保护，它掉线最不该无人发现** |
| 原因码不再指错层 | ✅ 完成 | `reason_for_node` 增感知链映射：`sensor_model`/`perception`/`object_tracker` 超时报 `sensor_timeout`，而不是此前的 `control_timeout`（后者会把人指向明明在跑的 control） |
| 表容量静默丢弃 | ✅ 完成 | `DEGRADE_MAX_NODES` 16 → 32。默认 `pipeline.json` 有 **19 个节点**，旧上限下第 17 个起的心跳被 `return` **静默丢弃** —— 运维侧看到的是"心跳都健康"的假绿。现在表满会 `LOG_ERROR` 点名（带防刷屏），并加注释说明为何是 32（与 `HEALTH_MAX_NODES` 对齐） |

**验证**：40 ctest 全绿（`safety_fault_evidence_tests` 新增看门狗覆盖面/容量/原因码
三类断言）；实测 `metrics.degrade.node_count` 由 3 → **8**，逐节点 age 均新鲜；
9 条静态 gate 全绿。

**门禁有效性自检（本项特别做了 A/B）**：把 `DEGRADE_MAX_NODES` 改回 16 重编译，
新增的 4 条断言**全部 FAIL**；还原为 32 后 PASS —— 证明这些断言不是恒真摆设。

### P2 进展（2026-10-11，第五刀：雷达传感器模型）

P2 的「冗余（**雷达模型**、…）」是最后一块空白：`sensor_model` 只有 lidar/gps/camera，
全仓**无 radar**。相机在夜间/逆光/恶劣天气显著退化、LiDAR 量程有限（默认 60m），
两者退化模式**相同方向**（都是"看不见"）；雷达补的正是这块短板。

| 项 | 状态 | 落地 |
|----|------|------|
| IDL 类型 | ✅ 完成 | `msg/adas_msgs.msg` 新增 `RadarTarget`（距离/方位/径向速度/RCS）+ `RadarFrame`（64 目标/帧）。**刻意设计字段顺序 + 显式 `reserved`**，使 wire size == struct size == 1048 —— 新增类型**不必**再往 `KNOWN_MISMATCHES` 里加一条（这是 `msg_layout_check.py` 当场抓出来的：原顺序下 wire=1044/struct=1048） |
| 观测模型 | ✅ 完成 | `modules/adas_nodes/radar_model.h`（header-only）。抽离成独立模块而非留在 `sensor_model_node.c` 里做 static —— 与 `lidar_scan` / `sensor_model_weather` 同一策略：**节点与单测链接同一份实现，无副本漂移** |
| 节点接线 | ✅ 完成 | `sensor_model` 新增 `sensor/radar` 发布（含 advertise）。`enable_radar` **默认 0** → 不加雷达的管线**行为零变化**（三份 pipeline 均只加声明与参数，不开开关） |
| 与 LiDAR 的差异（为何是冗余而非重复） | ✅ 完成 | 量程 200m vs 60m；只报方位角（无高度）→ 目标数远少于点云；报**径向速度**（LiDAR 单帧点云没有的直接量测）；**天气只衰减 RCS 幅度、不影响可达性** |
| 单测 | ✅ 完成 | 8 条雷达几何单测（量程/FOV 门限、方位角以车头为基准、径向速度符号、目标数截断容量守卫、同种子可复现）+ 1 条**经真实总线往返**测试（组帧→序列化→反序列化后目标内容不变） |

**最关键的断言**（写在单测里，是本项的**设计意图**而非附带检查）：
`test_radar_survives_bad_weather` —— 恶劣天气下雷达**仍报同一目标**（只降 RCS 幅度），
而相机可见度会被**乘性衰减**。这正是 P2 要的"任一模态退化时剩余模态仍能支撑安全"。

**验证**：40 ctest 全绿（`test_adas_nodes_logic` 87→95 项、`test_new_modules` 新增
总线往返）；9 条静态 gate 全绿（含 `msg_layout` — 新类型**零** padding 失配）；
实跑 `enable_radar=1` 时 `sensor/radar` 稳定 ~10Hz（182 帧/18s）；
`scenario_regression --baseline` 11/11 PASS 0 回归。

### P2 进展（2026-10-11，第六刀：UN R157 法规场景库）

P2 的「法规场景库（UN R157 ALKS + 国内 L3 测试规程：切入/切出、前车急刹、
静止障碍、施工区、ODD 退出、传感器失效注入）」此前只有零散覆盖。本刀补齐
**切入 / 切出 + 静止障碍**三类，并在补的过程中**抓出一个真实死锁**。

| 项 | 状态 | 落地 |
|----|------|------|
| `allow_npc_lane_change` 实现 | ✅ 完成 | `scene_events.cpp` 里**注释写着"若未来需要恢复 NPC 编导变道（如 cutin 测试场景），加 allow_npc_lane_change: true 开关"——但这个开关从未实现**（NPC 横控被无条件归零）。没有它，任何"NPC 切入 ego 车道"的法规场景在结构上都写不出来（写了也只是 NPC 在原车道直行 = 假绿）。已实现：`Choreography.allow_npc_lane_change`（默认 false，既有场景零变化）+ 解析 + 消费点条件判断 |
| `r157_cut_in` | ✅ 完成 | 旁车切入：邻道车用 `vy`（**连续**横向速度）切入本车道。**没用 choreography 的 `dl` 传送** —— 传送是位置跳变，实测会直接撞上（`dl` 适合"重置到 ego 前方"而非"切进来"）。判据：无碰撞 + 不超时 + 均速。实测 PASS |
| `r157_cut_out` | ✅ 完成 | 前车切出暴露静止障碍：本车道前车用 `vy` 切到外侧车道让开，同时正前方有静止车。实测 PASS（**修完死锁后**，见下） |
| 进 suite + 门禁 | ✅ 完成 | 两场景进 `suite.json`（13 个 enabled），由 `scenario_regression` 覆盖 |

**🔴 本刀抓出的真实死锁（由 r157_cut_out 当场暴露）**：

静止障碍停在 ego 正前方时，ego 正常跟停后**永久堵死**（实测
`stagnation_duration_s=44.6`、`lane_change_count=0`、avg_speed 0.40 m/s、
behavior 恒为 `FOLLOW`）。

根因在 `behavior_planner` 的超车价值判据：

```
worthwhile = blocked && (best_gap > min_gap)
min_gap    = min_overtake_gap_base(25.0) + rel_speed × 2.0
```

实测日志 `best_gap=7.2 blocked=1 worthwhile=0` —— ego 跟停在 desired_gap≈5m
（实测 7.2m），而 `min_gap` 是 **25m 起跳**，故 `best_gap > min_gap`
**恒不成立** → `worthwhile` 恒 false → 永不评估变道 → 永久堵死。

**关键洞察**：`min_gap` 的语义（"前方留够空间才**值得**超车"）对**运动**前车
成立（逼近过程中 best_gap 从大到小穿过阈值），但对**已经停在正前方**的障碍物
无解 —— **车越守规矩停得越近，越永远不绕行**。

**修法**：前车速度低于 `min_overtake_lead_moving_mps`（新参数，默认 0.5 m/s）
即视为"已停住"→ 绕行价值**恒真**，把"可不可行"交给下游的变道安全评估
（目标车道 rear/front gap）—— 那才是正确的守门人。运动前车仍走原判据，
避免低速跟车时误触发无效变道。

**修后实测**：avg_speed 0.40 → **14.26 m/s**，`lane_change_count` 0 → **4**，
stagnation 44.6s → 消失，场景 PASS。

**验证**：`r157_cut_in` / `r157_cut_out` 均 PASS；`behavior_fsm_check` 全绿
（转移表完整性未被破坏）；9 条静态 gate 全绿；`scenario_regression` 见下。

**P2 遗留**：法规场景库仍缺"前车急刹 / 施工区 / 传感器失效注入"三类；
另有"感知降级后系统行为是否仍安全"的专项判据（需构造"盲开仍不撞"场景）。


## 与近期工作的关系

- 近期回归修复见 `UPGRADE_DIRECTION.md`：`main` 先稳定在 `a065f26`，
  BEV 升级链保留在 `feature/bev-perception-upgrade`。
- 方向一是该分支的直接延伸；方向二~五是新能力，建议在各自特性分支上推进，
  经 `/verify` 与指标门禁后再合回 `main`。

---

*注：本文为长期方向性路线图，非实施补丁。各方向落地时再拆为可验证的小步提交。*
