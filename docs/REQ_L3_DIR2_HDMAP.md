# 需求规格：L3 方向二 — HD-Map 自动化（OSM → Lanelet2 + 车道级消费侧落地）

> 文档定位：路线图（`docs/ROADMAP_L3.md` 方向二）的**可执行规格**。
> 不是教程（教程在 `docs/book/`），不是运维交接（交接在 `docs/HANDOFF_*.md`）。
> 读完这份，下一步该写什么代码、改哪些 msg 契约、加哪些 gate，应该**毫无歧义**。
>
> 关联：
> - 路线图：`docs/ROADMAP_L3.md` 方向二
> - 已落地（仅诊断透传）：`modules/adas_nodes/flowsim_node.cpp` 发布 `localization/lane_match`，`modules/adas_nodes/monitor_node.c` 订阅做对比
> - 已落地（OSM 链路雏形）：`tools/osm2kmap.py` / `tools/osm_to_map.py` / `maps/osm_lujiazui_v2/` / `scenarios/city_comprehensive.json`
> - 抄作业源：Lanelet2（`fzi-forschungszentrum-informatik/Lanelet2`）、OSM（`openstreetmap/osm2lanelet`）

---

## 0. 一句话目标

> **让 planning 真正消费 ego 在车道级 lanelet 上的位置；fusion 仅在 obs 上挂 lane_match hint（不下结论、不丢 obs，遵守 CLAUDE.md 职责铁律）；而不是把 `lane_match` 只当诊断量透传。**

这是方向二"杠杆点"——一旦落地，方向三（OSM 路线分配 → 批量 NPC）、方向四（地图参考规划）、方向五（场景 DSL 用真实路网生成）三条线都同时被打开。

---

## 1. 背景与现状

### 1.1 当前状态（2026-09-24）

| 能力 | 状态 | 位置 |
|---|---|---|
| OSM → flowsim 内部 `map.json` | ✅ 已有 | `tools/osm2kmap.py` 输出 `maps/osm_lujiazui_v2/map.json` |
| flowsim 内部车道几何（road/lane centerline）| ✅ 已有 | `modules/adas_nodes/flowsim/road_network.cpp` |
| flowsim 发布权威 `localization/lane_match` | ✅ 已有（诊断透传）| `flowsim_node.cpp` `publish_lane_match` |
| monitor 订阅 `lane_match` 并对比快照 vs 权威 | ✅ 已有 | `monitor_node.c` `on_lane_match` |
| Lanelet2 表达 / 地图匹配库 / 标准 primitive | ❌ 缺失 | — |
| planning 消费 `lane_match` 字段做横向规划 | ❌ 缺失 | 现状用 `lane_d` 启发式 + `road_pos.world()` 切线 |
| fusion 给 obs 挂 lane_match hint | ❌ 未做 | v0.2 引入 D2-07；本期不允许 fusion 做硬过滤（CLAUDE.md 职责铁律）|
| OSM 转标准 lanelet2 XML（可被 Lanelet2 库直接读）| ❌ 缺失 | 现有 `map.json` 是 flowsim 内部格式 |
| 信号灯 / 交规 / 限速 / 停止线 | ⚠️ 部分 | 红绿灯有，停止线、限速 sign 没结构化 |

### 1.2 痛点（为什么必须做）

- `flowsim_node.cpp` 主循环 ego 段每次发布 `vehicle/state` 时，**横向参考系用的是"车道 centerline 启发式 + road_pos.world() 切线"**，不是 Lanelet2 标准；
- planning 的 `target_lane_offset` / `project_to_reference_path` 全靠这套启发式横向参考，**遇到单向路 / 弯道 / 路口 lane 数变化时已经多次出 bug**（见 HANDOFF 故障表："变道冲出车道"、"右转支路场景 control 跟踪坏轨迹"等）；
- `localization/lane_match` 已经能算出来，但**全栈没人接**——只是 monitor 拿来对比"快照是不是和权威值漂了"，定位上算"鸡肋功能"；
- 方向三想用 OSM 路网做 NPC 路线分配，方向五想做"在真实路网上随机化场景"，**两件事都需要一个标准 lanelet 表达**才能跨节点传递。

### 1.3 抄作业选型

| 选项 | 选 / 不选 | 理由 |
|---|---|---|
| **Lanelet2**（FZI）| ✅ 选 | C++ 头文件库、HD-Map 事实标准、`lanelet2_matching` 子库直接给"对齐到车道级"算法 |
| OpenDRIVE + esmini（已在子模块）| ⚠️ 部分 | esmini 已在用、OpenDRIVE 适合做"渲染/路网拓扑"，但**不适合做高频规划输入**（每次插值都重算，planning 跑 50Hz 顶不住） |
| Apollo HD-Map | ❌ 不选 | 格式封闭、protobuf 强绑定、Baidu 内部协议 |
| 自己造 | ❌ 不选 | 路线图已明确否决 |

**结论**：Lanelet2 做"权威表达 + 地图匹配"，OpenDRIVE 通过 esmini 继续承担"渲染/轨迹回放"分工。

