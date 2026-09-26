# D2-07 obs_lane_match_hint — 设计说明

> 状态：D2-07 baseline 上线（Obstacle IDL 扩展 + fusion_node 订阅 lane_match + 序列化透传）。
> Owner: 主 agent（Codex），sub-agent A (Einstein) 改 IDL/codegen，sub-agent B (Lorentz) 改 fusion_node，父 agent 联合验证 + 文档 + commit。
> 锁定依据：`docs/REQ_L3_DIR2_HDMAP.md` §4.2 FR-RT-05 + §10.1 模块职责铁律。

## 1. 责任链（who owns what）

| 角色 | 文件 | 状态 |
|---|---|---|
| IDL 扩展 | `msg/adas_msgs.msg` + `tools/msg_codegen.py` | ✅ commit（sub-agent A）|
| Codegen 重生 | `build/gen/Obstacle.h` + 22 个派生 | ✅ 自跑（sub-task A `python3 tools/msg_codegen.py ... --schema-state ...`）|
| Codegen round-trip 单测 | `tests/test_obstacle_hint_field.c` | ✅ **36/36 PASS**（sub-agent A）|
| Fusion 订阅 lane_match | `modules/adas_nodes/perception_fusion_node.cpp` | ✅ commit（sub-agent B）|
| Hint 计算 helper | `modules/adas_nodes/fusion_lane_hint.h`（NEW, header-only） | ✅ commit |
| Hint 计算 + cache + apply 单测 | `tests/test_adas_nodes_logic.c` | ✅ **71/71 PASS**（原 62 + 新 9；sub-agent B）|
| Producer buffer overflow 修复 | `perception_node.cpp` / `lidar_driver_node.c` / `stereo_vision_node.c` / `bev_detection_node.cpp` / `monitor_node.c` | ✅ 5 文件（父 agent 联合验证时）|
| msg_layout_check 已知 mismatches 更新 | `ci/gates/msg_layout_check.py` Obstacle (35,40) | ✅ commit |
| Pipeline car allow_hung_subs 加 `localization/lane_match` | `config/pipeline_car.json` | ✅ commit |

## 2. IDL 扩展

### 2.1 Obstacle struct 新 wire/struct 布局

| # | Field | offset (C struct) | size | wire offset | kind |
|---|-------|------------------|------|-------------|------|
| 1 | id                | 0  | 4B | [0..3]   | UINT  |
| 2 | x                 | 4  | 4B | [4..7]   | FLOAT |
| 3 | y                 | 8  | 4B | [8..11]  | FLOAT |
| 4 | vx                | 12 | 4B | [12..15] | FLOAT |
| 5 | vy                | 16 | 4B | [16..19] | FLOAT |
| 6 | width             | 20 | 4B | [20..23] | FLOAT |
| 7 | length            | 24 | 4B | [24..27] | FLOAT |
| 8 | lane_id           | 28 | 1B | [28]     | INT   |
| 9 | type              | 29 | 1B | [29]     | ENUM  |
| 10| confidence        | 32 | 4B | [30..33] | FLOAT |（C struct 32 是 2B padding）|
| 11| **obs_lane_match_hint** | 36 | 1B | **[34]** | BOOL  |（wire 末尾，C struct 末尾 + 3B trailing pad）|

- **wire size**：35B（原 34B）
- **sizeof(Obstacle)**：40B（原 36B，因 bool @36 后有 3B trailing pad 让 struct 对齐 4B）
- **ObstacleList wire**：16 + 128×35 = **4496B**（原 4368B）
- **ObstacleList struct**：16 + 128×40 = **5144B**（原 4632B）

### 2.2 SCHEMA_VERSION 自动 bump（D2-07 引入 sub-task A 的 codegen 改进）

`tools/msg_codegen.py` 新增 `CodeGenerator(schema_state=...)` 参数 + `--schema-state` CLI flag：

