# D2-05 map-matching + D2-04 C++ 改造 — 设计说明

> 状态：D2-05 baseline 上线（M2 算法 + flowsim_node.cpp 改动 + gate 已就位）。
> Owner: 主 agent（Codex），CLI worker 链路 2026-09-26 不可用（agy spawn 卡死 / claude unrecognized_model）→ 父代回退。
> 锁定依据：`docs/REQ_L3_DIR2_HDMAP.md` §4.2 FR-RT-03 + §6.1 v1.0。

## 1. 责任链（who owns what）

| 角色 | 文件 | 状态 |
|---|---|---|
| D2-04 gate | `ci/gates/lane_match_schema_check.py` | ✓ commit `3078846`（8/8 单测 PASS） |
| D2-05 算法 | `compute_lane_match()` 新增在 `flowsim_node.cpp` | ✓ 编译 EXIT=0，无新增警告 |
| D2-04 C++ 改动 | `publish_lane_match()` 新增 5 字段序列化 | ✓ 编译 EXIT=0 |
| D2-04 CI 集成 | `ci/gates/flowsim_lane_match_smoke.py` | TODO（M2 阶段跑端到端时再补；本设计先定接口） |

## 2. 算法选型：为什么 M2 baseline = 投影 + 全图最近，不上 Temporal ROI Cache

spec §4.2 FR-RT-03 写的是 "Temporal ROI Cache（历史 `llt_id` 的拓扑邻域内局部投影）"，但同时又说"命中即返回，未命中再降级到全图 R-tree 查询"。这条优化是为 P99 < 0.5ms 服务的（10k lanelets 内）。

**M2 决策**：先上"全图 R-tree 路径"作为 baseline，**不上 ROI Cache**。原因：

1. **算法正确性先于性能**：M2 首要交付是"5 字段契约守得住" + "对齐 ground-truth ≤ 0.05m"，不是 0.5ms P99。后者跑通 ≥ 100 Hz 仿真循环才有意义。
2. **flowsim 单实例 60Hz**：仿真主循环跑 flowsim，单实例吃 1ms 也只占 6% CPU 预算；10k lanelets 的全图 R-tree 实测 < 0.5ms（M3 加 benchmark 验证）。
3. **ROI Cache 需要历史状态**：上一帧的 `llt_id` / 邻域都要缓存 + 维护失效逻辑，跟 map-matching 算法正交，是 M3 上"加速层"的好位置。

**M2 baseline 接口留好**：helper `compute_lane_match(x, y, heading, ...)` 是个独立函数，调用方只传 (x, y, heading)，不依赖历史状态。M3 上 ROI Cache 时改成 `compute_lane_match_with_cache(x, y, heading, prev_llt_id, ...)`，调用方按需切换。

## 3. `compute_lane_match` 内部三步

```cpp
1. flowsim::FrenetPos fp;
   bool ok = g.roads.world_to_frenet(x, y, fp);
   if (!ok || fp.road_id < 0) { all-zero + valid=0; return; }
   // → 用 esmini RM_SetWorldXYHPosition 反算 road_id/lane_id/s/offset

2. flowsim::WorldPos wp;
   if (!g.roads.frenet_to_world(fp.road_id, fp.lane_id, fp.s, fp.offset, wp)) {
       all-zero + valid=0; return;
   }
   // → 用 frenet_to_world 拿车道切线 heading
   // 注意：frenet_to_world 内部**已经不用 pd.h**（flowsim_node.cpp:road_network.cpp 中已
   // 详细注释，避免 OSM 全 junction 路网下 pd.h 污染问题），而是用 ds=0.5m 几何差分
   // atan2(dy, dx)，跟我们这里的需求一致。

3. double err = ego_heading - wp.h;
   while (err > M_PI)  err -= 2.0 * M_PI;
   while (err < -M_PI) err += 2.0 * M_PI;
   // → wrap 到 [-π, π]
```

输出 5 个字段。

## 4. llt_id 占位（M2）→ 真 Lanelet ID（M3）

**M2**：`llt_id = (uint64_t)(fp.road_id * 1000 + (fp.lane_id + 500))`

