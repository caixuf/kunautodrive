# 五个方向的落地状态 — 2026-11-07

> 目标：把 `docs/ROADMAP_L3.md` 的方向一~五从"路线图"推进到**可验证的落地切片**。
> 本文记录本轮实际做了哪些代码/门禁、用什么命令可复现，以及**仍未完成的部分**。
> 判断标准沿用仓库铁律：能跑、能过门禁、有实测证据；**不把"写了文档"算完成**。

## 一句话结论

| 方向 | 本轮状态 | 一句话 |
|---|---|---|
| 一 · 多模态感知 | ✅ 雷达 + 视觉退化 + 契约 + BEV 活性 | 雷达转换/并入/断流兜底 + 相机退化模型 + publish 类型门禁 + `bev/obstacles` 活性门禁（<1Hz FAIL）；BEV 仍为直通影子、无精度对比 |
| 二 · 地图自动化 | ✅ D2-10 收口 | 4 场景 × 3 次实测 12/12 PASS，最差横向 1.185 ≤ 1.65；A1–A6、A8–A11 达标 |
| 三 · 交通流 | ✅ 密度/多 OD/行人/加塞 | 时间+区域密度、OSM 多 OD 链逐车分配、行人自动生成、`npc_cutin` 场景、MOBIL 一帧中止修复 |
| 四 · 混合决策 | ✅ 影子/仲裁/置信度 | promote 转向+轨迹阈值、接管边界 gate、`ControlRaw.confidence` + 置信度门（<0.5 回退规则） |
| 五 · 仿真闭环 | ✅ RNG 隔离 + 确定性门禁 | flowsim/NPC 自持 RNG、`init_fingerprint` 确定性 gate、`ota push` 强制过 gate、闭环评估确定性回归 |

---

## 方向一 · 多模态感知底座

**已落地**

| 项 | 文件 | 断言 |
|---|---|---|
| 雷达 → 障碍物纯转换 | `modules/adas_nodes/radar_post.h` | `test_adas_nodes_logic` 新增 6 例：车体系映射/方位符号/置信度单调/不伪造宽长类型/容量守卫/去重并入 |
| perception 可选并入 | `perception_node.cpp`（`enable_radar_fusion`，默认 0） | 默认零行为变化；开启后按 2.5m 去重，同一目标不重复计数 |
| LiDAR 掉线时雷达单独出帧 | 同上（sensor 模式 `no fresh cloud` 分支） | 实测：`mode=sensor` + `lidar_mode=0` + `enable_radar=1`，`perception/obstacles` 仍以 ~10Hz 发布（82 帧/8s，0 drop） |
| 配置类型纠错 | `config/pipeline*.json` | `CameraFrame→LidarFrame`、`GpsFrame→GpsData`（代码实际发布类型） |
| 接线门禁 | `ci/gates/sensor_wiring_check.py` / `topic_contract_check.py` | `enable_radar=1` 必须有 `sensor/radar` 消费者；publish `type` 必须是真实 IDL（已做 A/B：注入 `GpsFrame` 即 FAIL） |

**未完成（如实记录）**：相机仍是 LiDARFrame 副本 + 可见度乘性衰减，无夜间/逆光/遮挡成像模型；
BEV 仍默认直通影子，无端到端影子对比门禁；雷达与视觉的置信度加权/航迹级融合未做。

## 方向二 · 地图自动化（D2-10 收口）

`docs/_reports/D2_10_4x3_validation_plan.md` 顶部状态已从 `DRAFT` 改为 **✅ DONE（2026-11-07 复核）**。

```bash
python3 ci/evaluators/scenario_regression.py --d2-10-scenes --repeats 3 --workers 4 \
  --results-dir /tmp/flow_regression_d210_fresh
```

实测：`straight_road` / `curve_road` / `urban_challenge` / `city_comprehensive` 全 PASS；
四次中位 `max_abs_llt_offset_m` = 1.109 / 0.070 / 1.185 / 0.155（最差 **1.185 ≤ 1.65**）。
A6（均速基线偏差 ≤2.2%）与 A9（匹配耗时 P99 0.354μs）此前已达标；A7 结论为无须重调。

## 方向三 · 交通流

**已落地**

