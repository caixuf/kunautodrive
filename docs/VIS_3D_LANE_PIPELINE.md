# 车道级 3D 渲染管线设计方案

> 2026-08-14。回答三个问题：① 护栏/路口等**连接处**为什么一直烂、怎么框架级解决；
> ② 高德/开源成熟地图的画线衔接为什么永远完美（不是人调的）；
> ③ 可视化界面怎么从"能用"到"专业软件"。

## TL;DR

| 层 | 现状（病） | 目标（药） |
|----|-----------|-----------|
| 数据 | map.json **已是车道级**（lanes 有 centerline/width/markings/successors，junctions 有 turn 级连接），但 scene_pub 压平成 road 级 ribbon（lanes 只发**条数**） | scene_pub 透传车道几何 + 标线语义 + 路口渠化数据 |
| 拓扑 | 每个 view 各自 detectJunctions / 几何瞎猜（RoadView 一份、ConnectorView 一份、BarrierView **根本没有**） | **TopologyModel** 单一事实源：路口/arm/裁剪区间一次算清，所有 view 消费切片 |
| 渲染 | 路面 ribbon + 启发式"画"标线（ offset 漂移、穿路口、方向错） | **标线 = 车道边界数据本身**；路口 = 显式渠化多边形，不是重叠遮盖 |
| UI | emoji 图标、无设计系统、配色素 | SVG 图标库 + 设计 token（色彩/字阶/间距/圆角/阴影） |

一句话：**标线不是画在路上的，标线就是车道边界；路口不是重叠出来的，是显式数据。
数据一旦车道级，"衔接完美"自动成立——因为没有东西可以错位。**

## 1. 行业怎么做（高德 3D 截图解读 + 开源同构）

高德 3D 模式的细节（2026-08 用户供图）：

1. **路口是一整块圆角铺装多边形**，四条路在路口边界精确终止——路面与路口是"拼接"，
   不是"重叠"，所以物理上不存在标线穿路口；
2. **车道线就是车道边界数据**：lane 边界折线 + 属性（白/黄、实/虚）直接成图，
   相邻两车道**共享同一条边界**——不存在"两条线对不齐"的可能；
3. **斑马线/停止线/导向线是路口渠化数据**，随路口多边形一起生成；
4. 建筑是浅灰白盒子 + 顶部微高光 + 软阴影，整体亮色系——干净的专业感。

开源同构：

| 项目 | 做法 |
|------|------|
| **Lanelet2**（Autoware） | lane polygon + boundary linestring 带 marking 属性；渲染器是地图的纯函数 |
| **OpenDRIVE** | junction 里有 connecting-lane 显式几何（**我们 json_to_xodr 已生成 laneLink**，esmini 直接能渲染） |
| **Mapbox/Maplibre** | style-spec 图层化渲染：fill/line 图层 + 属性驱动颜色线宽 = 地图界的"设计 token" |
| **osm2world** | OSM → 3D 程序化挤出，路口平滑由数据插值 |

结论：**成熟方案里没有一个几何启发式是"渲染时现猜"的，全部是数据生产期算好。**

## 2. 我们的数据底账（已盘点，方案可行性依据）

`maps/osm_lujiazui/map.json`（其他图同构）：

- `roads[].lanes[]`：`{id, index, width, direction, centerline, markings, successors}` ——
  **车道中心线 + 宽度 + 标线语义 + 车道级后继**，全图 1161 条 lane、999 条带 successors；
- `lanes[].markings`：`{"type":"dashed_white|solid_white|double_yellow","side":"left|right"}` ——
  GB 5768 语义已在数据里；
- `junctions[]`：843 个，`{type:"fork", incoming_road, connecting_roads:[{id, turn}]}` ——
  **turn 级路口连通**（straight/left/right/uturn）；
- `json_to_xodr.py`：已生成 OpenDRIVE laneLink（2026-08-14 160e316），esmini 链路完整。

**瓶颈定位**：`scene_pub.cpp build_road_network_json` 把这一切压平——
`lanes` 只发 `drivable_lanes` **数字**，junctions 只发 `{x,y,z,radius,roads}`（turn 数据被丢），
markings/successors 完全不发。前端只能拿 road 中心线 + 车道数，靠启发式回画全部细节。
**所有"连接处烂"都源于这一个压扁点。**

