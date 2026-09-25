# D2-08 信号灯 / 停止线 / 限速结构化 — 设计说明

> 状态：D2-08 step 1 (M2 baseline) 上线 — 转换器扩展 + 契约更新 + 单测全绿。
> Owner: 主 agent（Codex），CLI worker 链路 2026-09-26 不可用，父代回退。
> 锁定依据：`docs/REQ_L3_DIR2_HDMAP.md` §4.1 D2-02 + §4.2 D2-08 + §6.1 v1.0。

## 1. 这一步交付了什么

| 改动 | 文件 | 行数 |
|---|---|---|
| 转换器：扩展 `OSMRegulatoryRelation` dataclass | `tools/json_to_lanelet.py` | +6 字段 |
| 转换器：§5b speed_limit regulatory 生成 | 同上 | +14 行 |
| 转换器：§5c 独立 stop_line regulatory 生成 | 同上 | +24 行 |
| 转换器：§5d 合并 regulatory 顺序 (traffic_light → speed_limit → stop_line) | 同上 | +2 行 |
| 转换器：§6 输出循环支持 subtype 字段 | 同上 | +6 行 |
| 契约：§2.5 加 §2.5.1~2.5.4 | `docs/M1_OSM_INTERFACE_CONTRACT.md` | +35 行 |
| 契约：gate Rule 4 / 5 注释 | 同上 | +2 行 |
| Gate：Rule 4 / 5 注释（**逻辑实现推后**） | `ci/gates/lanelet_consistency_check.py` | +2 行 |
| 测试：test_01 / test_04 适配新计数 | `tests/test_json_to_lanelet.py` | 已修 |
| 测试：test_09 / test_10 / test_11 新增 | 同上 | +60 行 |

## 2. regulatory_element 三种 subtype（D2-08 v1.0-clarify-3 契约）

| subtype | data source | members | 关键 tag |
|---|---|---|---|
| `traffic_light` | `map.json.landmarks.traffic_lights[].lane` | `stop_line` + `ref_line` | `subtype=traffic_light` |
| `speed_limit` | `map.json.roads[].speed_limit`（每个 lane 一个） | 仅 `ref_line`（**无** `stop_line`） | `subtype=speed_limit` + `speed_limit=<m/s>` |
| `stop_line` | `map.json.landmarks.stop_lines[].lane` | `stop_line` + `ref_line` | `subtype=stop_line` |

输出顺序严格按 `traffic_light → speed_limit → stop_line`（契约 §2.5.4），同 subtype 内按 lane id 字典序。

## 3. 字节级确定性

D2-02 字节级稳定性原则在 D2-08 之后仍然成立：

- `speed_limit` 数值精度：2 位小数（与 lanelet relation 一致）
- regulatory id 从 `RT0` 起，跟现有 traffic_light 计数**累加**（不是重新从 0 开始）
- tag 顺序固定：`type` → `subtype` → （speed_limit 仅 `speed_limit`）→ `lanelet_id`
- member 输出：`stop_line`（可选）→ `ref_line`（必出）

`test_11_byte_stability_with_regulatory_D2_08` 验证两次运行 bytes 相等。

## 4. 与现有 traffic_light 的关系

traffic_light 的 regulatory_element 在 M1 已经实现（`OSMRegulatoryRelation` 字段 `stop_line_way_id` + `ref_line_way_id` + `lanelet_id`），D2-08 没改它。改的：

- dataclass 加 `subtype: str` 字段（必填）
- dataclass 加 `speed_limit: str | None = None` 字段
- dataclass 调 `subtype="traffic_light"`（保持兼容）
- 第 §6 输出循环按 subtype 决定是否输出 `stop_line` member + `speed_limit` tag

## 5. 已知 gap（M3 待办，不在 D2-08 M2 范围）

### 5.1 traffic_light 的 schema 不一致

| 数据源 | `traffic_light` 字段格式 | 转换器能识别 |
|---|---|---|
| osm_test / test fixture | `{id, lane}`（lane 关联） | ✓ |
| osm_lujiazui_v2 (880 items) | `{id, x, y_lane, heading, red_s, yellow_s, green_s}`（坐标，无 lane 字段） | ✗ 被 `valid_tls` 过滤 |

osm_lujiazui_v2 的 880 个 traffic_light 全部被丢弃，因为转换器要求每条 traffic_light 必须有 `lane` / `lane_id` 字段。**M3 待办**：转换器加 `_find_nearest_lane(x, y)` fallback，用 lane centerline 投影找最近 lane。