- codegen 启动时读 `${GEN_DIR}/msg_schema_state.json`（已存在的 state）
- 对每个 struct 计算当前 `layout_hash`，与 state 中记录的 hash 对比
- 不一致 → 自动 bump `version + 1` + 写回 state
- 一致 → version 不变

**好处**：以后任何 IDL 改动自动 bump version，不用手动改 codegen 默认值。

**注意**：`OBSTACLE_SCHEMA_VERSION` 现在 = **2**（自动 bumped），`OBSTACLE_SCHEMA_HASH` = **0xcef8d07f**（重算）。

**⚠️ 副作用**：`OBSTACLE_TYPE_ID` 从 `0xe8ec547a` 变成 `0x322bd084u`（自动重算，因 type_id hash 包含完整 field signature）。
旧 producer/consumer 按 `type_id + schema_hash` 双判（`src/core/serializer.c:182`），
新 type_id + 新 schema_hash 与旧 type_id 不匹配 → 旧消费者会自动报 SCHEMA_INCOMPATIBLE（这是预期行为：wire format 改了，必须升级两端）。

如果未来需要 type_id 稳定（例如在 wire format 不变时），加 IDL directive `@type_id 0x...` 是后续可加的扩展点。

## 3. Hint 计算规则（spec §4.2 FR-RT-05 v1 简化版）

```
hint = |obs.y - lm.llt_offset| < 3.0 × lm.lane_width
```

**几何解释**：
- ego 当前车道中心线 lateral 偏移 = `lm.llt_offset`（米）
- obs 相对 ego 中心的横向距离（车体坐标系）= `obs.y`（米）
- obs 相对车道中心线的横向距离 = `obs.y - llt_offset`
- 阈值 `3 × lane_width` 覆盖 ego 同车道 + 两侧各 1 lane

**Defensive checks（按顺序短路）**：

| # | 触发条件 | 行为 |
|---|---|---|
| 1 | `lm.valid == false` | 所有 obs 的 hint = false |
| 2 | `lm.fresh == false`（> lm_max_age_us）| 同上 |
| 3 | `lm.lane_width ≤ 0` 或 `!isfinite` | 同上 |
| 4 | `obs.lane_id == -1`（感知未分配车道） | 该 obs 的 hint = false |
| 5 | 都不触发 | 执行 lateral distance 公式 |

## 4. 关键不变量（spec §4.2 铁律 + CLAUDE.md 职责铁律）

1. **obs 总数不变**：`fusion_apply_hint()` **仅设** `obs.obs_lane_match_hint`，**不**过滤、**不**删除任何 obs。单测 `test_lane_hint_apply_hint_batch` 显式断言 count=3 不变。
2. **不下结论**：hint=false **不**代表"obs 在其它车道"，**只**代表 fusion 暂无法判断（cache 失效 / 陈旧 / obs 未分配车道）。
3. **planning 自决**：fusion 只打 metadata，planning 自己消费 hint + lane_match 决策（CLAUDE.md 职责铁律）。
4. **M2 向后兼容**：M2 时期没 lane_match 输入时 cache.valid = false → 所有 hint = false → 不影响既有消费方。

## 5. LaneMatchCache 线程模型

```
   transport thread             fusion main coroutine
        │                              │
        │  on_lane_match()             │
        │  lm_mutex.lock()             │
        │  write {valid,lane_width,    │
        │         llt_offset,stamp_us}  │
        │  lm_mutex.unlock()           │
        │                              │  lm_mutex.lock()
        │                              │  snap = lm_cache
        │                              │  lm_mutex.unlock()
        │                              │  check_freshness(&snap)
        │                              │  apply_hint(&tracked, &snap)
        │                              │  ↓
        │                              │  serialize + publish
```

- **pthread_mutex** 而非 atomic：cache 写入 4 个字段需原子，atomic 粒度太细易出错
- 主协程读时做本地 snapshot 再释放锁，避免 O(N) 持锁遍历 obs
- 默认 `lm_max_age_us = 1s`（60Hz fusion × 1s ÷ 100ms publish 周期 = 6 个 cycle，足够新鲜）
- 可通过 `params_json` 的 `lm_max_age_ms` 字段配置

