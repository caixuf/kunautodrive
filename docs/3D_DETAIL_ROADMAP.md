# 3D 生成细节演进路线（临时文档 / 回家继续干指南）

> 目的：记录 3D 生成架构的**当前状态**、**已列入计划的待办**、以及「如何让生成的 3D
> 越来越细节」的**分阶段演进路线**。写给回家后继续推进的自己/协作者。
>
> 配套阅读：`docs/VISUALIZATION_ARCHITECTURE.md`（3D 总体架构）、`docs/VIS_MODULE_GUIDE.md`
> （渲染模块门禁）、`docs/ROAD_MARKINGS_MODULE.md`（路面标线模块）、
> `docs/MAP_ENGINE_ROUTING.md`（地图/A*）、`docs/FLOWBOARD_SCENE_CONTRACT.md`（数据契约）。

---

## 0. 快速上手（跑起来看 3D）

```bash
# 1) 起预览服务（不跑仿真，纯看地图）
build/bin/flowmond --port 8800 --html-path tools/flowboard/index.html
# 浏览器 http://localhost:8800 → 地图路线 → 下拉选地图 → 「👁 3D 预览选中路线」

# 2) 起完整仿真 demo（默认 straight_road）
bash scripts/demo.sh                      # 默认 15s；或
bash scripts/demo.sh --scenario scenarios/osm_lujiazui.json   # OSM 大地图
bash scripts/demo.sh --scenario scenarios/city_grid_map.json  # 5km 网格

# 3) 3D 门禁（改了 tools/flowboard/ 必须过）
npm run vis:check:all

# 4) 离线静态检查
python3 tools/pipeline_check.py
```

关键目录/文件：

| 作用 | 路径 |
|------|------|
| 3D 渲染视图（路面/路口/建筑/设施…） | `tools/flowboard/js/vis/view/` |
| 坐标/数学（`worldToThree` 收敛，门禁强制） | `tools/flowboard/js/vis/math/Coord.js` |
| 场景数据适配（scenario → 3D topo） | `tools/flowboard/js/showcase/sceneAdapter.js` |
| 数据层导出 / API | `src/core/scenario_loader.c`、`src/core/monitor_server.c`、`modules/adas_nodes/flowsim/scene_pub.cpp` |
| 地图生成 | `tools/extract_city_map.py`、`tools/grid_map_generator.py`、`tools/osm_to_map.py` |

---

## 1. 当前 3D 生成架构（现状速览）

数据流：**场景/地图 JSON → 数据层（scenario_loader / scene_pub / monitor_server）→ sceneAdapter → 各 View → Three.js**

各 View 职责：

| View | 渲染内容 |
|------|----------|
| `RoadView` | 路面 ribbon、路缘、人行道/绿化带、中心线（双黄）、车道分隔线（白虚线）、边缘线（白实线） |
| `ConnectorView` | 路口多边形铺装、斑马线、停止线、匝道渐变、高架桥墩、防撞桶 |
| `RoadFacilityView` | 停止线/斑马线（信号灯推导）、停车位/考试桩、导向箭头 |
| `BuildingView` | OSM 建筑 OBB（footprint+height） |
| `GroundView` / `TrajectoryView` / `VehicleView` / `BarrierView` / `TrafficLightView` / `ETCGateView` | 地面、轨迹光流、车辆、护栏、信号灯、ETC 门架 |
| `JunctionDetect` | 交叉口中心检测（数据层 junctions 优先，几何聚类兜底） |

契约与门禁：

- 坐标统一走 `Coord.worldToThree`（ENU → THREE），`vis_coord_property.test.mjs` 强制。
- 路面标线方向已按国标 GB 5768.3 校正（见 §2）。
- 修改 `tools/flowboard/` 必须通过 `npm run vis:check:all`，新增场景要同步 `tools/flowboard/showcase/scenes.json`（`python3 tools/build_showcase.py`）。

---

## 2. 本会话已完成（做过的细节工作）

- **斑马线方向修正**（GB 5768.3 5.8：条纹应与道路中心线平行）：`ConnectorView.js`
  `_buildJunctionPatch` / `_buildJunctionCap`、`RoadFacilityView.js` `addCrosswalk` 三处
  改为「条纹平行道路、沿道路长度短（2.5m）、跨道路铺条」。新增测试断言
  `tests/vis_road_facility.test.mjs`。