### 5.2 gate Rule 4 / 5 逻辑实现

D2-09 gate 现有实现只解析 `type=lanelet` 的 relation（契约 §7.3 明文），不解析 regulatory_element。D2-08 注释里加了 Rule 4 / 5 但**实际校验代码未实现**。

**M3 待办**：gate 解析 `type=regulatory_element` 的 relation（按 subtype 分桶），实现：
- Rule 4：每个 map.json 有 speed_limit 的 lane ↔ .osm 里 subtype=speed_limit regulatory 双向覆盖
- Rule 5：每个 landmarks.stop_lines[] item ↔ .osm 里 subtype=stop_line regulatory 双向覆盖

注：M2 step 2（flowsim 加载 Lanelet2）触发后，gate 才有动力实现 Rule 4/5 —— 现在 Rule 1/2/3 已能守 lanelet 一致性，regulatory 完整性靠转换器单测守。

### 5.3 flowsim 加载 Lanelet2 + topic 内容扩展

spec §4.2 FR-RT-01 要求 flowsim 启动期 `lanelet::load(...)`，加载完 regulatory_element 后发布到 `road/traffic_lights` / `road/traffic_signs` 已有 topic。

**M2 step 2**（推后）：
1. `git submodule update --init --recursive third_party/lanelet2`（要装 Eigen + Boost + Glibmm）
2. `flowsim_node.cpp` 加 `lanelet::LaneletMap` 加载 + regulatory 查询
3. `road/traffic_lights` topic 字段扩展（加 subtype / ref_line_id / signal_group 等）
4. `road/traffic_signs` topic **新增**（目前没有该 topic）

## 6. 验证清单

- [x] 转换器单测 11/11 PASS（test_01~test_11 含 D2-08 step 1 新增 3 个）
- [x] D2-04 gate self-test 7/7 PASS（commit `3078846` 未受影响）
- [x] D2-04 单测 8/8 PASS（同上）
- [x] D2-09 gate 回归 EXIT=0（M1 + M2 step 1 未破坏）
- [x] 转换器字节级稳定性（含新 regulatory）test_11 验证
- [x] 契约版本号更新到 v1.0-clarify-3
- [ ] M3：gate Rule 4/5 实际校验代码
- [ ] M3：转换器坐标 → lane 自动映射
- [ ] M3：flowsim 加载 Lanelet2 + topic 扩展

---

## 5.5 Step 2 收尾：Rule 4 / Rule 5 实际校验落地 + 大地图 lanelet.osm 生成（2026-09-26）

> **目标**：把 M2 step 2 推后项 §5.3 拆出的"一致性 gate 全绿"前置补齐：
> ① 8 个 maps 全量生成 `lanelet.osm`；② gate 实现 D2-08 契约 §2.5.2/§2.5.3 的
> Rule 4 / Rule 5 实际校验逻辑（注释占位 → 真代码）。

### 5.5.1 这一步交付了什么

| 改动 | 文件 | 行数 |
|---|---|---|
| Gate: `parse_regulatory_elements()` 新增 | `ci/gates/lanelet_consistency_check.py` | +59 行 |
| Gate: `_map_speed_limits()` + `_map_stop_lines()` 新增 | 同上 | +47 行 |
| Gate: `check_consistency()` 加 Rule 4/5 块 | 同上 | +49 行 |
| Gate: docstring 更新 + `SPEED_LIMIT_TOLERANCE_MPS` 常量 | 同上 | +14 行 |
| 单测: Rule 4 Forward/Backward + Rule 5 Forward/Backward + helpers | `tests/test_lanelet_consistency_rule4_5.py` | +412 行（12 个用例）|
| 7 个 maps 生成 `lanelet.osm`（commit 进 repo）| `maps/{osm_test,city_center,city_ring,osm_munich,city_grid,beijing_guomao,osm_lujiazui_v2}/lanelet.osm` | 共 ~16 MB |
| 大地图 gitignore + 重建脚本 | `.gitignore` / `tools/build_lanelet.sh` | +8 行 / +108 行 |
| CI 集成：`lanelet-consistency-gate` job | `.github/workflows/ci.yml` | +12 行 |

### 5.5.2 数据结构：4 桶独立

