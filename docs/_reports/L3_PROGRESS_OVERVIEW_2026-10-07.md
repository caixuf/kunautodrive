# L3 升级现状速览（2026-10-07 摸底）

> 目的：一次性讲清「项目现在在做的 L3 升级是什么、推进到哪、卡在哪、下一步是什么」。
> 依据仓库内文档 + 代码实测，不引入仓库外信息。权威方向文档见
> `docs/ROADMAP_L3.md`（长期）与 `docs/UPGRADE_DIRECTION.md`（近期回归）。

---

## 0. 一句话结论

**长期目标是 6 个方向的「接近真 L3」演进；当前实际主战场是方向二（OSM→HD-Map 自动化）。**
方向二已完成 M1 + M2 + M3 step 1（工具链 / 数据层 / 契约层全绿），**剩 3 件收尾**：
D2-06 planning 真消费、D2-03 真 Lanelet2 ID 切换、D2-10 端到端收口。
**方向六（L3 安全闭环：ODD / TOR / MRM / DMS / EDR）在代码里基本空白，只有路线图。**

---

## 1. 两条并行的「L3」线

| 线 | 文档 | 定位 | 状态 |
|---|---|---|---|
| 近期回归修复 | `docs/UPGRADE_DIRECTION.md` | 把 `main` 稳回已知可用基线 `a065f26`，BEV 升级隔离到特性分支 | 已落（文档记录决策） |
| 长期 L3 路线图 | `docs/ROADMAP_L3.md` | 6 大方向，把"单场景演示"演进为"可随仿真进化的近 L3 系统" | 方向二在推进，方向六未动 |

> ⚠️ 命名坑：`include/degrade_ladder.h` 的 `DEGRADE_L3` = 降级阶梯"立即停"档，
> **不是** SAE L3。路线图建议改名 `DEGRADE_STOP`，尚未做。

---

## 2. 路线图 6 方向与真实进度

| 方向 | 内容 | 代码实况 |
|---|---|---|
| 一 多模态感知 | 相机 BEV + 雷达融合、可降级感知 | `bev_detection_node` / `bev_pre` / `bev_post` 已在；`sensor_model` 有 lidar/gps/camera **占位**，**无雷达模型**；BEV 直通模式曾被判为跑偏根因（`UPGRADE_DIRECTION`） |
| 二 地图自动化 | OSM → lanelet2/HD-Map + map matching | **当前主战场**，见 §3 |
| 三 交通流生成 | NPC 泊松到达 + IDM 跟车 + MOBIL 换道 | `flowsim/npc_ai.{h,cpp}` + `traffic_density_spawn.h` 已有雏形；`traffic_density` 自动填充刚做成端到端可观测（HEAD commit `d19d0c1`） |
| 四 混合决策规划 | 规则层兜底 + 模型层 shadow → 接管 + 仲裁 | 规则层（behavior FSM + Frenet）在跑；模型层有 shadow/评估雏形（`test_evaluate_shadow_mode.py`） |
| 五 仿真闭环 | 场景随机化 + 批量 headless + 评估门禁 + 训练 | `SIM_DIGEST` / `LEARNING_LOOP` / `DATA_CLOSED_LOOP` + `tools/train_e2e/*` 雏形在；确定性回放/模糊测试待补 |
| 六 L3 安全闭环 | ODD / TOR / DMS / MRM / 冗余 / EDR / 安全论证 | **基本空白**：无 ODD/TOR/DMS 节点；仅 `src/core/degrade_ladder.c` 的降级阶梯 |

---

## 3. 方向二（HD-Map）任务矩阵 —— 当前主线

外部需求文档 `REQ_L3_DIR2_HDMAP.md` **不在本仓库**（`M1_SUBMODULE_SETUP.md` 已注明），
本仓库只能从 `docs/_reports/D2_*.md` + 代码反推。