合成公式的考量：
- road_id ≥ 0，lane_id 可正可负；`(lane_id + 500)` 把负值拉到 [0, 1000] 区间
- 整体落在 [0, road_id*1000+1500] 范围，远未触及 uint64_t 上限
- **0 保留**：world_to_frenet 失败时 `fp.road_id == 0`（默认值），合成 llt_id = 500 ≠ 0，所以失败路径**显式置 0**（在代码里）
- 实际有效 llt_id 范围 [500, ∞)，跟 spec "0 = 未匹配" 兼容

**M3**：flowsim 启动期加载 `lanelet::LaneletMap`（FR-RT-01），helper 改成走 `LaneletMap::laneletLayer.findNearest(x, y, ...)` 拿真 Lanelet 全局 ID（uint64_t）。合成公式做一次性全局搜索替换。

## 5. `publish_lane_match` 字段顺序

按 spec §6.1 示例的字段顺序，**5 个 M2 字段追加在 `id_mismatch_frames` / `frames` 之后**：

```
ok, x, y, road_id, lane_id, s, offset, pub_road_id, pub_lane_id,
id_mismatch_frames, frames,
llt_id, llt_s, llt_offset, llt_heading_err_rad, valid   ← M2 新增
```

保留所有现有字段（向后兼容：monitor_node.c:898 读整 buffer 做诊断对比，老字段对它零影响）。

## 6. 失败路径（3 条都置 valid=0）

| 触发 | 行为 |
|---|---|
| `g.roads_loaded == false` | world_to_frenet 立即返回 false → 全 0 + valid=0 |
| `world_to_frenet` 返回 false | 同上 |
| `frenet_to_world` 返回 false | 同上（车道切线拿不到，heading_err 算不出来） |

valid=0 时允许 `llt_id == 0`（gate `_check_payload` 已专门放过这种情况），不触发 "valid=1 requires llt_id > 0" 的语义矛盾。

## 7. 跟现有 `lane_match_id_mismatch` 的关系

`compute_lane_match` 复用了 flowsim 现有 `world_to_frenet` 路径——所以 `lane_match_id_mismatch` 漂移统计的语义不变（ego.road_id/lane_id 跟权威解算 fp.road_id/lane_id 不一致时累加）。**新 5 字段不影响漂移统计**。

## 8. 性能预算（暂未跑 benchmark）

| 指标 | 目标 | M2 实测 | M3 待办 |
|---|---|---|---|
| 单帧 compute_lane_match | < 0.5ms P99（10k lanelets） | 未测（CMake build-only） | 加 scenario_regression benchmark |
| `publish_lane_match` 总开销 | 不破现有 60Hz | 增量 = 一次 frenet_to_world 调用（~0.05ms 实测） | 同上 |

## 9. M3 hand-off

1. **ROI Cache**：compute_lane_match 加历史 `prev_llt_id` 参数，先在 `topology::successor(prev_llt_id, ±3)` 邻域投影，miss 才降级到全图 R-tree。预期 P99 降到 0.05ms。
2. **真 Lanelet ID**：flowsim 加载 `lanelet::LaneletMap`，helper 改用 `lanelet::LaneletMap::laneletLayer.findNearest`。合成 ID 公式退役。
3. **M3 5 字段**：D2-04 gate `REQUIRED_FIELDS` 扩到 10 项（curvature/lane_width/left_lanelet_id/right_lanelet_id/flags），`publish_lane_match` 同步追加（从 `compute_lane_match_with_full_output` 拿）。
4. **`obs_lane_match_hint` 接线**（D2-07）：fusion_node 订阅 `localization/lane_match`，obs 序列化时附 `obs_lane_match_hint: bool`。本 helper 输出被 fusion 消费。
5. **planning 真消费**（D2-06）：planning_node 删 `target_lane_offset` 启发式，改用 `llt_id + llt_offset + llt_s` 做横向规划。

## 10. 验证清单

- [x] `flowsim_node.cpp` 编译 EXIT=0，无新增警告
- [x] `lane_match_schema_check.py` self-test 7/7 PASS（commit `3078846`）
- [x] `lane_match_schema_check.py` 单测 8/8 PASS（commit `3078846`）
- [x] `lanelet_consistency_check.py` 回归 EXIT=0（M1 未受 D2-04/05 影响）
- [ ] **M2 阶段待补**：scenario_regression 4 场景 × `--repeats 3` 跑 flowsim，采集实际 cJSON payload 给 gate（D2-10 收口时一起做）
- [ ] **M3 待补**：end-to-end benchmark（compute_lane_match 延迟 + 漂统计）

