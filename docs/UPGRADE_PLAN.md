# FlowEngine 通用性 + 感知/渲染升级计划

> 基线：`main` @ `ead078c`  
> 已验收：full21 600s PASS；`intersection_fork` / `intersection_right_turn` / `intersection_straight` / `one_lane_two_way` / `lane_drop_merge` 已 enabled 并通过 baseline；`intersection_merge` disabled（1→2 车道切换失败）。  
> 目标：不把“仿真场景通过”当成“通用能力通过”；每一项升级都必须有可复现 fixture、量化判据和门禁。

---

## 0. 先给结论

1. **Tesla FSD 的渲染不是“实时 3DGS”**。更接近：地图/车道拓扑 + 检测/跟踪 + occupancy/神经世界模型 + 精心设计的 HMI 着色，训练和推理都依赖海量数据与算力。3DGS 目前更适合离线重建、合成数据、golden scene，不适合作为车端实时 FSD 渲染的主路径。
2. **我们现在的短板不是 Three.js 画不出框**，而是：
   - 感知输入只有仿真的 16 线 LiDAR + DBSCAN + BEV-Lite，缺少相机语义、dense occupancy、遮挡/时序融合；
   - 前端 `PerceptionView` 目前是“角标框 + 点云 + 预测光带”，不是参考图那种完整 3D wireframe + ID/速度/车道/TTC；
   - 规划通用性还差跨车道数模型、环岛/停止标志/通行权、动态障碍扫掠、merge/sensor 场景矩阵。
3. **升级顺序**：先把“可见、稳定、可验收”的感知可视化与 merge 1→2 修绿；再做 occupancy/相机融合；最后才做神经世界模型/3DGS 离线管线。

---

## 0.1 执行状态（2026-11-26）

| 项 | 状态 | 证据 |
|---|---|---|
| demo.sh 旗舰演示场景 | ✅ 已完成 | `DEFAULT_SCENARIO=scenarios/city_procedural_world.json`：4 车道 CBD → 坡道爬升 → 6m 高架桥，环境车流 10 辆；`bash scripts/demo.sh --no-browser 45` PASS（x 1054m / avg 11.8 m/s / min road margin 0.64m） |
| 感知 3D 线框 | ✅ 已完成 | `PerceptionView` 8 顶点 12 边完整外框 + 分类色（车青/人琥珀/两轮绿/静态灰），装饰建筑/桥墩不再画框；`tests/vis_perception_wireframe.test.mjs` 10 项断言并入 `npm run vis:check` |
| demo 收尾汇总 | ✅ 已完成 | 改读 `/tmp/flow_topology.json`，输出车速/定位/规划/控制/行为/BEV；不再出现空值行 |
| BEV 城市场景假 FAIL | ✅ 已完成 | 真值不再统计 WorldEngine 装饰设施（sensor_model 不对其投射 LiDAR）；城市场景 `count 1 vs truth 5` 在容差内 PASS |
| 感知 GT/Perception/Diff 三态 | ⬜ 未做 | 当前用 `store.perceived` 与真值合并去重；三态开关需要后端 `source` 切换 + UI |
| merge 1→2 车道模型切换 | ✅ 已完成 | `intersection_merge` 已 enabled + baseline：x 270m / avg 9.08 m/s / min road margin 0.38m / max lane error 0.37m。根因不是跨模型，而是短路段 U_TURN（<360m 从起点就掉头）+ ego 起点偏离匝道车道中心 1m |
| 环岛 / 停止标志 / 通行权 | ⬜ 未做 | 无 fixture |
| 动态障碍扫掠 | ⬜ 未做 | 仍为点测，未按 TTC/同车道过滤 |
| sensor 场景矩阵 | ⬜ 未做 | 仍只有 nightly 三场景 |

## 1. 差距拆解

### 1.1 感知 / 渲染

| 维度 | 当前 | 目标（分阶段） |
|---|---|---|
| 目标框 | 8 角标 L 形 bracket；颜色/尺寸较粗 | 完整 3D wireframe + 角标双模式；类别色、ID、速度箭头、置信度、lane id、TTC |
| 目标生命周期 | 依赖 tracker topic；前端有 0.85m 去重 | ID 稳定、coast/occlusion 可见、丢失淡出、误检标记 |
| 场景理解 | 点云 + 目标框 + 地图车道 | occupancy/free-space 栅格、可行驶走廊、动态/静态分层、预测多模态 |
| 传感器覆盖 | 前视包络扇区 + 22m 弧 | 多传感器 FOV、遮挡锥、点云密度热力、盲区提示 |
| 真值/感知对比 | 前端混用 `entities`（真值）与 `perceived` | 三态开关：GT / Perception / Diff；差异高亮 |
| 夜景/材质 | SR 风格 + 基础光照 | 夜间/雨天/雾天可读性；不追求写实照片级 |
| 世界模型 | 无 | 离线 3DGS/NeRF 重建用于 golden scene + 合成数据；车端用 occupancy/scene graph |

### 1.2 规划通用性（五条计划）

