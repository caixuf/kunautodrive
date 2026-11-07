# 五个方向的落地状态 — 2026-11-07

> 目标：把 `docs/ROADMAP_L3.md` 的方向一~五从"路线图"推进到**可验证的落地切片**。
> 本文记录本轮实际做了哪些代码/门禁、用什么命令可复现，以及**仍未完成的部分**。
> 判断标准沿用仓库铁律：能跑、能过门禁、有实测证据；**不把"写了文档"算完成**。

## 一句话结论

| 方向 | 本轮状态 | 一句话 |
|---|---|---|
| 一 · 多模态感知 | 🟡 雷达链路打通 | `RadarFrame → ObstacleList` 纯转换 + perception 可选并入 + LiDAR 失效时雷达单独出帧 + 接线门禁；视觉仍为直通影子 |
| 二 · 地图自动化 | ✅ D2-10 收口 | 4 场景 × 3 次实测 12/12 PASS，最差横向 1.185 ≤ 1.65；A1–A6、A8–A11 达标 |
| 三 · 交通流 | 🟡 密度可按时间/区域调 | `time_windows` / `zones` 纯算法 + 场景解析 + 启动期与运行期泊松接入；OSM 多路线分配、加塞场景未做 |
| 四 · 混合决策 | 🟡 门禁闭合 | promote 门禁补转向/轨迹阈值；新增 `model-takeover-boundary-gate`（非实验管线禁 `direct_ctrl`、仲裁器必须在链上）；模型仍不参与行为决策 |
| 五 · 仿真闭环 | 🟡 部署门禁 + 确定性评估 | `ota push` 必须过 promote gate；闭环评估新增确定性回归测试；整车仿真确定性仍受全局 `rand()`/墙钟限制 |

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
```