---

## 2. 范围

### 2.1 In Scope（本期必做）

| ID | 能力 | 验收形式 |
|---|---|---|
| **D2-01** | Lanelet2 作为 git submodule / third_party 引入，构建可链接 | `cmake --build` 通过、新增 `find_package(Lanelet2)` 或 `add_subdirectory` |
| **D2-02** | `map.json` → Lanelet2 OSM XML（.osm）转换器，可逆或可重生成 | `tools/json_to_lanelet.py` 跑通，产出 `maps/osm_lujiazui_v2/lanelet.osm`，单测覆盖 lane/area/regulatory element |
| **D2-03** | flowsim 启动期加载 `.osm` 一次，构建**只读**的 `lanelet::LaneletMap` 内存对象 | flowsim 启动日志打印 `loaded N lanelets M areas K regulatory_elements` |
| **D2-04** | `localization/lane_match` 消息契约扩展：M2 增加 `llt_id` / `llt_s` / `llt_offset` / `llt_heading_err_rad` / `valid`（**5 个 cJSON 字段**）；M3 再追加 `curvature` / `lane_width` / `left_lanelet_id` / `right_lanelet_id` / `flags`（**再 5 个，累计 10 个**）| `ci/gates/lane_match_schema_check.py` 守 cJSON schema（M2: 5 字段 / M3: 10 字段）；`topic_contract_check.py` **不动**（v0.2 已纠错，本格不再列）；旧消费方（仅 monitor）天然兼容 cJSON 字段追加 |
| **D2-05** | map-matching 算法集成：每帧 `vehicle/state` → `lanelet2_matching::matchObject` → 1 个 lanelet + (s, offset, heading_err) | 与现有权威解算（`flowsim_node.cpp` 内的 ground-truth）对齐到厘米级；CI 单测覆盖：直道、弯道、十字路口、掉头段 |
| **D2-06** | planning **真正消费** `lane_match.llt_id` + `llt_offset` 做横向规划：替代 `target_lane_offset` 启发式 | `planning_node.cpp` 删除 `target_lane_offset` 旧分支；新增 1 条门禁："scenario_regression 默认场景下，ego `llt_offset` 全程绝对值 ≤ `lane_width/2 - 0.1m`" |
| **D2-07** | fusion **只在 obs 上挂 `obs_lane_match_hint: bool` 提示**，不下结论、不丢 obs | `fusion_node.cpp` 订阅 `lane_match`，obs 序列化时附加 `obs_lane_match_hint: bool`（**全程统一字段名**；hint=true = 该 obs 与 ego 在同一 lanelet 或相邻车道 ±1）。planning 自己消费 hint + lane_match 决策。门禁：obs 总数与切 hint **前后一致**（0 obs 不能被静默丢成 N），且 `obs_lane_match_hint=true` 的 obs 数 ≥ 真值同/邻车道 obs 数的 95% |
| **D2-08** | 信号灯 / 停止线 / 限速**结构化**（Lanelet2 `regulatory_element`，**仅静态子集**）| 红绿灯已经在跑；停止线 / 限速 sign 在 lanelet.xml 里表达，flowsim 加载并发布到 `road/traffic_lights` / `road/traffic_signs` 已有 topic（topic 契约不变，仅内容扩展）。**明示不含路权仲裁（Right-of-Way）/ 让行冲突图 / 动态交规决策** —— 路线图原文"带拓扑/车道/交规/信号灯"中的"交规"是广义的（含 Right-of-Way 仲裁图），本期仅做静态设施表达，路权仲裁推到方向二 v2 |
| **D2-09** | CI gate：`lanelet_consistency_check.py` —— 校验 `map.json` ↔ `lanelet.osm` 双向覆盖（每个 map.json 的 lane 在 .osm 里能找到，反之亦然）| `ci/gates/lanelet_consistency_check.py`，PR 必须绿 |
| **D2-10** | `--repeats N` 在方向二覆盖场景上跑稳定（直道/弯道/路口/掉头段都不抖）| `curve_road` / `straight_road` / `city_comprehensive` / `urban_challenge` 共 4 个场景 × `--repeats 3` 全 PASS |

### 2.2 Out of Scope（本期明确不做）

**显式不做（路线图内、本期推迟）**：
- ❌ **不**做完整 OSM → lanelet 自动转换（POI / building / 复杂交叉口语义先空）；
- ❌ **不**做 OpenDRIVE ↔ Lanelet2 双向转换（esmini 链路维持现状，只读 .xodr 不写 .osm）；
- ❌ **不**换规划算法本体（Frenet / ST / LTV-MPC 全保留，只换横向参考系的输入来源）；
- ❌ **不**上雷达 / 相机模型（属于方向一）；
- ❌ **不**改 SUMO 流量注入（属于方向三）；
- ❌ **不**做"动态障碍物也匹配到 lanelet 上"（NPC / 行人只在 flowsim 物理里跑，不进 lanelet graph）；
- ❌ **不**做 RTK/GNSS 真实定位（仍用 flowsim ground-truth 喂 `vehicle/state`，定位噪声先关）；
- ❌ **不**改仪表盘 3D 渲染管线（esmini/OpenDRIVE 渲染链路继续承担）；
- ❌ **不**做"开放街道全球数据导入 UI"（命令行批量即可）。

