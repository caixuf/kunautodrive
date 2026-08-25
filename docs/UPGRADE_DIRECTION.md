# 升级方向（Upgrade Direction）

> 背景：默认场景（直道 `scenarios/straight_road.json`）在最近的提交链上出现"车辆跑偏 /
> 仪表盘无数据"的回归。本文记录根因分析结论与下一步升级方向，供后续在特性分支上
> 修复时参考，避免再把已知可用的 `main` 推坏。

---

## 1. 现象

- 默认场景运行后车辆横向跑偏，行为不符合预期。
- 仪表盘 / monitor 长时间 `waiting for data`，`/tmp/flow_topology.json` 超时。
- 将整个仓库回退到 `a065f26bd9946c98fcd44bd3424564b829a9fae7` 后现象消失，恢复正常。

## 2. 提交链（按时间，remote `origin/main` 在 `a065f26` 之上的 6 个提交）

```
a065f26  ── 已知可用基线（main 当前应停留在这一状态）
  │
  ├─ dc51051  feat: implement map preview mode ...
  ├─ a5509c5  feat: add BEV multi-modal perception detection head   ← 嫌疑最大
  ├─ 09a12b4  feat: add city_comprehensive test scenario ...       ← 最初被指认
  ├─ 014db3a  test: update scenario baselines + add city_comprehensive
  ├─ d783099  fix: enforce LF line endings via gitattributes
  └─ 335306b  refactor: update flowsim and planning node interfaces
```

## 3. 根因分析

### 3.1 最初怀疑：09a12b4 的两处改动

`09a12b4` 同时改了两条逻辑：

1. **flowsim 初始对齐（snap）被收窄**：从"dist>15m 或 unset 才对齐"收窄为"仅当
   ego 严格为 (0,0,0) 才对齐路由起点"。理论上会让未对齐的 ego 不再被纠正。
2. **planning 的 `target_lane_offset` 分支**：在 `g.map_ref_count>0 && !g.has_behavior`
   时强制 `offset=0.0`（道路中心），在正常车道保持场景里把车强行拉到路中心。

### 3.2 实测推翻了"snap 是主因"

针对 (1) 放宽 snap（在朝向偏差 >90° 时也触发对齐）后，日志显示 ego 被强制转到
heading=0°(+x)，但路由实际行进方向是 −x（向西，`navigation.log: reverse=1`）。
后果：

```
UTURN_FLAG 0 → 1
control.log: 持续 DATA_TIMEOUT — lane-following fallback
车辆冻结 / 仪表盘 waiting for data
```

即**该修复本身会把车冻住**，因此回退了该 snap 改动。这说明 snap 收窄不是跑偏的主因。

### 3.3 真正主因（高概率）：a5509c5 — BEV 感知架构升级

关键证据链：仅回退到 `a065f26`（**同时去掉了 a5509c5**）后一切恢复正常。
而 `09a12b4` 的修复只动了 snap / planning，**完全没碰感知**——这正是"为什么只修
09a12b4 的两处仍然没好"的原因。

`a5509c5` 是感知架构的大改（新增 `bev_pre.c` / `bev_post.c` / `bev_detection_node.cpp`
约 1700 行，DBSCAN+最近邻 → BEV 检测头）。在没有真实模型权重（no-model / 直通模式）
时，其坐标变换或检测输出若存在偏差，会：

```
perception 误检/漏检障碍物
   → fusion 输出异常
   → planning 误避让 / 目标横向偏移异常
   → 车辆横向跑偏
```

因此**默认场景跑偏的最可能根因是 a5509c5 的 BEV 感知升级在直通模式下的回归，
而非 09a12b4**。

## 4. 当前决策

- **`main` 回退到 `a065f26` 作为已知可用基线**，保证默认场景 / 仪表盘开箱即用。
- **BEV 感知升级 + city_comprehensive 场景作为特性分支保留**，不在 `main` 上直接推进，
  避免再次把可用场景推坏。
- 本次不深挖 a5509c5 的感知回归（用户决定"算了"），改为文档记录方向，后续在分支上修。

## 5. 下一步升级方向（推荐路径）

目标：在保留 `city_comprehensive`（OSM 路线跟随）这个新场景的同时，让默认场景
依然可用。建议按以下顺序在特性分支（如 `feature/bev-perception-fix`）上推进：

1. **先把升级链从 `main` 隔离到分支**
   - `git branch feature/bev-perception-upgrade 335306b`（保留 6 个提交，不丢工作）。
   - `main` 停在 `a065f26`，保证默认场景可用。

2. **定位 a5509c5 的感知直通回归**
   - 在 BEV 节点 no-model/直通模式下，对比 `perception/obstacles` 与旧 DBSCAN 基线
     的输出（数量、坐标、速度），确认是否误检/坐标翻转。
   - 重点检查：图像/点云 → 自车坐标系的变换矩阵；检测头在空模型时的占位输出是否为
     全零或退化值（导致 fusion 异常）。
   - 建议加一层 **shadow mode**：BEV 输出与旧感知并行跑，monitor 对比两者，
     回归未修前不接管主链路。

3. **修正 planning 的 `target_lane_offset` 分支**
   - 仅当 `!g.has_behavior && !g.has_navigation` 时才进入"地图路线巡航 -> offset=0"
     分支；对有 `navigation/path`（route_count>0）的正常车道保持场景不应强制拉到路中心。
   - 这条与 a5509c5 无关，可独立于感知回归先修。

4. **snap 逻辑保持现状**
   - 仅 (0,0,0) 才对齐路由起点的行为经实测是正确的；不要为"跑偏"而放宽，
     放宽反而会把车转到逆着路由行进方向并冻结。

5. **回归验证**
   - 默认场景 `straight_road.json`：车辆不跑偏、仪表盘正常出数。
   - `city_comprehensive.json`：路线跟随可用（可选，待感知修好后再启用）。
   - 跑 `/verify` 后再合回 `main`。

## 6. 遗留待办

- [ ] 在分支上复现并定位 a5509c5 BEV 直通模式回归（perception 输出对比）。
- [ ] 加 shadow-mode 对比，避免再次污染主链路。
- [ ] 修 planning `target_lane_offset` 的 `!g.has_navigation` 守护。
- [ ] 默认场景 + city_comprehensive 双场景回归通过后再合 `main`。

---

*注：本文为方向性记录，不是修复补丁。当前 `main` 应停留在 `a065f26` 已知可用状态。*