## 3. 目标架构

```
map.json（车道级，已有）
   ├─→ json_to_xodr → esmini（仿真，不变）
   └─→ scene_pub 扩展（P2）── road_network v2：
         edges[].lanes[]  = {centerline, width, markings, direction}
         edges[].junctions = {polygon, arms:[{road, end, tangent}], connectors:[{from_lane, to_lane, path, turn}]}
                    │
                    ▼
        TopologyModel（js/vis/model/，P0）
          · 数据有 junctions/lanes → 直接消费
          · 缺数据 → 现有几何聚类兜底（旧场景零回归）
          · 输出：junctions[]（arm 权威切线=walk、渠化锚点）、
                  per-edge clipped ranges（标线/家具各区间）
                    │
        ┌───────────┼───────────────┬─────────────┐
        ▼           ▼               ▼             ▼
    RoadView    ConnectorView   BarrierView   Streetlight/Tree...
    lane多边形   渠化(停止线/     只在 clipped    只在 clipped
    边界成标线   斑马线/导向虚线)  ranges 内布设   ranges 内
```

三条铁律（与模块职责铁律对称）：

1. **数据即几何** —— 标线/渠化只能来自数据或 TopologyModel，view 禁止自研几何启发式；
2. **路口是显式数据** —— polygon/arms/connectors 一次算清，view 不各自 detectJunctions；
3. **单一事实源** —— 同一拓扑事实出现第二份计算 = 违规（护栏裁剪与标线裁剪必须同源）。

## 4. 分期落地

### P0 — TopologyModel + 护栏/家具收敛（纯 JS，不动数据管线，1 个 PR）

**就是回答"各种连接处"的第一步：把已修好的路口几何收敛成模型，护栏/路灯/树全部消费它。**

- 新建 `js/vis/model/TopologyModel.js`：
  - 输入 road_network，输出 junctions（centers + arms + **walk 权威切线** + 渠化锚点）
    + `clippedRanges(edgeId)`：每个 edge 的「路口外区间」列表；
  - JunctionDetect 并入其中（对外只暴露 TopologyModel，detectJunctions 变内部实现）；
- **BarrierView 接 clippedRanges**：护栏在路口边界断开 —— 修掉"弯道/路口穿插成圈"
  （当前根因：护栏从 edge 首通铺到尾，路口处多条 edge 的护栏互相穿插）；
- 护栏紧弯自交防护：内側偏移曲线在曲率半径 < 偏移量时退化自交 ——
  局部弦高检查，退化段跳过（S 弯/掉头弯不再出现打结）；
- Streetlight/Tree 同接 clippedRanges（路口里不再长树/立灯）；
- 门禁：`junction_markings.test.mjs` 扩展护栏断言（弯道/路口无穿插：护栏线段不与
  任何 junction 圆相交）。

### P1 — 路口渠化 v1（数据透传 + 前端渠化，1-2 个 PR）

- **scene_pub 透传 `junctions[].connecting_roads[].turn`**（数据就在 map.json，
  现在被丢）+ arms 端点切线（数据层 walk，与前端 walkFromJunction 同算法收敛）；
- ConnectorView 渠化：
  - 路口多边形**圆角化**（arm 间按夹角倒圆，向高德看齐的第一眼观感）；
  - **路口导向虚线**：用 connecting_roads.turn 画 lane 级转向引导线
    （左转弯导流线/直行导流线，复活 `_buildTurnConnectors` 的正确数据版）；
  - 停止线/斑马线按 turn 数据归属来车方向（而非每个 arm 全画）；
- 门禁：渠化锚点数值 invariant + osm_lujiazui golden 截图。

### P2 — 车道级渲染（scene_pub 扩展，1-2 个 PR）

- scene_pub 输出 `edges[].lanes[] = {centerline, width, markings, direction}`
  （map.json 已有，透传 + esmini 模式用 xodr lane 几何）；
- RoadView 改车道级渲染：
  - 路面 = lane 多边形合并填充（车道间无缝）；
  - **标线 = lane 边界折线按 markings 语义成图**（dashed_white→虚线、solid_white→实线、
    double_yellow→双黄）——offset 漂移/双偏移/穿路口问题连根消失；
  - 路口内不生成 lane 边界 → 标线自动在路口终止（不需要任何裁剪过滤）；