**显式不做（路线图没写但容易被默认追问的）**：
- ❌ **大地图 Tiling / Streaming / 内存卸载**：本期只支持单场景小型地图（≤ 10k lanelets）常驻内存。超大范围路网动态切片加载属于方向二 v2（agi 评审追加），本期不在范围内。
- ❌ **3D 高程 / 坡度 / 立交桥 Z 轴匹配**：本期仅做平面 UTM 2D Frenet 投影，不支持多层立交、坡道 Z 轴坐标匹配、`<elevationProfile>` 高程解析（即便 esmini 链路支持）。所有路口视为平面交叉。

### 2.3 不做就会出事（v0.2 加严）

1. **不做 D2-06（planning 消费 lane_match）**，只做 D2-01~05 的工具层 = "鸡肋"现状重演。**D2-06 是本期成败线**：planning 真改了横向规划参考系、CI 真有 gate 守着 `max_lane_offset_m` 不越线，才算方向二落地。
2. **不做 D2-05 Temporal ROI Cache**，A9（< 0.5ms）在 10k lanelets 的 CI 机器上必 flake，整条 CI 红绿不稳、debug 噩梦。
3. **不做 A11 匹配失效降级**，`valid=0` 时 planning 会因缺横向参考系进入 DATA_TIMEOUT（参见 [HANDOFF_2026-09-23.md §2](HANDOFF_2026-09-23.md) 同类故障）；safety_control 兜底也救不了横向轨迹。
4. **若 D2-07 不改成"fusion 只打 hint"**，fusion_node 会越权做车道过滤，违反 CLAUDE.md 职责铁律；map-matching 一旦误跳变，obs 被静默丢成 0，safety_control 近场 TTC 失去目标。

---

## 3. 用户场景（User Stories）

| 角色 | 场景 | 期望 |
|---|---|---|
| **算法开发**（自己）| 在 `city_comprehensive` 跑场景，想知道 ego 当前在 OSM 哪条 lanelet 上、距路口多远、横向偏多少 | monitor/日志里能直接看 `llt_id=road_r10260585s0.lane.1, s=145.3m, offset=+0.42m` |
| **算法开发**| 想在 `maps/osm_lujiazui_v2` 加一条新路（OSM 拉个 bbox 重跑转换器）| `bash tools/build_lanelet.sh osm_lujiazui_v2` 一条命令，CI 自动跑 consistency gate |
| **planning 同学**| 想验证"目标车道偏移 0.3m"在弯道是否被严格执行 | `demo_evaluator` 加一条判据：`max(\|llt_offset\|) ≤ 0.4`；弯道场景默认场景下要红就红，要绿就绿，不靠运气 |
| **fusion 同学**| 想用"同车道前车"做 ACC 跟车，不用管对向车的"幽灵 TTC" | fusion 订阅 `lane_match`，给每个 obs 打 `obs_lane_match_hint: bool`；planning 自己决定怎么用 hint。**fusion 不做硬过滤**（CLAUDE.md 职责铁律），obs 一个不丢 |
| **方向三同学**| 想"在 OSM 路网上批量生成 NPC 流量" | 拿到 `LaneletMap` 对象后，调 Lanelet2 routing API 拿候选路径，剩下的事就是 SUMO 接入 |
| **方向五同学**| 想"在某 lanelet 上随机注入切入事件做模糊测试" | fixture 里写 `llt_id=road_xxx.lane.2, s=120.5`，flowsim 按 ID 定位注入点 |
| **CI**| 担心"加了新场景但忘了同步 lanelet.osm"导致 PR 漂移 | `lanelet_consistency_check.py` 在 PR 跑挂掉，错误信息直接说"map.json 多 lane X、.osm 找不到对应 llt_id" |

---

## 4. 功能需求（Functional Requirements）

> 编号格式 `FR-<子模块>-<序号>`。所有 FR 都必须能被一个单测或一个 gate 验证。

### 4.1 数据 / 工具层

#### FR-OSM-01：OSM → Lanelet2 OSM XML 转换
- **输入**：`maps/<name>/map.json`（现有 flowsim 内部格式）
- **输出**：`maps/<name>/lanelet.osm`（Lanelet2 标准 OSM XML）
- **必含元素**：`<way>` (lanelet 边界)、`<relation type="lanelet">`、`<relation type="regulatory_element">` (traffic_light / stop_line / speed_limit)
- **必含属性**：`type`、`subtype`、`oneway`、`speed_limit`、`vehicle`、`location` (UTM 投影)
- **错误**：map.json 缺失 `roads[].lanes[].centerline` → 退出码非零 + 行号
- **测试**：`tests/test_json_to_lanelet.py`，用例：直道 / 弯道 / 十字路口 / 单向多车道 / 双向路 / 含信号灯路口