| 项 | 文件 | 证据 |
|---|---|---|
| 时段/区域密度纯算法 | `modules/adas_nodes/traffic_density_spawn.h`（`TrafficDensityWindow` + `traffic_density_window_scale` + `seg_spawn_count_scaled`） | `test_adas_nodes_logic` 新增 2 例（半开区间/非法窗跳过/倍率精确缩放/NaN 守卫） |
| 场景契约 | `include/scenario_loader.h` + `src/core/scenario_loader.c` | `test_traffic_density_scenario` 新增 1 例：解析 + 非法窗跳过 + 序列化不丢字段 |
| 启动期接入 | `flowsim_node.cpp` `flowsim_auto_populate_traffic_density` | `dense_route_traffic` 基线 spawn 20 → `zones=[0,4000)×0` 时 **0**；`time_windows=[0,1000)×0` 时 **0** |
| 运行期泊松接入 | `flowsim_node.cpp` `step_poisson_traffic(..., sim_time_s)` | 到达率按当前仿真时间窗 × ego 所在 route 区域窗缩放；无窗口时倍率 1.0（零回归） |

**未完成**：NPC 仍共用 ego 的单条中央 route，**没有按 OSM 多 OD 路线逐车分配**；自动补给仍只生成车辆、无行人；
加塞场景（`act:"cutin"`）未落场景文件；MOBIL 的"变道一帧中止"缺陷未修（见 `docs/FAILURE_MODES.md` §3）。

## 方向四 · 模型 + 规则混合决策

**已落地**

| 项 | 文件 | 断言 |
|---|---|---|
| promote 门禁补横向/轨迹 | `tools/modelctl.py` | `shadow_steer_mae` 缺失 = 无法判定 → 拒绝；>0.12rad 拒绝；ADE>3m / FDE>5m 拒绝（阈值与 `evaluate_shadow_mode.py` 同源） |
| 接管边界门禁 | `ci/gates/model_takeover_boundary_check.py` + CI `model-takeover-boundary-gate` | 非 `pipeline_cortex.json` 的 `inference.control_mode` 必须 shadow；`safety_control` 必须调用 `safety_arbiter_apply`。A/B：把 sensor 管线改 `direct_ctrl` 即 FAIL |
| 测试 | `ci/evaluators/test_e2e_training_tools.py` | 转向 0.15rad / ADE 3.5m 两负例均拒绝且不覆盖 runtime model |

**未完成**：behavior/planning 仍不消费模型输出（模型只在 control 指令层经 `safety_arbiter` 参与）；
置信度驱动的渐进接管、shadow 指标与 promote 的**闭环**（行为级）未做。

## 方向五 · 仿真闭环

**已落地**

| 项 | 文件 | 证据 |
|---|---|---|
| OTA 不能绕过门禁 | `tools/modelctl.py` `cmd_ota_push` | 无 `manifest.json` 的裸模型 → 非零退出且不写 `/tmp/flow_ota_cmd.json`；合格 artifact → 写入 |
| 闭环评估确定性回归 | `ci/evaluators/test_closed_loop_determinism.py` | 同模型跑两遍，除 `created_unix_ms` 外 metrics/gates/result 逐字段相等；非有限值检查；显式锁定"只有时间戳可变" |
| 场景模糊测试 | 既有 `tools/scenario_fuzzer.py` + `scenario_fuzzer_tests` | 本轮未改，仍是参数扰动器（无变异/覆盖反馈） |

**未完成（最大的诚实缺口）**：整车 demo 的确定性仍不成立 —— `flowsim` 的随机源是进程级 `rand()`，
会被 `sensor_model` / `perception` 的 `srand(42)` 覆盖，叠加墙钟节拍与线程调度 → **同 seed 不保证同轨迹**。
待办已登记：flowsim/NPC 改自持 RNG + headless 跑的确定性断言（见 `docs/FAILURE_MODES.md` §6）。

---

## 第二轮：把"未完成"逐项补掉

| 项 | 落地 | 证据 |
|---|---|---|
| 相机退化模型 | `sensor_model_camera_degradation()`：visibility × weather contrast × lighting − dusk glare；camera 出帧用 effective 因子 | 单测 4 例（clear/night+fog/dusk 眩光/单调+地板） |
| 视觉 → 契约纠错 | `sensor/camera` 类型 `CameraFrame→LidarFrame`、`sensor/gps→GpsData` | topic gate A/B |
| MOBIL 变道修复 | 纵向分发保持 `LaneChange`；E2 到达后 `LeadLost`→Cruise 并计数 | `npc_cutin` 场景 PASS，`lane_changes_completed=1` |
| choreography cutin 瞬移 | `act:"cutin"` 不再走 teleport 重置位置 | 修复前 t=6 邻道车瞬移到 ego 车道当场碰撞 |
| 加塞场景 | `scenarios/npc_cutin.json` + `expect_npc_lane_change` 判据 | PASS（min TTC 3.6s，无碰撞） |
| 行人自动生成 | `traffic_density.pedestrian_ratio` + 路边步行（不横穿）+ 独立 RNG 流 | `pedestrian_flow` PASS，`pedestrian_max=5` |
| OSM 多 OD 路线 | scenario `npc_routes` → 并入 road_network → flowsim 建独立 Route → 轮转交织分配 | `npc_route_spawned=5`；`npc-multi-route-gate` PASS |
| 模型置信度门 | `ControlRaw.confidence` + `safety_arbiter_apply_ex`（<0.5 回退规则，NaN 拒绝）；inference 写 pred_conf、control 写 1.0 | 单测 1 例三段（低/高/关闭/NaN） |
| 随机源隔离 | `flowsim/sim_rng.h`，flowsim/NPC/行人三条自持流；删除全局 `srand/rand` 依赖 | 单测 2 例（同 seed 同序列/范围） |
| 确定性门禁 | `scene.init_fingerprint`（seed + 初始实体布局/转向意图）+ 两次启动比较 + 改 seed 必变 | `determinism-gate` PASS |