- 旧场景无 lanes 数据 → 走 TopologyModel 几何兜底（现状行为），零回归；
- 门禁：lane 边界 vs road ribbon 面积一致性 invariant；标线端点必在路口边界或
  lane 终点（不再出现半空悬线）。

### P3 — 设计系统 + UI 专业化（1-2 个 PR）

- **SVG 图标库**替 emoji：统一 24px viewBox、1.5px 描边、单色素描风
  （参考 Lucide/Feather 规范），消灭 emoji 跨平台渲染差异（🛰📊 在 Windows/macOS
  长相不同 = "幼稚感"的最大来源）；
- **设计 token**（`js/vis/theme/tokens.js` + CSS 变量）：色彩（深色底 + 科技蓝主色 +
  语义色 success/warn/danger）、字阶（11/12/13/16/20）、间距（4 的倍数）、圆角
  （6/10）、阴影分级；所有面板/按钮消费 token，禁止魔法数；
- 字体：系统等宽数字（`font-variant-numeric: tabular-nums`）用于数据面板，
  中文 PingFang/微软雅黑 + 西文 Inter 回退栈；
- 3D 场景主题：保留深色（与仪表盘一致），但**配色向高德学"分层"**——
  路面/路缘/人行道/绿化四级明度拉开（当前全挤在暗灰区所以"素"），
  建筑顶部微高光 + AO 软阴影；
- 门禁：tokens 外颜色值 grep 门禁（与 Coord.js 纯函数门禁同构）。

### 排期依赖

```
P0（TopologyModel + 护栏）  ← 独立，最先
P1（路口渠化）              ← 依赖 P0 的 arm 模型
P2（车道级渲染）            ← 依赖 P0（消费 lanes 兜底逻辑），可与 P1 并行
P3（设计系统）              ← 完全独立，任何时候可做
```

## 5. 验证体系（延续"数值先行"，禁止让文档替代测试）

| 门禁 | 抓什么 |
|------|--------|
| `junction_markings.test.mjs`（已落地） | 斑马线朝向/位置/隧道抑制，真实 three + 实例矩阵断言，OSM 全图 5032 条纹 0 偏差 |
| P0 扩展：护栏 invariant | 护栏线段 ∩ junction 圆 = ∅；弯道德化段跳过有日志 |
| P1/P2 扩展：渠化/车道边界 invariant | 标线端点 ∈ {路口边界, lane 终点}；lane 多边形面积 ≈ ribbon 面积 |
| golden 截图（puppeteer 无头一帧） | 纯观感项（圆角/配色/图标）人审 golden 图——功能性几何 100% 数值化后，只剩这些需要人眼 |
| `vis:check:all` | 全绿才可合并（现状纪律不变） |

## 6. 风险与权衡

| 风险 | 对策 |
|------|------|
| lane 多边形顶点量翻倍 | 已有 mergeGeometries 合批；标线按 type 合批（虚线已单 geo）；OSM 全图实测 draw call 预算内 |
| scene_pub 扩展打破旧前端 | road_network v2 字段**纯新增**，旧字段保留；TopologyModel 双路消费 |
| 旧场景（直道/弯道 json）无 lanes | TopologyModel 几何兜底路径 = 现状行为，scenario_regression 全量回归兜底 |
| 高德浅色主题 vs 仪表盘深色 | 3D 场景独立主题 token，不跟面板走；默认深色精致化，浅色可选 |

## 7. 与本周已落地修复的关系

2026-08-14 已完成（本方案的数据/门禁前置）：

- 斑马线/停止线 **walkFromJunction 径向出圈**（ConnectorView）——OSM 全图 5032 条纹
  朝向偏差 >15° 数量 **974 → 0**（`tests/junction_markings.test.mjs` 锁定，已入 vis:check:all）；
- 隧道/地道 arm 不再画斑马线 + 隧道路面降级暗色无标线（不再斜穿街区抢眼）；
- 护栏观感 v1：W 板双波 0.66/0.46、立柱 2m 间距加粗、镀锌钢配色（几何穿插由 P0 根治）。

这些修复的 walk/裁剪逻辑就是 TopologyModel 的雏形——P0 做的是**收敛**，不是重写。