#### FR-OSM-02：OSM 转换的可重生成
- 同输入字节级相同输出（同 OSM 文件 mmap 比对通过）
- 实现要点：写 XML 时属性顺序固定、双精度坐标固定 6 位小数、relation member 排序稳定

### 4.2 运行时 / 集成层

#### FR-RT-01：flowsim 启动加载 `.osm`
- 启动期一次 `lanelet::load("maps/.../lanelet.osm")`，构建 `lanelet::LaneletMap`
- 失败 → 启动失败（exit 1）+ 错误日志带文件路径
- 启动日志：`[flowsim] loaded lanelet map: N lanelets, M areas, K regulatory_elements, took Xms`

#### FR-RT-02：`localization/lane_match` 消息契约扩展（cJSON 路径）
- 该 topic 当前是 cJSON 字符串（见 §6.1），**加字段即追加 JSON key**，零破坏
- M2 追加 **5 字段**：`llt_id` / `llt_s` / `llt_offset` / `llt_heading_err_rad` / `valid`
- M3 再追加 **5 字段**（累计 10 个 cJSON key）：`curvature` / `lane_width` / `left_lanelet_id` / `right_lanelet_id` / `flags`
- 契约守护靠 `ci/gates/lane_match_schema_check.py`（FR-CI-01 的姊妹 gate），不靠 `topic_contract_check.py`（后者不约束 cJSON 内容）
- 现有唯一消费方 `monitor_node.c:898` 只读整个 buffer 做诊断对比，新字段对 monitor 零影响

#### FR-RT-03：map-matching 算法（含 Temporal ROI Cache 前置约束）
- 每帧 `vehicle/state` 输入，输出 1 个最优匹配（LLT id, s, offset, heading_err）
- 算法选择：**自写"投影 + 邻域最近"**，不直接用 `lanelet2_matching::matchObject`（库默认 ICP 噪声大，规划 50Hz 跑不动；详见 M2 设计文档）
- **前置约束（Temporal ROI Cache）**：每帧先在"历史 `llt_id` 的拓扑邻域"内做局部投影 —— 即 ego 当前所在 lanelet 前后 ≤ 3 个 successors + 左右 ≤ 1 邻 lanelet（共 ≤ 11 个候选）。命中即返回，未命中再降级到全图 R-tree 查询。该约束是 A9（< 0.5ms P99）的可达性前提。
- 单帧预算：< 0.5ms (P99, 10k lanelets 内)
- 对齐精度：与现有 flowsim 权威解算（ground-truth）误差 ≤ 0.05m (offset)、≤ 0.02rad (heading_err)
- **失效处理**：所有候选均失败 → `valid=0`；触发 A11 降级状态机（见 §7.1）

#### FR-RT-04：planning 消费 lane_match
- `planning_node.cpp` 删除 `target_lane_offset` 启发式分支（详见 `UPGRADE_DIRECTION.md` §5.3）
- 横向规划参考系改用 `lane_match.llt_id + llt_offset + llt_s`
- 切换行为：行为决策 FSM 不变（lane_change / uturn 仍由 behavior_planner 决定）、规划只接"在哪条 lanelet 上、横向偏多少"作为输入
- 兼容性：map_ref 路径仍保留（map_ref_count > 0 时继续走 route 中心线，与 lanelet 不冲突；map_ref_count = 0 时回退到 lanelet-only）

#### FR-RT-05：fusion 仅打 hint，不做车道过滤（守住职责铁律）

> **设计动机**：[CLAUDE.md 模块职责铁律](../CLAUDE.md) 明文："跨界写逻辑的代码不会被合并"。fusion 的职责是 EKF 状态估计（5D 状态向量 `x,y,v,heading,yaw_rate`），"obs 是否在 ego 车道"是**规划前决策语义**，fusion 没有资格下结论。若在 fusion 做硬过滤，一旦 map-matching 抖动或误跳变，会把 obs 静默丢弃，直接剥夺下游 `safety_control` 近场 TTC 刹停兜底机会。

- `fusion_node.cpp` 订阅 `localization/lane_match`
- 在 obs 序列化时附加 `obs_lane_match_hint: bool`（true = 该 obs 与 ego 在同一 lanelet 或相邻车道 ±1）
- **不**做硬过滤、不丢 obs（保留所有 obs 给 safety_control 兜底）
- planning 自己消费 `lane_match.llt_id` + `obs.hint` 决定怎么处理（不消费 hint 也行，hint 是软提示）
- 单测：`test_fusion_lane_hint` 覆盖：同车道 / 相邻车道 / 对向 2 车道外 / 锥外 均正确打 hint；**obs 总数与打 hint 前完全一致**（最强不变量）

### 4.3 CI / Gate 层

#### FR-CI-01：`lanelet_consistency_check.py`
- 比对 `map.json` ↔ `lanelet.osm`：
  - 每个 `map.json` 的 lane 在 .osm 里能找到对应 llt（按 centerline 起点距离 ≤ 1.0m 匹配）
  - 每个 .osm 的 lanelet 在 .osm `map.json` 里有对应记录
