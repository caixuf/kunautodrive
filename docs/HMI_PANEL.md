# HMI 专区（FlowBoard 内嵌驾驶界面）

> 面向人看的驾驶 HMI，**实时联动仿真**。与 3D 调试视图分开：3D 是"看世界"，HMI 是"看车在想什么/在做什么"。
> 对标参考：Tesla FSD v15「Reality View」（单一端到端可视化、see & control plan）。

## 位置

- 前端：`tools/flowboard/index.html` 的 `#hmi-card`（`ws-observe` 工作区，默认「观察」页可见，可折叠）。
- 逻辑：`tools/flowboard/js/vis/hud/HmiModel.js`（纯函数，可单测）。
- DOM：`tools/flowboard/js/vis/hud/HmiPanel.js`（薄渲染，仅写 DOM，不做推导）。
- 样式：`tools/flowboard/css/hmi.css`（复用 `css/style.css` 的 `:root` token）。
- 接线：`js/app.js` 的 `updateHmi()`（4Hz 限流，折叠时零开销），在 `updateAll()` 里调用。

## 数据流（不新开连接）

```
monitor_node → /tmp/flow_topology.json → flowmond /api/stream (SSE) → app.js 解析 topoData
   → buildHmiState(topoData)  → updateHmiPanel(body, state)  → 四分区 DOM
```

驾驶/诊断部分**复用** `computeDrivingStatus(metrics)`（与 #drive-hud 同一事实源，禁止第二份实现）。

## 四分区

| 分区 | 数据字段（topo.metrics） | 状态 |
|------|------------------------|------|
| 驾驶核心 | `scene.ego`（速度/姿态）、`behavior`、`control_debug`、`scene.lane`、前车/TTC | ✅ HMI-1 |
| 感知 / 环境 | `scene.perception_entities` / `entities` 计数、`bev.count`、`metrics.occupancy`（traversability 真栅格） | ✅ 计数 + mini BEV 占用图 |
| 规划意图 | `behavior`（committed/target lane、route_goal、变道间隙、并道距离）+ `scene.trajectory_path`（轨迹片段）→ 意图文本 + 车道梯 + mini path | ✅ HMI-1/HMI-3 |
| 态势 / 诊断 | `p2p.locLabel`、`tor`、`l3_events`、`odd` | ✅ HMI-1 |

## 打磨流程（联动仿真）

```bash
bash scripts/demo.sh --no-browser          # 起管线 + flowmond
# 浏览器打开 http://localhost:8800 ，「观察」页即见 HMI 驾驶界面卡片
```

改 HMI 外观/内容只动 `HmiModel.js`（映射）+ `HmiPanel.js`（DOM）+ `hmi.css`（样式），不碰 C 链路。

## 门禁

```bash
npm run hmi:check      # hmi_model（纯逻辑 68 项）+ hmi_panel（DOM 冒烟 31 项）
npm run vis:check:all  # 含 hmi:check，合并前必须全绿
```

- `HmiModel` 缺字段/空数据一律降级占位，绝不抛错（有单测锁）。
- 无数据不显示假值（本仓 fake-green 禁忌）。
- `tools/flowboard/**` 改动合并前 `npm run vis:check:all` 必须绿（CLAUDE.md 3D 门禁铁律）。

## HMI-2（占用栅格，已接入）

数据链：`traversability_node` 把 `g.grid[]`（FREE=1/OCCUPIED=2/UNKNOWN=0）压成 `cells_b64`
（约 5.3KB）+ `grid_w/grid_h/cell_size_m/origin_x/origin_y/frame` 写进 `perception/traversability`；
`monitor_node` 订阅并带锁透传到 `metrics.occupancy`；`HmiModel` 解码成 `Uint8Array`；
`HmiPanel` 在 `#hmi-occ-canvas` 上画 mini BEV（FREE 绿 / OCCUPIED 红 / UNKNOWN 底，车头朝上）。

- 索引 = `gx*grid_h + gy`（gx 前向 0→x_range，gy 横向 0→2*y_range）；车身系。
- 默认配置实测：`108×37 @0.25m`，约 530 occupied / 3466 free cell。
- 局限：默认管线是**障碍-only**配置（FREE = 无障碍且地面内，非稠密深度真值），如实标注，不吹成稠密 occupancy 网络。
- 时域融合（stereo log-odds 路径）：按 ego 速度自适应衰减 `decay = base·exp(-k·v)`，`k=grid_decay_speed_k`（默认 0.02，0=关闭，clamp [0.3,0.995]），减前向拖影、零重采样；单测 `test_traversability` T6/T7 覆盖（含 k=0 关闭与 0.3 下界）。obstacle-only 默认路径每帧重建、不经过此段，行为不变。

## HMI-3（规划意图增强，已接入）

数据源全在 `metrics.behavior`（behavior_planner 真实输出）+ `metrics.scene.trajectory_path`，
前端只做归约，不重算规划：

- **目标车道梯**（`.hmi-lanes`）：`lane_count` 个方框，当前车道蓝（`committed_lane`）、目标车道琥珀（`target_lane`）；
  文本 `当前 L{cur+1}/{n} → 目标 L{tgt+1}`，另标 `路线 L{route_lane_goal+1}`。
- **规划轨迹片段**（`#hmi-traj-canvas`）：`trajectory_path` 世界 ENU → ego 相对（`Coord.forwardENU`，
  禁裸 `Math.sin/cos`）→ 近场 clip ≤60m → 前向上画折线；倒挡段(v<0)红色。
- **并道 / 缩线**：`dist_to_lane_drop_m`（前方 X m 并道）/ `upcoming_own_lanes` / `blocked|trav_blocked`。
- **变道间隙**：`left_ok/right_ok` + `left_gap/right_gap` → `左 ✓5.5m · 右 ✗—`。

## 完成度增强（告警 / 转向 / 图例 / 快捷键）

- **顶部告警横幅**（`#hmi-alerts`）：聚合 TOR/MRM、`degrade.active`、`safety_evidence.fault`（结构体，
  `type/id="none"` 视为健康）、`action.immediate_stop/mrm_stop/disable_lane_change`、控制告警、碰撞风险、
  受阻、ODD 越界/驶出/定位发散、L3 事件；按最高严重度整条着色（bad 时脉冲）。全正常显示「全部正常」。
- **转向指示**（`#hmi-blinker`）：由 behavior 机动派生 `◀ 左` / `右 ▶` / `⚠ 双闪`（MRM）——仅表达意图，非原始灯控。
- **占用栅格图例**：`空闲 / 占用 / 自车·前向上`，避免小图看不懂。
- **快捷键 `T`** 折叠/展开 HMI 卡片（折叠态经 `saveState` 持久化）；卡片表头也可点。

## 后续（可选，未做）

- 占用栅格世界系运动补偿（当前 `traversability` 时域融合未做运动补偿，车大幅移动时旧观测滞留衰减）。
- 让行/停止线倒计时（需后端在 behavior 里暴露计时量，当前无此字段，不臆造）。