- **路口/标线规范审计**：停止线（白实线横跨来向半幅）、导向箭头、中心线（双黄实线）、
  车道分隔线（白虚线）、边缘线（白实线）均核对合规，无需改动。
- **OSM 连通性**（另一 AI）：`osm_to_map.py generate_connectivity` 掉头保护改为「仅不同
  街道才跳过」→ 陆家嘴环路 -179° 断链修复，运行时 A* 不再 `无路可达`。
- **OSM 大地图**：`maps/osm_lujiazui`（491 路 / 843 路口簇 / 564 建筑），`--check` 通过；
  main 链 9/9 从 lane 0 可达。
- **N-S 掉头方向判定**：`planning_node.cpp` forward_space 改「弧长 + 60m 覆盖护栏」、
  障碍钳制改「车头方向投影」（N-S/E-W 通用）；验证默认 21.0 m/s、city_grid 15.2、ctest 7/7。
- **默认场景回归修复**：回退 planning forward_space 弧长化 + flowsim `u_turn_active` 相对
  路线两处（它们叠加导致起步掉头卡死），恢复 HEAD 逻辑后再以覆盖护栏方式重做 N-S 修复。
- **预览接入**：`monitor_server.c` /api/map/preview 白名单加 `osm_test`/`osm_lujiazui`；
  `index.html` 地图下拉加两项；`flowmond`/`flow_launcher` 重建后端点验证返回完整 map。

---

## 3. 待办清单（已列入计划，按优先级）

- [x] **P0 — A* 起终点启发式修复**（让 OSM 大地图真正能开一段）**✓ 2026-08-14 f20a3eb**
  - 位置：`modules/adas_nodes/flowsim_node.cpp` `build_route_via_astar`
  - 问题：起终点用 `road 0 → road max_road`（过滤后的路序 ≠ 路线链序）→ OSM 上 A*
    只给出 2 路 128m 短路线，且可能不在 ego 脚下 → demo 里 ego 1.2 m/s、beh=NA。
  - 方案：`scenario_loader.c` route 过滤时注入 `legacy_id`=map.json 全量索引（与
    json_to_xodr 全量导出的 xodr road id 同编号空间）；`build_route_via_astar`
    直接以 edges[]（过滤后即 road_chain 顺序）调 `build_from_chain`，A* 仅作链
    不可达时兜底改道（OSM 主链 lane successors 带岔路，起终点 A* 会抄近路跳段）。
  - 验证：osm_lujiazui demo ego 0→14.2m/s 沿主链（9 段 3213m）行驶，planning
    waypoint=25 持续跟随；city_grid 25 段 5000m 零行为变化；straight_road 回退
    正常；ctest 25/25。
- [x] **P1 — 预览大图性能**（✓ 2026-08-14）：Node 实测 SceneDirector.update 3.39s →
      581ms（5.8x），RoadView.build 2.9s → 475ms。三个优化：
      ① **isRampEdge 误判修复**（最大头，占原耗时 87%）：旧判断含 `oneway===true`，
      OSM 330/491 条单行道被当匝道 → 第二遍匝道循环 ramp×main 全组合跑
      findRampConnection（330×161 次 O(n) 点扫）+ 主路画成匝道样式（缺中心黄线）。
      匝道只由 type=ramp_curve 或 name 含 ramp 判定（city_ring/city_center 匝道
      type 均为 ramp_curve，不依赖 oneway，零回归）。
      ② **虚线合批**：dashedLine/dashedLineInRange 每段虚线一个 BufferGeometry
      → 整条虚线合并成单 geo（OSM 数万 geo + 数万次 computeVertexNormals 消除）。
      ③ **offsetSpine 删死代码**：buildCumulative 计算了但从未使用。
      验证：vis:check:all 全绿；浏览器实测 OSM 491 路 + 387 建筑正常渲染，
      105 draw call / 1.88M 三角形，无 JS 错误无黑屏。