- 失败信息要可定位（带 map.json 行号 / .osm node ref）
- PR 必须绿，否则合不进 main

#### FR-CI-02：`scenario_regression` 加新门禁
- 在 `scenarios/suite.json` 加字段 `max_lane_offset_m`（注：仓库无 `tests/baseline/`，suite 在 `scenarios/suite.json`）（默认 = `lane_width/2 - 0.1`）
- `demo_evaluator.py` 采集 `llt_offset` 全程绝对值最大值，超阈值 → FAIL
- 4 场景（curve_road / straight_road / city_comprehensive / urban_challenge）× `--repeats 3` 全 PASS

#### FR-CI-03：sensor-wiring gate 与 topic-contract gate 分工明确
- `sensor_wiring_check.py` 维持**纯粹 LiDAR 参数校验**职责（量程门 / 频率门 / 噪声门），**不**承担 topic 拓扑检查
- sensor 编排（`pipeline_sensor.json`）的 topic 拓扑守门走 `topic_contract_check.py --config config/pipeline_sensor.json`（这是 `topic_contract_check.py` 既有的能力）
- 本期新增：两个 gate 都加一行，验证 `pipeline_sensor.json` 里 `localization/lane_match` 既在 publisher 的 `s_outputs`、又在 subscriber 的 `s_inputs`

---

## 5. 非功能需求（Non-Functional Requirements）

| ID | 项 | 指标 | 验证 |
|---|---|---|---|
| NFR-01 | 单帧 map-matching 耗时 | < 0.5ms (P99) | 单测 + `quick_verify --bench` |
| NFR-02 | 启动加载 `.osm` 时间 | < 500ms（10k lanelets 内）| 单测 + 启动日志断言 |
| NFR-03 | 内存占用 | `LaneletMap` < 200MB（10k lanelets） | `valgrind --tool=massif` 截图存档 |
| NFR-04 | 横向规划稳定性 | `llt_offset` 全程不超 `lane_width/2 - 0.1m` | `demo_evaluator` 新门禁 |
| NFR-05 | 既有 baseline 兼容 | 切到 lanelet 参考系后，4 场景 `--repeats 3` 全 PASS 且 `avg_speed_mps` 与 baseline 偏差 ≤ 5% | `scenario_regression --baseline` |
| NFR-06 | API 稳定性 | Lanelet2 升级到上游新版本时，本地 wrapper 改动 ≤ 1 文件 50 行 | 单测隔离，wrapper 仅暴露本项目用的子集 |
| NFR-07 | 可重生成性 | 同一 `map.json` 跑转换器字节级相同输出 | 单测 `test_json_to_lanelet_deterministic` |
| NFR-08 | CI 跑时长 | `lanelet_consistency_check.py` < 5s | gate 自带超时 |

---

## 6. 接口契约（Interface Contracts）

### 6.1 msg 契约（cJSON 路径，零破坏）

> **重要事实**（v0.2 纠正 v0.1 错误）：`localization/lane_match` 当前是 **cJSON 字符串**，不是 `msg_codegen` 二进制 struct。证据：
> - 生产端 `flowsim_node.cpp:1217`：`cJSON_AddNumberToObject(j, "id_mismatch_frames", ...)`
> - 消费端 `monitor_node.c:903`：`memcpy(g.lane_match_json, msg->data, copy)` + `cJSON_Parse`
>
> 因此 v0.1 里"加字段 + DEPRECATED 旧字段 + msg_layout_check.py 守 schema"全部错误。cJSON 天然字段自描述，**加字段零破坏**，不需要 DEPRECATED 流程。`msg_layout_check.py` 不适用本 topic。

**新增字段**（直接追加到现有 cJSON 对象）：
```json
{
  "id_mismatch_frames": <int>,
  "frames": <int>,

  // [NEW, M2 起使用] Lanelet2 匹配输出
  "llt_id": <uint64>,            // Lanelet 全局 ID；0 = 未匹配
  "llt_s": <double>,             // 沿车道中心线弧长 (m)
  "llt_offset": <double>,        // 横向偏移 (m, 左正右负)
  "llt_heading_err_rad": <double>,// 车头 vs 车道切线 (rad)
  "valid": <int>                 // 0 = 匹配失效 (车不在路网内)
}
```

**M3 起追加**（与 §6.2 wrapper 字段对齐）：
```json
{
  "curvature": <double>,         // 1/m
  "lane_width": <double>,        // m
  "left_lanelet_id": <uint64>,   // 0 = 无左邻
  "right_lanelet_id": <uint64>,  // 0 = 无右邻
  "flags": <uint32>              // ABI 保留位（当前未用，置 0）
}
```

**契约守护**：新增 `ci/gates/lane_match_schema_check.py`，要求所有 **5 个（M2）/ 10 个（M3）** cJSON key 必须存在、类型必须匹配；字段缺失或类型不符 → exit 1。**M2 → M3 演进是纯增量**：每次只追加 key，不删不改老 key，旧消费方天然兼容。**替代** v0.1 误指的 `topic_contract_check.py`（后者只检查 C struct 的 `s_inputs/s_outputs` 拓扑声明，不约束 cJSON 内容）。

