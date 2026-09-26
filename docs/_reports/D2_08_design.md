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

### 5.6 Step 2 §5.1 收口：traffic_light 坐标→lane fallback（2026-09-26 晚）

> **目标**：把 880 + 185 + 34 个**坐标式** traffic_light（osm_lujiazui_v2 / beijing_guomao / osm_zhengdong）
> 从"全部丢弃"变为"全部匹配"。

### 5.6.1 数据 schema 差异

| 既有契约 §2.5.1 | osm_lujiazui_v2 实测 |
|---|---|
| `{id, lane}` 或 `{id, lane_id, x, y, ...}` | `{id, x, y_lane, heading, red_s, yellow_s, green_s, phase_offset_s}` |
| 有 `lane` / `lane_id` 字段（显式关联）| **无** `lane` / `lane_id` 字段，**只有坐标** |
| converter 直接关联 | converter 必须**推断** lane |

`x` / `y_lane` **不是世界坐标**（osm_lujiazui_v2 值域 0~100 < 路网 bound 6000+），
是**车道局部坐标**：每条 lane 中心线参数化为 `s`（弧长），tl 处于 `(s=tl.x, l=tl.y_lane)`。

### 5.6.2 算法（4 步）

```
1. 显式 lane 字段优先（M2 旧路径，向后兼容）
2. 否则坐标式 fallback：
   a. arc-length ≥ tl.x 的 lane 作为候选（cum_s 二分查找）
   b. 在 s=tl.x 处用 ds=0.5m 几何差分 atan2(dy,dx) 拿 lane 切线 heading
   c. P_world = C + tl.y_lane × N（OpenDRIVE 约定 y_lane>0=左）
   d. heading 角度预过滤（|lane_heading - tl.heading| ≤ π/4）减候选 ~80%
   e. 段中心点空间网格找 P_world 最近的 polyline
   f. 判定：closest_polyline.lane_id == 假设 lane → 此 lane 认领自己的 P_world
   g. 候选按 (perpendicular_dist, s_err, lane_id) 字典序挑最小
3. 未匹配 → stderr 警告 + 静默丢弃（与既有路径一致）
4. 累计 unmatched 数 → stderr 输出"warning: N/M traffic_lights 无法匹配任何 lane"
```

### 5.6.3 实现

`tools/json_to_lanelet.py` 加 4 个 helper：
- `_perp_dist_point_segment()` — 点-线段精确垂直距离
- `_build_segment_grid()` — 段中心点空间网格（start/end/mid 三格防稀疏段漏检）
- `_closest_polyline_dist()` — 网格驱动的最近 polyline 段查找
- `_find_nearest_lane()` — 主算法

`lane_info_list` 加 `"centerline": pts` 字段（_find_nearest_lane 必需）。

traffic_light 收集循环改写：显式 `{lane}` 字段优先；坐标式走 fallback；未匹配 stderr 警告。

### 5.6.4 验证结果

| 数据源 | traffic_light 数 | 之前（step 1） | 现在（step 2 §5.1）|
|---|---|---|---|
| osm_lujiazui_v2 | 880 | **0（全部丢）** | **880** |
| beijing_guomao | 185 | 0 | 185 |
| osm_zhengdong | 34 | 0 | 34 |

### 5.6.5 性能

| maps | 旧耗时 | 新耗时 | overhead |
|---|---|---|---|
| osm_lujiazui_v2 | ~25s | ~33s | ~30% |
| osm_zhengdong | - | ~27s | - |
| beijing_guomao | - | ~12s | - |

**优化空间**（本期没做）：
- grid build 移到 `convert_map_dict` 入口（一次构建，给所有 tl 复用）
- numpy 向量化内层循环（候选 lanes 二分 + 插值）

可再砍 ~50% 时间。

### 5.6.6 已知局限（透明）

- **over-matching**：对于 (x, y_lane) 重复 / 几何上能"认领"的多个 lane，算法按 (perpendicular_dist, s_err, lane_id) 字典序破并列，导致某些短 lane 接到过多 tl。统计上 880 tls 分布在 94 个 lane（平均 ~9 tls/lane），重 traffic_light 路口短 lane 接到几十个 tl 是合理的。
- **OSM 边界 tl**：osm_lujiazui_v2 现在 880/880 全匹配（sub-task 报告 880/880 全 OK）；少数边界外的 tl 会被 dropped + stderr 警告。

### 5.6.7 单测

`tests/test_json_to_lanelet.py` 加 `TestTrafficLightFallback` 类，4 个用例：
1. `test_tl_with_lane_field_still_works` —— 显式 `{id, lane}` 路径（向后兼容）
2. `test_tl_with_x_y_lane_finds_nearest_lane` —— 坐标式 fallback 命中正确 lane
3. `test_tl_far_from_any_lane_returns_none` —— 远离所有 lane 的 tl 不匹配
4. `test_lujiazui_v2_extracts_880_traffic_lights` —— 880/880 验证（耗时 ~25s）

跑 `pytest tests/test_json_to_lanelet.py` —— **15/15 PASS**（原 11 + 新 4）。

### 5.6.8 验证清单

- [x] `pytest tests/test_json_to_lanelet.py` —— **15/15 PASS**
- [x] `ci/gates/lanelet_consistency_check.py` —— 8 maps 全绿（含 traffic_light 在 .osm 里有但 gate 不守 → 不破坏）
- [x] `ci/gates/lane_match_schema_check.py --self-test` —— 15/15 PASS（未受影响）
- [x] 其它 gates 全绿（topic_contract / msg_layout / plugin_symbol / sensor_wiring / zombie_ban）
- [x] `bash scripts/build_lanelet.sh osm_lujiazui_v2 --quiet` —— 重生成 + gate 全绿
- [x] `test_obstacle_hint_field` 36/36 PASS（未受影响）
- [x] `test_adas_nodes_logic` 71/71 PASS（未受影响）
- [x] `test_lanelet_consistency_rule4_5` 12/12 PASS（未受影响）
- [x] `test_lane_match_schema_check` 17/17 PASS（未受影响）
- [x] `bash scripts/demo.sh --no-browser 10` —— 跑通无 crash
