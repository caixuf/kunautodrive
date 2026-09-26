# D2-04 lane_match cJSON schema gate — 设计说明

> 状态：M2 进度 1/2（Python gate + 单测已落，flowsim_node.cpp 改 5 字段待 D2-05 算法先就位）。
> Owner: 主 agent（Codex），agy spawn 链路 2026-09-26 卡死，按 `cli-worker-router` guardrail 父代回退。
> 锁定依据：`docs/REQ_L3_DIR2_HDMAP.md` §6.1 v1.0。

## 1. 校验目标

锁定 `localization/lane_match` 这条 topic 的 cJSON payload 在 M2 起的形态。`topic_contract_check.py` 不够——
它只查 C struct 的 `s_inputs/s_outputs` 拓扑声明，不读 cJSON 内容。M2 起的字段集是直接在
flowsim 序列化时 `cJSON_AddNumberToObject` 追加的，必须有独立 gate 守**键名 / 类型 / 语义约束**。

## 2. Schema（M2，5 字段）

| key | 类型 | 单位 | 含义 |
|---|---|---|---|
| `llt_id` | int | — | Lanelet 全局 ID；0 = 未匹配 |
| `llt_s` | float | m | 沿车道中心线弧长 |
| `llt_offset` | float | m | 横向偏移（左正右负） |
| `llt_heading_err_rad` | float | rad | 车头 vs 车道切线夹角 |
| `valid` | int | — | 0 = 匹配失效（车不在路网内） |

**预存字段**：`id_mismatch_frames` / `frames` 仍由 flowsim 现有逻辑发出，gate 不约束。

## 3. 校验规则

| 规则 | 触发 | 错误信息 |
|---|---|---|
| 5 个 key 全部存在 | 任一缺失 | `missing required field '<key>'` |
| 类型匹配（`llt_id`/`valid` 必须 int，3 个 `llt_*` 必须 number） | 类型不符 | `field '<key>' is <type> (<value>), expected <int\|number>` |
| `bool` 假扮 int | `valid` 是 `True`/`False` | `field '<key>' is bool` |
| `valid=1` ⇒ `llt_id>0` | 矛盾状态 | `valid=1 requires llt_id > 0 (got <n>)` |
| **额外字段**：允许（M3 前向兼容） | — | — |

## 4. 为什么 `valid=false` 允许松弛

匹配失效时（车开出 road network / 在 junction 内未解算 / D2-05 算法主动放弃），所有 5 个 `llt_*` 都
是 0，没有物理意义。强制要求 `valid=0` 时 `llt_id>0` 会逼 D2-05 在降级路径里编造 lanelet id——这违反
A11 降级状态机（spec §7.1）。**门禁的语义边界到此为止**；"是否要降级"是 fusion 跟 planning 跑
`obs_lane_match_hint` 时判定的事，不归本 gate。

## 5. 跟现有 gates 的分工

| gate | 职责 | 触发时机 |
|---|---|---|
| `topic_contract_check.py` | 校验节点 C struct 的 `s_inputs`/`s_outputs` 拓扑声明 | PR / pre-merge |
| `lanelet_consistency_check.py` | 校验 `map.json` ↔ `lanelet.osm` 双向覆盖 | M1 已上线 |
| **`lane_match_schema_check.py`** | **校验 `localization/lane_match` cJSON 内容** | **M2 起新加** |

三个 gate 正交：拓扑 / 一致性 / 内容 各管一段。

## 6. CI 接入

- `--self-test` 跑 7 个内置用例（覆盖 valid/degraded/missing/zero-id/wrong-type/bool/forward-compat）
- 真实场景：`fixtures/lane_match_*.json` 由 demo / scenario_regression 跑出，自动 commit 进 fixtures 目录或临时目录传给 gate
- 返回码：`0` 全绿，`1` 任一 fixture 失败 + stderr 出 diff 友好的错误

## 7. M3 hand-off

D2-04 当前是 M2 字段集（5 个）。M3 起 spec §6.2 追加 5 个 wrapper 字段：

```json
{ "curvature": <double>, "lane_width": <double>,
  "left_lanelet_id": <uint64>, "right_lanelet_id": <uint64>, "flags": <uint32> }
```

**演进原则**：纯追加 key，不删不改老 key。本 gate 用 `_check_payload` 跑"额外字段放行"，
M3 把这 5 个 key 加进 `REQUIRED_FIELDS` 即可，**没有破坏性改动**。`forward_compat_extra_fields`
测试用例已经预演了这条路径。

**M3 待办**：把 REQUIRED_FIELDS 扩到 10 项 + 加 M3-only 单测 + 跑 scenario_regression 4 场景 ×
`--repeats 3`（D2-10 一起收尾）。

---

## 8. M3 落地（lane_match 5 → 10 字段扩展，2026-09-26）

> v1.0 spec §6.1 写明："M2 → M3 演进是纯增量：每次只追加 key，不删不改老 key，旧消费方天然兼容。"
> 本节记录 M3 阶段已实现的 schema 升级。

### 8.1 升级后的 `REQUIRED_FIELDS`