**下游迁移**：现有唯一消费方 `monitor_node.c:898` 只读 `lane_match_json` 整个 buffer 做诊断对比，新字段对它零影响。

### 6.2 C++ API 契约（v0.2 扩充）

**新增头文件**：`include/lanelet_match.h`（薄 wrapper，避免直接 include Lanelet2 头文件污染下游）

**M2 起发布的 struct**（最小可用集）：
```c
typedef struct {
    uint64_t llt_id;               // Lanelet 全局 ID；0 = 未匹配
    double   llt_s;                // 沿车道中心线弧长 (m)
    double   llt_offset;           // 横向偏移 (m, 左正右负)
    double   llt_heading_err_rad;  // 车头 vs 车道切线 (rad)
    int      valid;                // 0 = 匹配失效 (车不在路网内)
} lanelet_match_t;
```

**M3 起发布的 struct**（planning 真切换前必须有 planning 前馈/拓扑需求）：
```c
typedef struct {
    /* === M2 字段 === */
    uint64_t llt_id;
    double   llt_s;
    double   llt_offset;
    double   llt_heading_err_rad;
    int      valid;

    /* === M3 起使用（规划前馈 + 变道拓扑） === */
    double   curvature;            // 1/m, 0 = 直线, 正 = 左弯
    double   lane_width;           // m, 0 表示未知
    uint64_t left_lanelet_id;      // 0 = 无左邻
    uint64_t right_lanelet_id;     // 0 = 无右邻

    /* === ABI 保留位（v0.2 起预留，禁用前不读不写） === */
    uint32_t flags;                // 当前必须置 0；M4 起按位启用
    uint32_t reserved;             // padding，未来扩展
} lanelet_match_t;
```

**配套 API**：
```c
// 启动期调一次
int lanelet_map_load(const char* osm_path);

// 每帧调一次（内部用 Temporal ROI Cache，见 FR-RT-03）
lanelet_match_t lanelet_match_pose(double x, double y, double heading_rad);
```

**下游消费方接口**：planning / fusion 只 include 这个 wrapper，不直接 include Lanelet2。**禁止**在 consumer 内部做 map-matching 二次开发（CLAUDE.md 职责铁律：去抖收敛在 localization 层）。

### 6.3 Python API 契约

**新增**：`tools/json_to_lanelet.py` CLI
```bash
python3 tools/json_to_lanelet.py \
    --input maps/osm_lujiazui_v2/map.json \
    --output maps/osm_lujiazui_v2/lanelet.osm \
    [--verify]  # 同时跑反向校验
```

**新增**：`ci/gates/lanelet_consistency_check.py`（同 §4.3 FR-CI-01）

---

## 7. 验收标准（Acceptance Criteria）

### 7.1 必须全绿才算 D2 完成

| # | 标准 | 验证命令 | 期望 |
|---|---|---|---|
| A1 | Lanelet2 子模块拉取并构建通过 | `git submodule update --init third_party/lanelet2 && cmake --build build` | exit 0 |
| A2 | 4 场景转换器单测全绿 | `pytest tests/test_json_to_lanelet.py -v` | 全部 PASS |
| A3 | flowsim 加载 `.osm` 启动日志含 `loaded N lanelets` | `bash scripts/demo.sh --no-browser 5 2>&1 \| grep loaded` | 行存在 |
| A4 | `lane_match_schema_check.py` 守 cJSON schema（M2: 5 字段；M3: 10 字段累计），`topic_contract_check.py` 不动 | `python3 ci/gates/lane_match_schema_check.py` | exit 0 |
| A5 | `lanelet_consistency_check.py` 对 4 个 map 全绿 | `python3 ci/gates/lanelet_consistency_check.py` | exit 0 |
| A6 | 4 场景 × `--repeats 3` sensor + 默认编排全 PASS | 见 §4.3 FR-CI-02 命令 | 8/8 × 2 编排全 PASS |
| A7 | `max_lane_offset_m` 门禁在 4 场景全绿 | 同上 | 零 FAIL |
| A8 | 与 baseline 偏差 ≤ 5% | `scenario_regression --baseline` 输出 | `summary["_spread"]` 内 |
| A9 | 单帧匹配 < 0.5ms | `quick_verify --bench lanelet_match` | P99 < 500μs |
| A10 | 故障模式表 CLAUDE.md 新增 2 条 | 编辑后 git diff | 提交 |
| **A11** | **匹配失效降级**：`valid=0` 时 planning 走 fallback 巡航（沿 `road/ref_path` + 安全车道保持 + 上报 LANE_MATCH_INVALID 状态），不报 DATA_TIMEOUT、不丢帧、不进 U_TURN | `tests/test_planning_fallback_when_match_invalid.cpp` 单测 + 4 场景注入一次匹配失败（人为把 wrapper 返回 `valid=0`），验证 ≤ 0.5s 内回到正常匹配状态 |