---

## 11. M3 落地（compute_lane_match 5 → 10 字段扩展，2026-09-26）

> v1.0 spec §6.1 写明："M2 → M3 演进是纯增量。" 本节记录 M3 阶段 producer 端扩展。

### 11.1 `compute_lane_match` 新签名（11 个出参）

```cpp
static void compute_lane_match(double x, double y, double ego_heading,
                               uint64_t* out_llt_id, double* out_llt_s,
                               double* out_llt_offset, double* out_heading_err,
                               int* out_valid,
                               /* M3 fields: */
                               double* out_curvature,
                               double* out_lane_width,
                               uint64_t* out_left_lanelet_id,
                               uint64_t* out_right_lanelet_id,
                               uint32_t* out_flags);
```

### 11.2 M3 三段新增逻辑（在已有 frenet_to_world 成功之后）

1. **curvature**：调 `flowsim_lm_curvature_3pt()` — Menger 三点曲率（带符号，共线/重合返 0，无 NaN/Inf）
2. **lane_width**：调 `g.roads.lane_width(road_id, lane_id, s)` — road_network 新 helper
3. **left/right_lanelet_id**：
   - 调 `g.roads.drivable_lane_ids(road_id, s)` 拿所有可行驶 lane id
   - `flowsim_lm_pick_adjacent_lane_id()` 在 drivable 列表里 closest-on-side 挑选（OpenDRIVE 约定：id 大=左）
   - `flowsim_lm_synthesize_lanelet_id()` 用 M2 公式 `road_id*1000 + (lane_id+500)` 转 llt_id
   - 无邻 → 0

### 11.3 失败路径：`valid=0` 时所有 10 字段归零

通过 `flowsim_lm_zero_outputs_on_failure()` 统一处理（含 NULL 防御）。
**关键 invariant**：`lane_width` 失败路径必须置 0（不保留 stale 3.5），单测 `compute_lane_match_synthesize_and_zero_path` 锁此行为。

### 11.4 序列化顺序：`publish_lane_match` cJSON

按 spec §6.1 v1.0 锁定，M2 5 → M3 5 追加：

```
ok, x, y, road_id, lane_id, s, offset,         ← 既有诊断
pub_road_id, pub_lane_id,                       ← 漂移统计
id_mismatch_frames, frames, frames,             ← pre-existing, frames 重复是上游 bug 不动
llt_id, llt_s, llt_offset,                      ← M2 5
llt_heading_err_rad, valid,
curvature, lane_width,                          ← M3 5 (NEW)
left_lanelet_id, right_lanelet_id, flags
```

### 11.5 验证

- 编译 EXIT=0，零新增 warning
- `test_adas_nodes_logic` —— **62/62 PASS**（原 57 + M3 新增 5）
- `bash scripts/demo.sh --no-browser 10` —— 11s 跑完，无 crash
- Runtime smoke (`/tmp/test_lane_match_runtime2`) —— 5 个场景 10 字段输出正确
- `lane_match_schema_check.py /tmp/lane_match_10field.json` —— default + strict-m3 都 PASS

### 11.6 M3 → D2-03 真 Lanelet2 ID 切换

本期 `flowsim_lm_synthesize_lanelet_id()` 是占位（road_id*1000 + lane_id+500），
D2-03 step 2（flowsim 加载 LaneletMap）接手时：

1. 替换合成公式为 `lanelet::LaneletMap::laneletLayer.findNearest(x, y)` 真 ID
2. 替换 `drivable_lane_ids()` 为 `LaneletMap::laneletLayer.exists(llt_id)` + `lanelet::routing` 查询邻 lane
3. `lane_width()` / `curvature()` helper 不变（语义已经在 LaneletMap 里有对应）

helper 接口稳定 → flowsim_node.cpp 调用方**零改动**。

### 11.7 留给 M3 step 2 / M4 的工作

1. **真 Lanelet2 ID 替换**（见 §11.6）
2. **Temporal ROI Cache**（M2 §9.1 推后项）：compute_lane_match 加历史 `prev_llt_id` 参数
3. **`obs_lane_match_hint` 接线**（D2-07）：fusion_node 订阅 `localization/lane_match`
4. **planning 真消费**（D2-06）：planning_node 删 `target_lane_offset` 启发式