```python
REQUIRED_FIELDS = (
    # M2 — 顺序与原 M2 严格保持
    ("llt_id",              "int"),       # uint64
    ("llt_s",               "number"),    # double, m
    ("llt_offset",          "number"),    # double, m
    ("llt_heading_err_rad", "number"),    # double, rad
    ("valid",               "int"),       # 0/1
    # M3 — spec §6.2 追加
    ("curvature",           "number"),    # double, 1/m
    ("lane_width",          "number"),    # double, m
    ("left_lanelet_id",     "int"),       # uint64 strict
    ("right_lanelet_id",    "int"),       # uint64 strict
    ("flags",               "int"),       # uint32, ABI reserved (must be 0)
)
```

### 8.2 新增 `--strict-m3` 选项

- **默认关闭**：M2 5 字段必填，M3 5 字段 optional 但**存在时强制类型校验**（避免 silent 漂移）
- **开启后**：全 10 字段必填（M3 producer 必须出齐）
- 用法：`python3 ci/gates/lane_match_schema_check.py [--strict-m3] <fixture.json>`
- 启用时机：D2-10 端到端收口时（`scenario_regression --repeats 3` 全 PASS 后切强制）
- 当前 PR 阻塞检查仍走默认模式（M2 producer 不发 M3 字段也 PASS，向后兼容）

### 8.3 类型判定严格化

- `kind="int"` 严格区分 Python `int` vs `float`（`bool` 是 int 子类，先排除）
  - 拒绝 `left_lanelet_id=43.0` 这种 producer 写成 float 的 bug（cJSON 区分 int-valued vs float-valued）
- `kind="number"` 接受 `int | float`（拒绝 bool / str）
- 错误信息：`field 'X' is <type> (<value>), expected <kind>`

### 8.4 self-test 矩阵

15 个 case（原 7 + 新 8）覆盖 default + strict 双模式：

| Case | default | strict |
|---|---|---|
| valid_payload | PASS | PASS |
| valid_payload_degraded | PASS | PASS |
| missing_field (M2) | FAIL | FAIL |
| valid_one_with_zero_llt_id | FAIL | FAIL |
| llt_offset_as_string | FAIL | FAIL |
| valid_as_bool_true | FAIL | FAIL |
| forward_compat_extra_fields | PASS | FAIL (缺 M3) |
| m3_valid_payload_10_fields | PASS | PASS |
| m3_curvature_as_string | FAIL | FAIL |
| m3_curvature_as_bool | FAIL | FAIL |
| m3_left_lanelet_id_as_bool | FAIL | FAIL |
| m3_right_lanelet_id_as_float | FAIL | FAIL |
| m3_flags_as_float | FAIL | FAIL |
| m3_missing_left_lanelet_id_strict | PASS | **FAIL** |
| m4_forward_compat_unknown_field | PASS | FAIL (缺 M3) |

### 8.5 与 C++ producer 端契约对齐

flowsim_node.cpp `publish_lane_match()` 严格按 M2 5 → M3 5 顺序追加 5 字段：

```cpp
/* M2 字段 */
cJSON_AddNumberToObject(j, "llt_id", (double)llt_id);
cJSON_AddNumberToObject(j, "llt_s", llt_s);
cJSON_AddNumberToObject(j, "llt_offset", llt_offset);
cJSON_AddNumberToObject(j, "llt_heading_err_rad", llt_heading_err_rad);
cJSON_AddNumberToObject(j, "valid", (double)valid);
/* M3 字段（顺序见上） */
cJSON_AddNumberToObject(j, "curvature", curvature);
cJSON_AddNumberToObject(j, "lane_width", lane_width);
cJSON_AddNumberToObject(j, "left_lanelet_id", (double)left_lanelet_id);
cJSON_AddNumberToObject(j, "right_lanelet_id", (double)right_lanelet_id);
cJSON_AddNumberToObject(j, "flags", (double)flags);
```

**关键避坑**：uint64 / uint32 字段**必须**用 `(double)` cast（已是 int 类型），
不能传字面量 `0` 或 `0.0`（cJSON 会自动判 float 表象，触发 strict-int 校验）。

### 8.6 验证

- `python3 ci/gates/lane_match_schema_check.py --self-test` —— **15/15 PASS**
- `python3 ci/gates/lane_match_schema_check.py --self-test --strict-m3` —— **15/15 PASS**
- `python3 tests/test_lane_match_schema_check.py` —— **17/17 PASS**（TestSchemaGate 8 + TestM3Fields 9）
- 端到端 fixture `/tmp/lane_match_10field.json` —— default + strict-m3 都 PASS

### 8.7 留给 M3 step 2 / M4 的工作

1. **CI 阻塞启用 `--strict-m3`**：D2-10 scenario_regression 4 场景 × 3 repeats 全 PASS 后切强制
2. **PR 校验**：D2-10 收口后给 ci/gates/lane_match_schema_check.py 加 mcap 自动 fixture 生成
3. **真 Lanelet2 ID 切换**（D2-03 step 2）：合成公式退役，flowsim 加载 LaneletMap 后 llt_id 改用真 ID