### 7.2 必须**主动**演示的对比（before/after）

- `straight_road` 默认场景：横向跑偏问题（见 `UPGRADE_DIRECTION.md` §1）即使保留 a5509c5 BEV 回归，**横向不漂**（因为横向规划不再依赖 BEV）
- `city_comprehensive` 默认场景：路线跟随 + 路口切换不再需要 `target_lane_offset=0` 兜底分支
- A/B：planning 切到 lane_match 前后，`max|llt_offset|` 实测对比图（落 `docs/evidence/REQ_L3_DIR2/before_after.json`）

---

## 8. 风险与开放问题（Risks & Open Questions）

### 8.1 风险

| ID | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | Lanelet2 升级破坏 ABI | 中 | 高 | NFR-06 wrapper 隔离；锁版本 v1.2.x |
| R2 | map-matching 在掉头段（ego 朝向 180° 翻）匹配错 ID | 中 | 中 | map-matching 加"近 50ms 历史 ID 优先"先验；单测覆盖掉头段 |
| R3 | planning 切换横向参考系导致 baseline 全红 | 高 | 高 | 旧 `target_lane_offset` 分支保留作 fallback，新逻辑灰度在 `pipeline_experimental_lanelet.json` 跑通再切默认 |
| R4 | esmini 渲染链路与 lanelet 不一致（不同坐标系）| 低 | 中 | 维持两条链路：渲染走 .xodr、规划走 .osm；规划侧把坐标统一到 map.json 起点 |
| R5 | sensor 编排下 BEV 回归未修，切 lane_match 后横向漂 | 中 | 中 | D2-06 与 BEV 修复**解耦**：先在 GT 编排跑通 lane_match 链路，sensor 编排等 BEV 修复合 main 后再切 |
| R6 | 路口 lanelet 分叉时匹配跳变（lane_id 1↔2 抖动）| 中 | 中 | map-matching 加平滑（前后帧同 road 优先） |

### 8.2 开放问题（需用户拍板）

| ID | 问题 | 选项 | 我的推荐 |
|---|---|---|---|
| Q1 | Lanelet2 用系统包还是 submodule？ | (a) system apt (b) git submodule | (b) submodule，可锁版本、CI 不用装系统包 |
| Q2 | map-matching 库函数还是自写？ | (a) `lanelet2_matching::matchObject` (b) 自写投影+邻域 | (b) 自写；库默认 ICP 噪声大、自写可控 |
| Q3 | planning 切换用特性分支还是直接 main？ | (a) `feature/lanelet-planning` (b) main | (a) 特性分支灰度，4 场景 × 3 重复合后再合 main |
| Q4 | lane_match 消息改名 v2 还是直接加字段？ | (a) 新 topic `lane_match_v2` (b) 旧 topic 加字段 | (b) 直接加字段。cJSON 天然字段自描述，加字段零破坏；现有消费方只有 monitor，对它零影响；CLAUDE.md 严禁 `_v2` 并存。**契约守护走新增的 `lane_match_schema_check.py`** |
| Q5 | 这次要不要顺便把 BEV 直通回归修了？ | (a) 顺便修 (b) 不修 | (b) 解耦，BEV 单独一支，方向二不背锅 |

---

## 9. 里程碑（Milestones）

> v0.3 节奏调整（agi 评审反馈）：总周期 **6 周 → 8 周**。
> - **M1 从 1~2 周 → 2~3 周**：Lanelet2 自身含 Eigen + Boost + pugixml 依赖，submodule 拉取 + cmake 集成 + 构建矩阵（debug/release/asan）单独跑通需预留时间；转换器单测要覆盖 6 个用例拓扑。
> - **M3 从 5~6 周 → 6~8 周（且工作量未减）**：横向参考系切换会触发 Frenet 参考线 + Stanley/LTV-MPC 控制器参数重调（参考 [CLAUDE.md](../CLAUDE.md) 历史上掉头死锁 / S 弯 4 层连环的沉淀），agy 建议预留 3 周以上调控制器 + 4 场景 × `--repeats 3` 全稳；M2 / M3 之间保留 1 周缓冲应对 M2 收尾的集成震荡。
>
> 每个里程碑结束都跑 `scenario_regression --repeats 3` + 三个 gate。

### M1：工具 + 数据层（第 1~3 周）

- D2-01（Lanelet2 引入 + cmake 集成）、D2-02（json→osm 转换器）、D2-09（consistency gate）
- 产出：`maps/<name>/lanelet.osm` 文件、`tools/json_to_lanelet.py`、`ci/gates/lanelet_consistency_check.py`、Lanelet2 build 通过
- 验证：A1 / A2 / A5

### M2：运行时 + 消费侧（第 4~5 周）

- D2-03（flowsim 加载）、D2-04（msg 契约扩展 + 新 gate `lane_match_schema_check.py`）、D2-05（map-matching + Temporal ROI Cache）
- 产出：flowsim 启动日志含 lanelet 信息、`localization/lane_match` 5 字段落地（M2 版）、wrapper 头文件 5 字段版
- 验证：A3 / A4（重写后） / A11