| 任务 | 内容 | 状态 | 证据 |
|---|---|---|---|
| D2-01 | Lanelet2 submodule 接入（M1 文档化） | ✅ | `.gitmodules` 有 `third_party/lanelet2`；仅占位，未编译 |
| D2-02 | 转换器 `tools/json_to_lanelet.py`（字节级确定 OSM 输出） | ✅ | 27KB 脚本；契约 `M1_OSM_INTERFACE_CONTRACT.md` |
| D2-09 | `ci/gates/lanelet_consistency_check.py`（map.json ↔ lanelet.osm） | ✅ | Rule 1–5 全跑；8 maps 全绿；CI job `lanelet-consistency-gate` |
| D2-08 | 信号灯/停止线/限速 regulatory_element | ✅ | step1 三 subtype + step2 Rule 4/5 + traffic_light 坐标→lane fallback（880/185/34 全匹配） |
| D2-04 | `lane_match` cJSON schema gate | ✅ | `lane_match_schema_check.py` 5→10 字段；`--strict-m3` 默认关 |
| D2-05 | map-matching `compute_lane_match` | ✅ | 投影 + 全图最近（ROI Cache 推后）；输出 10 字段 |
| D2-07 | `obs_lane_match_hint`（IDL + fusion 订阅 + 透传） | ✅ | `Obstacle` wire 34→35B；`fusion_lane_hint.h`；`test_obstacle_hint_field` 36/36 |
| **D2-06** | **planning 真消费 lane_match** | ⚠️ **部分** | 已消费 `obs_lane_match_hint` 做 TTC 跟车候选（两段式 hint-first，`planning_node.cpp:1772`）；**横向规划仍是 `target_lane_offset` 启发式（`:2031`），未改读 `llt_id/llt_offset/llt_s`** ← 关键成功线 |
| **D2-03** | **真 Lanelet2 ID 替换** | ❌ 未做 | `llt_id` 仍是合成式 `road_id*1000+(lane_id+500)`；需 flowsim 启动加载 `LaneletMap`（要 Boost + pugixml） |
| **D2-10** | **端到端收口** | ❌ 未做 | 4 场景 × `--repeats 3`、`max_lane_offset_m` 门禁、`--strict-m3` 切 CI 阻塞 均未做 |

### 里程碑
- **M1**：submodule 占位 + 接口契约 + 转换器 + consistency gate → ✅
- **M2**：step1 转换器 regulatory 三 subtype → ✅；step2 Rule 4/5 实校验 + 大地图 lanelet.osm 生成 → ✅
- **M3**：step1 `lane_match` 5→10 字段（D2-04/05 M3）→ ✅；step2 flowsim 加载 Lanelet2 + topic 扩展 + 真 Lanelet2 ID → ❌

---

## 4. 质量门禁与测试现状

**8 个静态 gate**（`ci/gates/`，每个一个 CI job）：topic-contract / msg-layout / plugin-symbol /
sensor-wiring / zombie-ban / book-guard / **lanelet-consistency** / **lane-match-schema**。

**L3 相关测试**：`test_json_to_lanelet.py` 15/15、`test_lane_match_schema_check.py` 17/17、
`test_lanelet_consistency_rule4_5.py` 12/12、`test_obstacle_hint_field.c` 36/36、
`test_adas_nodes_logic` 71/71。

**`docs/_reports/review_2026-09-27.md` 审阅结论**：7 HIGH 已修（buffer overflow、死测试未注册、
断言放宽、schema_hash 双判空头支票、default profile 漏接 hint 等）；**14 MEDIUM + 15 LOW 未修**；
**review 修复后的 demo 10s 端到端未复验**。

---

## 5. 已知风险 / 技术债（路线图 P0 体检，2026-10-02）

- `lane_change_traffic` 车道保持 FAIL（99% 巡航帧压线，worst 7.0m）—— 待复现定位。
- 险情指标无门禁：`straight_road` min TTC 0.124s、`lane_change_traffic` 0.025s 均放行。
- 舒适性无门禁：jerk_max 68.6 m/s³（舒适 < 10）。
- 所有场景 `safety_evidence_present: False`。
- 构建缺 `libeigen3-dev` 时 Frenet 静默不编 → planning 退化为只车道保持（应改为硬失败）。
- FlowBoard `/api/map/preview` 404；`[RoadAxis] road 无 lanes` 警告。

---

## 6. 下一步（依赖顺序）

按 `HANDOFF_2026-09-26.md` 与 `ROADMAP_L3.md` 收敛，推荐顺序：

1. **D2-06 planning 真消费**（critical success line）：删 `target_lane_offset` 启发式，
   改读 `lane_match.llt_id + llt_offset + llt_s`；控制器重调；4 场景 × 3 repeats 全 PASS。
2. **D2-03 真 Lanelet2 ID**：装 Boost + pugixml，flowsim 启动 `lanelet::load()`，
   合成 llt_id 退役（helper 接口已稳定，调用方零改动）。
3. **D2-10 收口**：`max_lane_offset_m` 门禁 + `--strict-m3` 切 CI 阻塞 + mcap 自动 fixture。
4. **清 MEDIUM/LOW**：lane_match_schema_check 进 CI、pipeline_car 契约漂移、frames 重复行等。
5. **补 demo 端到端复验**（review 修复后未跑）。
6. 方向六 P0（若要动 L3 安全）：先修 `lane_change_traffic` + 加 TTC/jerk/safety_evidence 门禁，
   再做 ODD → TOR → MRM。

---

*本文件为一次性摸底留档，非交付补丁；分支 `cline/7f67n24k`。*