- [x] **P2（可选）— OSM A* 连通回归脚本**：`tools/check_map_connectivity.py` 已落地并接入
  CI `map-connectivity-gate` job。遍历所有被 scenarios/*.json 引用的地图，对每条 route
  跑 `astar_route.py --check`（相邻 road 对 lane successor 连通 + 首尾 A* 端点可达）。
  已知断链（city_center / osm_test 陆家嘴环路 14.7m gap）在 KNOWN_BROKEN 白名单 WARN
  不阻断，白名单外任何 FAIL 都 ERROR 阻断合并 —— 与 scenario-file-gate 对称，防止
  "改了地图 demo 里 ego 开不动"类回归。本地验证：5 图全跑通，2 WARN / 0 FAIL。
- [ ] 上述完成后：`vis:check:all` + `pipeline_check` + 环城路 E-W 回归全量过一遍再收。

### 剩余视觉噪点（回家解决，已定位未修）

- [ ] **路口白色细线残留**：`_buildTurnConnectors` 依赖 junctions[].connecting_roads[].turn，
      但 scene_pub 输出 junctions 不含这些字段（只有 {id,x,y,z,radius,n,roads}），
      `fork.type !== 'fork'` 跳过所有 → **实际死代码，不触发**。data-junction 场景
      （showcase 预览）也走 mapToRoadNetwork 无 junctions。标记为不触发，暂不修。
- [ ] **防撞桶已修**（本会话）：`_buildBarrierEndCap` 加孤端检测——端点无任何其他
      edge 端点在 1.5m 邻接容差内才是真断头。OSM 982→236 桶（-76%，仅留边界断头）；
      city_grid 2600→0 桶（网格本就不该有桶）。若预览仍见零星桶，属地图边界正常。
      弯路护栏（BarrierView 金属护栏）只对 type='highway' 生效，OSM 0 条 highway
      不触发，无需额外修。
- [ ] **S 弯外侧路肩/人行道收窄**：offsetSpine 曲率钳制为对称钳制，外侧急弯略收窄，
      观感可接受；如需更精细可改为内侧钳制（注意坐标手性，见 RoadView.js offsetSpine 注释）。
- [ ] **路口斑马线/停止线**：已用路口端向外切线统一基准；若个别不规则路口仍偏，
      可让 detectJunctions 输出每 arm 的权威切线（advisor 建议），替代端点反推。

### 框架 vs 细节：还要修多少？（2026-08-14 评估）

**结论：框架没有大问题，不需要重构。剩余 = 4 个框架级点修 + 大量增量细节打磨。**

**框架级点修（各独立、不连锁，按优先级）：**
1. **A* 起终点启发式**（见上 P0）：`build_route_via_astar` 用 `road 0→max_road`
   而非 route 链首尾 → OSM 上路线短/不在 ego 脚下。修完 OSM 才能真正开。
2. **行进方向参考抽象**：planning/flowsim/behavior 各自推导"前方"
   （forward_space / on_return / U-turn 触发 / 障碍钳制），是反复出现
   "N-S/E-W 假设" bug 的根。抽一个"沿参考路径行进方向"统一工具，
   各模块消费它，不再各自写死世界轴。本会话已回退+重做两处，证明分散推导易错。
3. **路口权威切线**：detectJunctions 输出每 arm 的 armTangent，渲染层
   （polygon/斑马线/停止线）统一读它，不再从 edge 端点反推（修不规则路口）。
4. **转向连接曲线门控**：`_buildTurnConnectors` 对 OSM 过密，加数据门控或
   OSM 地图整体关闭（见上"剩余视觉噪点"）。

**细节级打磨（增量、不碰框架，即 §4 路线图 P0-P5）：**
标线组合箭头/非机动车横道/导流线 → 路口圆角/渠化岛 → 建筑贴图/屋顶/LOD →
标志牌/路灯/公交/树（OSM 数据源）→ 天空/水面/天气 → 大图性能/LOD。
每步都是「拉数据 + 加 View + 过门禁 + 补断言」闭环，可无限做但不阻塞主线。

**为什么判定框架健康（不是自我安慰）：**
- 端到端链路（地图→仿真→3D）全部跑通，三个地图源共用单一几何层；
- 门禁能兜住回归（坐标收敛/斑马线方向/dotfile 泄漏/缓冲截断都是门禁或日志抓住的）；
- 本会话修的全是"假设/推导"类 bug（写死 +x、端点反推方向），**没有一个是
  架构类 bug**——架构（单一事实源、统一契约、View 隔离）反而防止了更大混乱。

**工作量直觉**：4 个点修各是半天到一天的独立小活；细节打磨无上限但随时可停。
先把 4 个点修做完，3D 就进入"纯加细节"阶段，不再有结构性返工。

---

## 4. 3D 细节演进路线图（核心：怎么越来越细节）

> 核心洞察：**3D 细节 = 数据源丰富度 × 渲染层表达能力 × 契约扩展**。三者的增长缺一不可：
> - 数据源有真数据（OSM 拉更多类），渲染层才画得出来；
> - 渲染层几何/材质/贴图表达力强，数据才有意义；
> - 每次新增都要同步 sceneAdapter / map.json 契约 + 门禁测试。

### P0 路面标线细节（继续走规范路线）
- 导向箭头按**车道转向**区分：直行 / 直行+左转 / 直行+右转 / 左转待转区箭头（GB 5768.3 5.13）。
- 非机动车横道线（5.18）、减速带/震荡线、渠化导流线（6.5）按路口类型渲染。
- 标线随车道数自适应（当前箭头只画直线箭头）。

### P1 交叉口细节
- 真实路缘**转弯半径**（圆弧圆角代替直切）——数据层 junctions 已有几何，渲染层可做圆角。
- 渠化岛 / 安全岛 / 导流岛（大路口）。
- 右转专用道 + 导流虚线。
- 路口范围按 incoming 车道数动态扩收。

### P2 建筑视觉（当前是纯色 OBB 盒子）
- **贴图立面**：按 footprint + height + building:levels 生成带窗格/墙面的盒子贴图
  （路线参考 osm2world）。
- **屋顶类型**：平顶/坡顶/塔楼顶。
- **LOD**：近景完整几何，远景合并为简化盒体。
- **材质**：玻璃幕墙高光、日夜光照切换。

### P3 道路设施与 POI（数据源扩展）
- 交通标志牌（限速/让行/禁停/停车让行）——OSM `traffic_sign` 拉取。
- 路灯、公交站（`highway=bus_stop`）、护栏/防撞桶（已有部分）。
- 行道树/绿化带（`landuse` / `natural=tree`）。
- 立交：桥墩/桥面阴影、匝道渐变（已有骨架）。

### P4 环境
- 天空盒、雾效、环境光/阴影（已有基础）。
- 天气/时段切换（雨雾反光、黄昏灯光）。
- 水面（黄浦江 `waterway`）与滨江绿化带。

### P5 性能与架构
- 全场景合批收敛（InstancedMesh + 纹理图集）。
- LOD 系统 + 视锥/距离裁剪。
- 大图（491 路）首帧加载优化（分块 / 静态合批 / worker 化解析）。

---

## 5. 数据源演进（让细节有真数据）

- 当前 OSM 已覆盖：道路（491 路）+ 建筑（564 栋）。
- 下一步可拉：`highway=traffic_signals`（信号灯位置）、`traffic_sign`（标志牌）、
  `highway=bus_stop`（公交站）、`highway=street_lamp`（路灯）、`natural=tree`（树木）、
  `waterway`（河流）、`landuse`（绿地/水域色块）。
- 原则：**按需拉取，映射到 map.json 的 landmarks / 新字段**，并同步
  `sceneAdapter` 契约 + 门禁测试，避免「数据有、前端不认」。

---

## 6. 验证与门禁纪律（回家照做）

1. 改 `tools/flowboard/` → `npm run vis:check:all`（不红）。
2. 新增/修改场景 → `python3 tools/build_showcase.py` 同步 `scenes.json`。
3. 新增渲染语义 → 在 `tests/vis_*.test.mjs` 加断言（参考斑马线方向断言）。
4. 改仿真（modules/adas_nodes）→ `ctest`（build/modules/adas_nodes）+ 默认场景 demo +
   涉及的地图 demo 都跑一遍。
5. 地图数据改动 → `python3 tools/osm_to_map.py --check maps/<id>`。
6. 提交前确认没有泄漏的 `scenarios/.route_*.json` 空文件（demo `--route` 残留，会挂门禁）。

---

## 7. 下一步一句话总结

先把 **P0 A* 起终点修复** 做掉让 OSM 能开一段；之后 3D 细节按
**P0 标线 → P1 路口 → P2 建筑 → P3 设施/POI → P4 环境** 的顺序推进，
每一步都「拉数据 + 加 View + 过门禁 + 补断言」闭环。