## 6. cJSON 字段缺失处理（向后兼容）

`on_lane_match()` 仅取 3 个字段：`valid` / `lane_width` / `llt_offset`。

| 场景 | 行为 |
|---|---|
| M2 payload（5 字段，缺 M3 lane_width） | `cJSON_GetObjectItem(root, "lane_width")` 返回 NULL → lane_width = 0.0 → 防御性 cache.valid = false → 所有 hint = false |
| M3 payload（10 字段齐全）| 3 字段正常解析 → cache.valid = true → compute_hint 按 lateral 公式 |
| 非法 JSON | cJSON_Parse 返回 NULL → 静默忽略（spec §6.1 兼容异常 payload）|
| 字段类型错（如 valid 是 bool）| cJSON_IsNumber 失败 → cache.valid 维持 false |

## 7. Producer 端 buffer overflow 修复

D2-07 把 Obstacle wire 34→35B，满载 128 obs 时 ObstacleList wire = 4496B，原 `uint8_t buf[4368]` 必越界。修复：

| 文件 | 修复 |
|---|---|
| `modules/adas_nodes/perception_node.cpp:454` | `uint8_t obs_buf[4368]` → `uint8_t obs_buf[sizeof(ObstacleList)]`（5144B 一定够用） |
| `modules/adas_nodes/lidar_driver_node.c:313` | 同上 |
| `modules/adas_nodes/stereo_vision_node.c:353` | 同上 |
| `modules/adas_nodes/perception_fusion_node.cpp:574` | 同上（sub-task B 改的）|
| `modules/adas_nodes/bev_detection_node.cpp:110` | 仅注释更新（buffer 已是 `sizeof(ObstacleList)`）|
| `modules/adas_nodes/monitor_node.c:418` | 仅注释更新（sizeof(5144) / 线格式(4496)）|

**统一规范**：以后任何 producer 端 ObstacleList 序列化 buffer，**一律用 `sizeof(ObstacleList)`**，
不要再写 magic number `4368` / `4496`（会随 D2-07+ 再次变更时再次越界）。

## 8. 验证

| 项 | 结果 |
|---|---|
| 编译 | EXIT=0, **0 warnings** |
| `test_obstacle_hint_field` | **36/36 PASS**（sub-agent A）|
| `test_adas_nodes_logic` | **71/71 PASS**（原 62 + sub-agent B 新 9）|
| ctest 全套（33 个测试）| **100% passed** |
| `msg_layout_check.py` | ✓ OK (20 types, 12 known mismatches)|
| `topic_contract_check.py` | ✓ OK (6 configs) |
| `plugin_symbol_check.py` | ✓ OK (34 plugins, 893 symbols) |
| `sensor_wiring_check.py` | ✓ OK |
| `zombie_ban_check.py` | ✓ OK |
| `lane_match_schema_check.py --self-test` | ✓ 15/15 PASS |
| `lanelet_consistency_check.py` | ✓ OK (8 maps) |
| `bash scripts/demo.sh --no-browser 10` | ✓ 跑通；s_inputs 显示 `localization/lane_match` 已 subscribe |

## 9. 留给 D2-06 / D2-10 的工作

1. **D2-06 planning 真消费 hint + lane_match**：planning_node 删 `target_lane_offset` 启发式，
   改读 `lane_match.llt_id + llt_offset + llt_s` 做横向规划；`obs.obs_lane_match_hint=true`
   的 obs 进候选集，hint=false 的进默认集（planning 自决）。
2. **D2-03 真 Lanelet2 ID 替换**：合成 llt_id → `lanelet::Lanelet::id()`，本批已留接口稳定。
3. **D2-10 收口**：4 场景 × `--repeats 3` 全 PASS + `max_lane_offset_m` 门禁 + `--strict-m3` 切 CI 阻塞。
4. **type_id 稳定 directive**（如需要）：`@type_id 0xe8ec547a` 形式 IDL directive 让旧 type_id 可保留。