### M3：planning/fusion 接入 + gate（第 6~8 周）

- D2-06（planning 切换 + Stanley/LTV-MPC 控制器参数重调）、D2-07（fusion 加 hint）、D2-08（信号灯/停止线结构化）、D2-10（repeats 稳）
- 产出：4 场景 × 3 repeats × 2 编排全绿、`max_lane_offset_m` 门禁守护、wrapper 11 字段版（M3 版）
- 验证：A6 / A7 / A8 / A9 / A10

---

## 10. 与现有体系的对齐

### 10.1 模块职责铁律（不破）

- flowsim：**只读** lanelet，**不修改**（仍然是仿真器，不是地图编辑器）；
- planning：仍是"速度与轨迹的唯一权威"，只是横向参考系换成 lane_match；
- fusion：仍是"obstacles → tracked_objects"的责任，多了一个 `obs_lane_match_hint` 标记维度（提示该 obs 与 ego 同/邻车道）；**不**做硬过滤、**不**替 planning 做目标选择（CLAUDE.md 职责铁律）；
- safety_control：与本方向无直接接口。

**v1.0 状态对齐（2026-09-26）**：
- `ab05f6e feat(vis): 车道可观测性叠加层 —— 规划/地图/定位/感知四方对账` 已在 main 落地：lane_match / planning_debug / lane_data（启发式）/ perceived_lanes 四组数据在同一 3D 屏可对账。这正是 D2-06（planning 真切换横向参考系）的**前置**——可视化先让横向跑偏问题可被肉眼定位，D2-06 在它的基础上完成"让 planning 决策也消费 lane_match"。
- 本规格 v1.0 与 `ab05f6e` 不冲突：`ab05f6e` 没动 `localization/lane_match` cJSON 契约本身（仅透传到前端），因此 §6.1 的 cJSON 字段扩展计划仍按原定节奏推进。

### 10.2 既有门禁联动

| 既有 gate | 改动 |
|---|---|
| `topic_contract_check.py` | **不动**（不约束 cJSON 内容；v0.1 误改） |
| **`lane_match_schema_check.py`**（新增）| **M2 起守 cJSON schema**：5 字段（M2）/ 10 字段累计（M3）必须存在、类型必须匹配；缺失或类型不符 → exit 1 |
| `sensor_wiring_check.py` | **不动其 LiDAR 校验职责**；仅新增 sensor 模式下 `localization/lane_match` 必须被某 publisher 输出 且 被某 subscriber 消费的弱校验（topic 拓扑主断言仍在 `topic_contract_check.py`）|
| `plugin_symbol_check.py` | 新增 `lanelet_map_load` / `lanelet_match_pose` 符号；M3 起 `lanelet_match_t` 字段数从 5 → 11 |
| `zombie_ban_check.py` | 无改动 |
| `msg_layout_check.py` | **不动**（lane_match 是 cJSON，不走 msg_codegen；v0.1 误改） |
| `test_evaluator_gate.py`（已有）| 新增负向用例 [26]（`max_lane_offset_m` 违规时必须 FAIL，防假 PASS）+ [27]（匹配失效注入后必须触发 A11 fallback，**不**假绿）|

### 10.3 与 ROADMAP_L3.md 的勾稽

| 路线图原文 | 本规格对应 |
|---|---|
| "把 OSM 自动转成带拓扑 / 车道 / 交规 / 信号灯的地图（lanelet2 或 OpenDrive 风格）" | FR-OSM-01 / FR-OSM-02 / D2-08 |
| "增加地图匹配（map matching）：把自车姿态对齐到车道级" | FR-RT-03 / D2-05 |
| "作为定位与规划的输入" | FR-RT-04 / FR-RT-05 / D2-06 / D2-07 |

---

## 11. 参考

- [Lanelet2](https://github.com/fzi-forschungszentrum-informatik/Lanelet2) —— HD-Map 表达 + matching 算法
- [OSM2Lanelet](https://github.com/osm2lanelet/osm2lanelet) —— 转换参考
- `docs/ROADMAP_L3.md` 方向二 —— 上层路线图
- `docs/UPGRADE_DIRECTION.md` —— 近期回归 + planning `target_lane_offset` 修订背景
- `docs/MAP_ENGINE_ROUTING.md` —— esmini / OpenDRIVE 既有边界
- `docs/HANDOFF_2026-09-22.md` §15 —— lane_match 暴露背景
- `docs/HANDOFF_2026-09-23.md` —— sensor 链路 + 评估器门禁现状

---

*文档版本：**v1.0**（2026-09-26 经 agy 四轮评审 **10/10 通过 / 零 must-fix** 定稿；与同日 main 落地的 `ab05f6e` 车道可观测性叠加层对齐，构成本规格 D2-06 的前置）*
*下一步：用户拍板 §8.2 五个开放问题（Q1~Q5 推荐全选推荐项）后，进入 M1 实施*