| # | 现状 | 缺口 |
|---|---|---|
| 1 | `ref_path` 逐点 `road_id/lane_id/lane_count/oneway/lane_width`；planning 在**同 lane_count/oneway/width** 时用 ref 模型；route-aware lane_match 已接入 | 1→2 merge 已 PASS（根因是短路段 U_TURN，不是跨模型）；**8→2 车道缩减**仍无 target slew / 迟滞，是下一步 |
| 2 | behavior 接管 route lane goal，planning 只消费 Behavior | 完成；后续扩展 branch/merge/roundabout 的导航意图 |
| 3 | 左转/右转/直行/分叉已可验收；fork 已 enabled | 环岛、停止标志、无信号路口通行权、gap acceptance、转弯速度/曲率 |
| 4 | 静态障碍扫掠过滤 + `traj.valid=0` 已接入 | 动态障碍仍点测；raw sweep 命中率高，不能直接拦 |
| 5 | 4 个新 enabled 门禁 + fork；merge disabled | merge / branch_select / roundabout / sensor 矩阵未覆盖 |

---

## 2. 目标架构

```
传感器仿真/回放
  LiDAR(16→32/64) + Camera + Radar + GPS/IMU
        │
        ▼
感知前端：BEV 栅格 + 2D/3D 检测 + 车道线 + occupancy
        │
        ▼
融合/跟踪：多目标 UKF/匈牙利 + 生命周期 + 车道关联 + 预测
        │
        ├──► 可视化：GT/Perception/Diff 三态 + 3D wireframe + occupancy
        │
        ▼
规划：per-point lane topology → 跨模型 target slew → 路口/环岛 maneuver
        │
        ▼
控制：轨迹跟随 + ROAD_GUARD（按 route 参考系，不按世界 y）
        │
        ▼
验证：scenario gate / invariant / long-run / sensor matrix / perception overlay gate
```

**单一事实源原则**：车道拓扑、参考线、目标车道只允许一份权威实现；禁止在 planning/control/前端各写一套公式。

---

## 3. 工作流

### A. 感知可视化与 QA（P0，先做）

**目标**：把“感知框”做成可信的调试/演示界面，而不是装饰。

1. `PerceptionView` 增加完整 3D wireframe 模式：
   - 8 顶点 12 边立方体；类别色；朝向箭头；速度矢量；ID/置信度/lane id/TTC 标签（距离近的显示）。
   - 角标框保留为“轻量模式”，可切换。
2. 三态数据源：GT（`scene.entities`）/ Perception（`store.perceived`）/ Diff；Diff 用红/黄高亮漏检/误检/位置偏差。
3. 生命周期：track id、coast、occlusion、lost fade；避免前端 0.85m 去重把相邻目标合并。
4. 传感器覆盖：LiDAR FOV/距离环、相机视锥、遮挡锥、盲区；夜间/雨天可读性。
5. 新增前端门禁 `perception_overlay_check`：
   - 至少 N 个目标框；ID 连续；框尺寸/朝向/地面高度在物理范围；无 NaN；GT/perception 数量差在阈值内。
   - 截图 golden（puppeteer 无头）用于人工 diff。

**涉及文件**：`tools/flowboard/js/vis/view/PerceptionView.js`、`SceneDirector.js`、`SceneStore`、`monitor_node` 的 `scene.perceived`、`tokens.js`。  
**验收**：`npm run vis:check:all` + 新 overlay 门禁；在 `urban_challenge`/`dense_npc`/`intersection_fork` 三个场景截图对比。

### B. 感知算法 / 数据（P1，中期）

1. 传感器升级：LiDAR 16→32/64 线（仿真可配）；增加相机检测（合成/规则/小模型）；雷达；时间同步。
2. 融合：camera-LiDAR BEV fusion；occupancy/free-space；动态/静态分层；遮挡推理。
3. 跟踪：UKF/CTRV、ID 生命周期、车道关联、cut-in 预测；降低 ID switch。
4. 模型：CenterPoint-Lite → 更强 BEV detector；数据采集（data_recorder）→ 训练 → ONNX/纯 C 量化；保持纯 C 推理核可复现。
5. 3DGS/NeRF：**只做离线** golden scene 重建、合成数据增强、可视化对比；不接车端实时渲染主路径。

**验收指标**：Recall/Precision、位置 MAE、ID switch、遮挡恢复率、BEV occupancy IoU、sensor 模式场景 PASS。  
**风险**：仿真传感器与真实域差异；算力/数据不足；不要用“渲染好看”替代感知指标。

### C. 规划通用性（P1，与 A/B 并行）

1. **跨 lane_count per-point 模型切换**（当前最优先）：
   - `ref_path` 已带逐点 lane_count/oneway/width；
   - 增加 target slew（例如 1.0–1.5 m/s）+ lane_ref 连续性；
   - 切换窗口用 hysteresis，避免 8→2 一帧跳 -12.25→-1.75；
   - `intersection_merge` 修绿并 enable。