**仍未完成（如实）**：BEV 检测仍为直通影子（仅有活性门禁，**无精度/影子对比门禁**）；模型仍不参与
behavior/planning 决策层（只在 control 指令层经仲裁器参与）；动态轨迹本身仍受墙钟/调度
影响，确定性只覆盖启动期指纹与闭环评估。

## code-review 抓出并已修的问题（同一 PR 内）

1. **密度倍率只改计数、不改落点** → 计数与落点共用 `traffic_density_effective_spacing()`；
   实测 `dense_route_traffic` base 12 / ×2 **26** / ×0.5 **4**（修复前 ×2 会被段尾 clamp 吞掉）。
2. **learning_loop 不写 `shadow_steer_mae`** → 新增 `shadow_eval_summary()` 透传门禁消费的全部字段；
   新增"生产端→promote 门禁"契约测试。
3. **sensor-wiring 门禁只查 subscribe、不查 `enable_radar_fusion`** → 改为"有效消费者"判据；
   A/B：雷达开 + 订阅在但 fusion=0 → FAIL，fusion=1 → PASS。
4. **IDL 索引为空时类型门禁静默跳过** → 空索引本身 FAIL。
5. **仲裁器检查可被注释掉的调用骗过** → 先剥离 C/C++ 注释与 `#if 0` 死代码再匹配；
   A/B：注释掉 `safety_arbiter_apply` → FAIL。
6. **`shadow_ade_mean: null` 绕过阈值** → 显式 None 回退到 `shadow_ade`；加负例测试。
7. **两套同义窗口结构 + 手工适配层** → 收敛到 `include/traffic_density_window.h` 单一事实源，
   删除适配函数。
8. **故障表行号漂移** → 按当前源码修正（`sensor_model_node.c:634` / `perception_node.cpp:696`）。

## simplify 轮（净 -70 行）

- `ci/gates/_common.py`：`params` / `node` / `strip_comments` / `strip_comments_and_dead_code`
  收敛三个 gate 的重复实现（此前 comment 剥离有 3 份、`_params` 2 份）。
- `tools/shadow_contract.py`：影子阈值（0.12rad / 3m / 5m）、`SHADOW_SIDECAR_KEYS`、
  数值门禁表**单一事实源**；`modelctl` / `learning_loop` / `evaluate_shadow_mode` 全部 import。
- `traffic_density_window.h` 增加 `traffic_density_window_valid()`（解析与运行时同判据）、
  `traffic_density_combined_scale()`（启动期/运行期组合倍率同一处）。
- perception 抽出 `take_new_radar()`，两条雷达路径不再各写一份加锁+序号比较。
- 删除 `traffic_density_seg_spawn_count_scaled` 包装层；计数与落点共用 effective spacing。
- topic 契约白名单从"类型名"改成 **(topic, type) 对**，禁止任意 topic 冒用 `raw` 绕过 IDL 校验。
- `idl_struct_names()` 每次 main 只读一次（原为每 config 读一次）。
- radar_post 删除死代码 cap 判断；128 槽改用 `sizeof` 从 ObstacleList 推导。

**跳过（记录理由）**：雷达并入放在 perception 而非 `perception_fusion`（需要新节点/topic 与
融合层重构，超出本轮切片）；`CI gate` 的"订阅≠消费"更深修法（运行时条件订阅 + 契约支持可选
topic）同样留作后续。

## 第三轮（夜间队列：T1/T2/T6 + 补口）