| 桶 | map.json 来源 | .osm 来源 | 校验 |
|---|---|---|---|
| `speed_limit` | `roads[].speed_limit`（lane 级覆盖优先级）| `<relation type=regulatory_element subtype=speed_limit>` | Rule 4 Forward + Backward + 数值 ±0.01 m/s |
| `stop_line` | 顶层 `landmarks.stop_lines[].lane` 聚合到 count | `<relation type=regulatory_element subtype=stop_line>` 按 lane 聚合 count | Rule 5 Forward + Backward + count 一致 |
| `traffic_light`（D2-08 step 1 已落地）| 顶层 `traffic_lights[].lane` + `landmarks.traffic_lights[].lane` | 同 subtype=traffic_light | D2-09 不守（契约 §7.3）；由 D2-02 转换器自测守 |
| `lanelet` | `roads[].lanes[].id` + centerline[0] | `<relation type=lanelet>` + left/right way 几何中点 | Rule 1/2/3（既有） |

### 5.5.3 Rule 4 容差选择：`SPEED_LIMIT_TOLERANCE_MPS = 0.01`

- 契约 §2.5.2："单位 m/s，**2 位小数**"
- 2 位小数的浮点表示边界 = 0.005 → 取 0.01（多 0.005 epsilon 防御浮点 `0.005+0.005 != 0.01`）
- 单测 `test_value_within_tolerance` 验证 10.005 vs 10.00 PASS（偏差 0.005 < 0.01）
- 单测 `test_value_mismatch` 验证 12.00 vs 10.00 FAIL（偏差 2.0 > 0.01）

### 5.5.4 大地图 lanelet.osm 处理：gitignore + 重建脚本

- `osm_zhengdong/lanelet.osm` 63MB → `.gitignore` 排除；不污染仓库
- `tools/build_lanelet.sh` 提供三条路径：
  - `bash tools/build_lanelet.sh` → 默认生成所有缺 lanelet.osm 的 maps
  - `bash tools/build_lanelet.sh osm_lujiazui_v2` → 生成指定地图（spec §3 user story）
  - `bash tools/build_lanelet.sh --check` → 跳过生成，只跑 gate（CI 友好）
  - `bash tools/build_lanelet.sh --all` → 强制重生成全部（map.json 改了之后用）
- CI `lanelet-consistency-gate` job 第一步 `bash tools/build_lanelet.sh --quiet`，第二步跑 gate

### 5.5.5 Rule 5 当前 vacuously 满足

8 个 maps 没有任何 `landmarks.stop_lines[]` 数据 → Rule 5 Forward + Backward 都是空集比对
（0 == 0）→ 不报错。但**单测已覆盖**（`test_count_match` / `test_count_mismatch` /
`test_extra_stop_line_no_decl`），将来给真实场景注入 stop_line 数据时 gate 立即能拦。

### 5.5.6 验证清单

- [x] `tests/test_lanelet_consistency_rule4_5.py` **12/12 PASS**
- [x] `tests/test_json_to_lanelet.py` **11/11 PASS**（未受影响）
- [x] `ci/gates/lanelet_consistency_check.py` 8 个 maps 全绿
- [x] `ci/gates/lane_match_schema_check.py --self-test` 7/7 PASS（未受影响）
- [x] `test_adas_nodes_logic` 57/57 PASS（未受影响）
- [x] 其它 gates 全绿（topic_contract / msg_layout / plugin_symbol / sensor_wiring / zombie_ban）
- [x] `tools/build_lanelet.sh --help` 输出正确（bash 自检）
- [x] `tools/build_lanelet.sh osm_zhengdong` 现生成 63MB .osm，gate 仍全绿

### 5.5.7 留给 M3 的工作

1. **`landmarks.traffic_lights` 坐标 → lane 自动映射**（§5.1）：当前 osm_lujiazui_v2 的 880 个
   traffic_light 因无 `lane` 字段被转换器丢弃；M3 step 2 需给转换器加 `_find_nearest_lane` fallback
2. **`road/traffic_lights` topic 字段扩展 + `road/traffic_signs` topic 新增**（§5.3）：
   flowsim 加载 .osm 后发布 regulatory_element 内容；本期不动
3. **D2-09 真 Lanelet ID 切换**：flowsim 加载 LaneletMap 后，M2 阶段合成的
   `road_id*1000 + lane_id+500` 让位给真 `lanelet::Id`（uint64_t）