2. **branch/merge**：`ROUTE_BRANCH_SELECT`/`ROUTE_MERGE` 场景化；merge gap gate 真跑；branch lane_ref 对齐。
3. **路口/环岛**：停止线、信号相位、通行权、gap acceptance、转弯限速；新增 `intersection_roundabout`、`stop_sign` 场景。
4. **动态扫掠**：在静态过滤基础上，按 TTC/同车道/预测轨迹筛选动态障碍；先诊断计数，再接 `traj.valid`；用 `dense_npc`/`r157_cut_in` 验证不误刹。
5. **行为**：扩展 route 目标到 branch/merge/roundabout；保持“behavior 决定做什么，planning 决定怎么做”。

**验收**：每个新场景 `scenario_regression --update-baseline` + `--baseline`；long-run lane keep；无 fake green。

### D. 验证 / 门禁（P0，贯穿）

1. **每项功能一个 fixture**：失败 fixture 可以 disabled，但必须有量化失败日志和 FAILURE_MODES 行。
2. **门禁分层**：L0 `pipeline_check`；L1 `demo_evaluator`；L1.5 `scenario_regression`；L2 参数回归；L3 交互；长跑 soak。
3. **感知 overlay 门禁**：框数量/ID/尺寸/高度/朝向；GT/Perception diff。
4. **sensor 矩阵**：至少 `urban_challenge`、`dense_npc`、`lane_change_traffic`、`intersection_fork`、`intersection_merge`、`r157_*` 在 sensor 编排跑。
5. **性能**：BEV/检测/跟踪/规划时延预算；长跑内存/句柄。

---

## 4. 里程碑

| 阶段 | 交付 | 验收 | 预估 |
|---|---|---|---|
| Phase 0 | 感知 overlay 三态 + 完整 wireframe；merge 1→2 target slew；动态 sweep 诊断计数 | vis overlay gate；merge baseline；full600 | 1–2 周 |
| Phase 1 | sensor 融合初版（camera+LiDAR BEV）、occupancy；branch/merge 场景；stop-sign 场景设计 | sensor 矩阵 3→6 场景；occupancy IoU | 3–6 周 |
| Phase 2 | 环岛/通行权/gap acceptance；动态 sweep 接入；tracker 升级 | roundabout/stop-sign gate；dense_npc 不误刹 | 6–10 周 |
| Phase 3 | 3DGS/NeRF 离线重建 + 合成数据；更强 BEV 模型；HMI 夜景/天气 | golden scene 重建指标；模型指标；长跑 | 10–16 周 |

> 时间是相对估算，取决于是否有真实数据/GPU/标注资源；纯仿真先行的部分可以更快。

---

## 5. 关键技术判断

1. **不要追实时 3DGS**：3DGS 的训练/渲染成本、动态物体/光照一致性、车端算力都不适合作为 FSD 主渲染；用 map + scene graph + occupancy 做实时，3DGS 做离线重建/数据。
2. **感知框不是“画出来”就行**：必须有 ID、速度、车道、置信度、遮挡、TTC；否则只是装饰。
3. **跨 lane_count 是分水岭**：per-point 字段只是数据契约；真正难的是 target/lane_ref 的连续切换。1→2 已证明几何连续时无需 slew；8→2 的 −12.25→−1.75 阶跃必须做 slew + 迟滞。
4. **环岛/停止标志不是场景数据问题**：需要行为状态、通行权、gap acceptance、停止线/信号消费；不能靠“参考线能转”冒充。
5. **门禁必须能抓已知故障**：新场景先复现失败，再修，再 enable；禁止先写 PASS 基线。

---

## 6. 近期两周的具体动作

1. `PerceptionView`：完整 wireframe + ID/速度/车道/TTC + GT/Perception/Diff 开关；加 `perception_overlay_check`。
2. `intersection_merge`：target slew + lane_ref 连续；修到 PASS 并 enable。
3. 动态 sweep：加 filtered dynamic hit 计数（不拦），在 `dense_npc`/`r157_cut_in` 统计；确认低误报后再接 `traj.valid`。
4. 场景：新增 `stop_sign`/`roundabout` 设计稿（先 disabled fixture + 失败日志）。
5. 全量验收：full600、sensor 6 场景、ctest、vis、静态门禁。

---

## 7. 风险与边界

- **仿真域差距**：16 线 LiDAR/合成相机与真实数据不同；感知指标不能直接对标 Tesla。
- **算力**：BEV/occupancy/3DGS 训练需要 GPU；纯 C 推理核要保持可复现。
- **不要用渲染掩盖感知**：HMI 可以好看，但门禁必须看 Recall/ID/occupancy。
- **场景过拟合**：每加一个场景必须同时有反例/长跑，避免“只会这一个路口”。
- **单点权威**：per-point topology、lane_match、control guard 的参考系必须统一，否则 fork/merge 还会反复。

---

## 8. 完成定义（Definition of Done）

- 五条计划每条都有：enabled fixture + baseline + 失败复现记录 + 量化判据。
- 感知 overlay：框数量/ID/尺寸/高度/朝向/覆盖率门禁；GT/Perception diff 可解释。
- 路口/环岛/停止标志/merge 至少各一个 enabled 场景；sensor 模式至少 6 场景。
- full600 + 长跑 + ctest + vis + 静态门禁全绿；无 fake green。