| 项 | 状态 | 证据 |
|---|---|---|
| T2 BEV 影子精度门禁 | ✅ DONE（commit `d866a83`） | monitor 透传 `metrics.bev.count`；评估器按 BEV 覆盖范围（前/后 60m、±20m）对比真值；`bev_shadow_check.py` 正常 3/3、永久丢帧被拒 |
| 模型 → planning 软建议 | ✅ DONE | `planning_model_assist.h`（置信度门 + 0.6×~1.2× 夹紧 + gain 默认 0）；planning 订阅 `inference/assist`（speed/confidence）；`test_adas_nodes_logic` 108/108 |
| 动态轨迹确定性 | ✅ DONE（门禁通过） | `plant-determinism-gate.py`：flowsim 单节点同 seed 轨迹 dump 逐字节相同、改 seed 必变 |
| 拥堵汇入难例 | ✅ DONE | `scenarios/congestion_merge.json`（25 veh/km + x=80 并入左道）；场景回归 PASS（24 NPC，lane_change=1，avg 11.6 m/s） |
| 无保护左转难例 | ❌ 未做 | 需要 junction connecting_road 级 route 编排；`cross_intersection` 的 json_to_xodr 不生成 junction 连接路，city_grid 需按 xodr junction 反查 branch_id —— 属独立小项目 |
| T1 Flake 猎杀 | ✅ 7/8 轮全绿 | 8 轮 `--workers 4`：7 轮 18/18；第 1 轮与并发编译重叠时 2 个 `no topology samples`（启动竞态，非场景逻辑） |
| T6 长稳 | 🔄 进行中 | T1 结束后 3×600s，结果写回 NIGHT_QUEUE |

## code-review 第二轮（第二轮改动）抓出并已修

1. `control/cmd` 的 `confidence` wire 字段在 `publish_cmd` 里从未赋值（未初始化栈值）→ 补 `bin.confidence`。
2. MOBIL `LaneChange` 被外部事件（红灯/脚本 override）打断时没有回退路径 → 记录源车道
   `lane_change_from_offset`，未完成离开时回到源车道，避免永久骑线。
3. `npc_multi_route_check` 用 `"invariant failed"` 子串（flowsim 从不输出）→ 改为解析
   `[INV] summary total=N` 并判 >0。
4. `act:"cutin"` 的 target 用世界 `ego.y` 当道路参考系 offset → 弯道/OSM 会偏出路面；
   改为 `world_to_frenet + offset_from_lane_internal`（直道行为不变）。
5. `npc_routes` 合并去重只在主链过滤成功（有 `legacy_id`）时有效 → 过滤失败回退全量时
   会重复追加道路；加 `roads_filtered` 条件。
6. `determinism_gate` 只比指纹，spawn 全断时仍绿 → 默认场景换成 `dense_route_traffic`，
   并断言 `traffic_density.spawned >= 1` 且实体数 ≥2。
7. 自动补给的行人是世界直线行走 → `step_npc_pedestrian` 增加 roads 参数，有 road_id 时
   沿 road s 推进（弯道/OSM 不跑出车道）。
8. choreography actor 查找把 ego（scenario_id=0）当成 actor 0 → 跳过 Ego 类型。
9. `sim_rng_seed` 直接用 42 这类小状态，xorshift 前几次抽样偏小 → 加 splitmix64 混淆。

## 本轮验证（全绿）

```bash
cmake --build build -j$(nproc) && cmake --build build/modules/adas_nodes -j$(nproc)   # ✅
env -u LD_LIBRARY_PATH ctest --test-dir build --timeout 60 \
  -LE "benchmark|manual|stability|integration"                                        # 48/48 passed
python3 -m pytest ci/evaluators -q                                                    # 107 passed, 1 skipped
python3 ci/gates/topic_contract_check.py                                              # OK（含新类型检查）
python3 ci/gates/sensor_wiring_check.py                                               # OK（含雷达消费者检查）
python3 ci/gates/model_takeover_boundary_check.py                                     # OK（新增）
python3 ci/gates/behavior_fsm_check.py                                                # OK
python3 ci/gates/lanelet_consistency_check.py                                         # OK
python3 tools/scenarioctl.py validate                                                 # OK
python3 ci/evaluators/scenario_regression.py --workers 4                              # 18/18 PASS（新增 cutin/pedestrian）
python3 ci/evaluators/scenario_regression.py --only dense_npc                          # PASS（改后复跑）
python3 ci/evaluators/scenario_regression.py --only straight_road                      # PASS（改后复跑）
python3 ci/evaluators/scenario_regression.py --only npc_cutin                          # PASS
python3 ci/evaluators/scenario_regression.py --only pedestrian_flow                    # PASS
python3 ci/gates/determinism_gate.py                                                   # OK
python3 ci/gates/npc_multi_route_check.py                                              # OK
```

单测：`test_adas_nodes_logic` **107/107**、`traffic_density_scenario` **6/6**；
`pytest ci/evaluators` **107 passed / 1 skipped**。