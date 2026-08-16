# Map Generation 模块（地图生成：DSL 单一枢纽架构）

> **本模块负责把"真实世界 / 手写 / 程序化"的地图输入，统一编译成运行时契约 `map.json`。**
>
> **核心原则：DSL 是唯一的创作入口，OSM 只是数据源，单向汇入 DSL，绝不并行直连运行时。**
>
> 本文件是**架构总纲**，取代原先"四个并行入口各生成 map.json"的旧设计（旧设计已被判定为
> 分层错误、入口混乱，见 §9 反模式）。一切新的地图生成工作都按本文件的单向管线推进。

---

## 0. 一句话心智模型

```
OSM(pbf/overpass) ──adapter──▶  .kmap(道路 DSL)   ┐
OSM(建筑)         ──adapter──▶  building DSL       ├─▶ map_compiler.py ─▶ map.json(运行时契约) ─▶ flowsim(C) + flowboard(JS)
手写/工具          ───────────▶  .kmap / building   ┘
场景脚本           ───────────▶  scenarios/*.json (按 map_id 引用地图)
```

**DSL 是枢纽（hub），不是又一条平行路径。** 任何来源（OSM、手写、程序化）想进运行时，
都必须先变成 DSL，再经唯一的 `map_compiler.py` 编译。没有"OSM → map.json"的直通车。

---

## 1. 分层（单向、不可回环）

```
┌──────────────────────────────────────────────────────────────────────────┐
│ DATA LAYER（数据层）                                                        │
│   OSM raw（pbf / overpass / net.xml）                                      │
│   角色：唯一的"事实来源"（source of truth），不是"产出目标"（sink）。          │
│   它不产生任何运行时 JSON。                                                 │
└───────────────────────────────┬──────────────────────────────────────────┘
                                 │  adapter（数据→引擎边界的转换器）
                                 ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ ADAPTER LAYER（适配层）                                                     │
│   osm2kmap     OSM路网 ──▶ .kmap 道路DSL（只产 DSL，绝不产 map.json）        │
│   netconvert   OSM ──▶ SUMO net.xml（被 osm2kmap 消费的几何引擎，非独立入口）│
│   OSM2World    OSM建筑 ──▶ building DSL + 网格引用                         │
│   性质：一次性离线工具，把脏数据翻译成干净的 DSL。改完即弃，不进运行时。        │
└───────────────────────────────┬──────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ ENGINE LAYER（引擎层）                                                      │
│   DSL 词汇：                                                                │
│     · .kmap（道路：Map/Road/Lane/Connection/Elevation/Marking）             │
│     · building DSL（建筑：原型 + 实例 + 网格引用）                           │
│     · scenarios/*.json（场景脚本，按 map_id 引用地图）                       │
│   唯一编译器：tools/map_compiler.py（.kmap/building DSL ──▶ 运行时契约）     │
│   运行时契约：map.json（schema_version / roads / connections / junctions /  │
│               landmarks / buildings …）— 下游唯一认识的东西                   │
└───────────────────────────────┬──────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ RUNTIME LAYER（运行时层）                                                   │
│   flowsim(C)  ：交通仿真、车辆运动、A* 路由、红绿灯状态机                    │
│   flowboard(JS)：Three.js 渲染、车道线/建筑/天空/相机                       │
│   二者都只认 map.json 契约，不关心它来自 OSM 还是手写。                       │
└──────────────────────────────────────────────────────────────────────────┘
```

**方向性铁律**：箭头只能从上往下。运行时不写 DSL；DSL 不写 OSM；adapter 不产 map.json。
任何"跨层抄近路"都是反模式（见 §9）。

---

## 2. 五条铁律（架构不可违反）

1. **DSL 是唯一的创作入口（single authoring entry）**
   map.json 只能由 `map_compiler.py` 从 DSL 编译得到。没有第二道会写 map.json 的程序。
   手写、程序化、OSM 都先变成 DSL，再编译。

2. **OSM 是源不是汇（OSM is source, not sink）**
   OSM 适配器的产出物是 **`.kmap` / building DSL 文件**，绝不是 map.json、routes.json
   或任何运行时 JSON。OSM 永远是"被翻译的原材料"，不是"能直接跑的产物"。

3. **单一编译器（single compiler）**
   全仓库只有 `tools/map_compiler.py` 这一个程序把 DSL 编译成运行时契约。
   不允许 `net2map.py` / `osm_to_map.py` / `grid_map_generator.py` 各自再写一份 map.json。
   若某入口过去"直连 map.json"，它的职责被拆成「adapter（产 DSL）＋ compiler（产契约）」两段。

4. **词汇即模块（vocabulary = modules）**
   道路类型（Road type）= 道路**原型模块**；连接类型（Connection：continue/fork/merge/cross）
   = **连接器模块**。渲染器用 `type → 模块注册表` 查表，而不是 `if/else + 正则` 分叉。
   新增一种路/一种连接 = 注册一个新模块，不改渲染分支。

5. **场景按 id 引用地图（scenario references map by id）**
   场景脚本不内嵌地图几何，只写 `"map_file": "maps/<id>/map.json"`（或 map_id）。
   地图与场景彻底解耦——同一张 `osm_lujiazui_v2` 可被 N 个场景复用，地图只编译一次。
   旧 `scenarios/osm_lujiazui_v2.json` 内嵌 `map_file` 字段本身不违规，但**不得再出现
   "OSM 直接喂出这张图"的并行管线**（见 §8 迁移）。

---

## 3. 适配层（ADAPTER LAYER）规范

适配器是**离线、可丢弃**的翻译器。它们的输出是 DSL 文本/文件，不是运行时契约。

### 3.1 `osm2kmap`（OSM 路网 → .kmap 道路 DSL）
- 由现有 `tools/net2map.py` **重构而来**，不是新增。
- 输入：`netconvert` 吃 OSM 产出的 `net.xml`（车道级几何由 SUMO 保证，这一步保留——
  它是 osm2kmap 内部的几何引擎，不是独立入口）。
- 输出：`.kmap` 文件（Road/Lane/Connection/Elevation/Marking 声明）。
- **关键变化**：不再直接写 `map.json` / `routes.json`。它把 SUMO 的车道级几何、
  路口内部 connector、lane successors 翻译为等价的 `.kmap` 语句。
- 连通性兜底（`generate_connectivity`）：从 net.xml 全路网邻接推导，落到 `.kmap` 的
  Connection 声明上，而不是落到运行时节点的 successor 字段里。

### 3.2 `netconvert`（OSM → SUMO net.xml）
- 纯几何引擎调用，是 `osm2kmap` 的前置工序，不是地图入口。
- 已有：`osm_bbox_clip.py`（按 bbox 裁 OSM）→ `netconvert` → `net.xml`。

### 3.3 `OSM2World` / 建筑适配器（OSM 建筑 → building DSL + 网格引用）
- 输入：OSM 建筑多边形（footprint + height + tags）。
- 输出：building DSL（建筑**原型**定义 + 按实例落位）+ 可选网格文件引用（`.obj/.gltf`）。
- 与道路对称：建筑也走"adapter 产 DSL → compiler 编译"两段式（见 §6）。

### 3.4 适配层交接清单
任何适配器 PR 必须证明：产物是 **DSL 文本**，且能被 `map_compiler.py` 原样编译出
与原先 map.json **几何一致**的契约（用 §7 门禁验证）。

---

## 4. 引擎层（ENGINE LAYER）：DSL 词汇

### 4.1 道路 DSL（`.kmap`，已存在并验证）
`tools/map_compiler.py` 已支持：
- `Map` — 元信息（schema_version / map_id / name / 范围）。
- `Road` — 一条道路原型实例（nodes / type / lanes / lane_width / oneway / speed_limit）。
- `Lane` — 单车道的显式声明（覆盖派生的特殊情况）。
- `Connection` — 道路间连接（continue / fork / merge / cross 四类连接器模块）。
- `Elevation` — 纵坡 / 高程。
- `Marking` — 标线覆盖（双黄 / 虚线 / 实线）。

**编译器复用** `extract_city_map.py` 的几何原语（`right_normal` / `offset_lane` /
`_markings`）派生车道几何，保证 DSL 道路与历史 `city_ring` 几何一致。

### 4.2 道路模块化词汇（Road type = 原型模块）
现实路网太复杂，抽象成有限的**道路原型**，每个原型是一份"模块配方"：

| Road type | 原型语义 | 派生内容（模块配方） |
|-----------|----------|----------------------|
| `highway` | 城市快速路 | 多车道、中央分隔、高限速、匝道接入点 |
| `urban`   | 城市主干/次干 | 双向车道、路口衔接、人行/机非隔离 |
| `residential` | 小区/支路 | 窄车道、低限速、路侧停车模块 |
| `ramp_curve` | 匝道 | 曲率约束、合/分流连接 |
| `intersection` | 路口 | 由 Connection 组合，无独立车道 |

> 渲染器持有一张 `ROAD_TYPE → 渲染模块` 注册表；新增原型 = 注册一条配方，不动渲染分支。

### 4.3 连接模块化词汇（Connection type = 连接器模块）
道路之间的拓扑衔接抽象为**连接器模块**，每个连接器是"把 A 路尾接到 B 路头"的标准件：

| Connection type | 连接器语义 |
|-----------------|-----------|
| `continue` | 同路延伸（直行续接，最常见） |
| `fork` | 一分为二（主路分匝道 / 主辅分离） |
| `merge` | 二合一（匝道汇入主路） |
| `cross` | 路口交叉（与 intersection 协同） |

> 旧 `net2map` 的 `:` 内部 connector（SUMO 路口内部车道）在此被显式归类为 `continue` /
> `merge` / `cross` 等连接器，不再以 `:` 命名特例存在于 road 列表里（见 §8）。

### 4.4 场景 DSL（`scenarios/*.json`）
- `ego` / `pass_criteria` / `route` / `scenarios` 等脚本字段不变。
- 只引用 `map_id` / `map_file`，**不内嵌地图几何**。
- 同一 map_id 可被多个场景脚本共享。

---

## 5. 运行时契约 `map.json`（枢纽的下游接口，保持不变）

`map_compiler.py` 编译出的契约结构（下游 flowsim / flowboard 唯一认识的东西）：

```
{
  schema_version, map_id, name,
  roads[]      // 每条含 centerline(= 最左车道 LEFT 边) + lanes[](各自 SUMO 中心线) + successors
  connections[]// continue/fork/merge/cross 显式连接
  junctions[]  // 路口拓扑
  landmarks[]  // traffic_lights（由 SUMO tlLogic 映射，见 §8）
  buildings[]  // 建筑（由 building DSL 编译，见 §6）
}
```

**关键几何契约（flowboard 渲染必读，已在 RoadView.js 修正）**：
- `road.centerline` = **最左车道 LEFT 边**（由最左车道 shape 左偏 `laneWidth/2` 得到）。
- 每条 `lane.centerline` = 该车道**自身**的 SUMO 中心线。
- 一条 n 车道路的真正车道组中心，相对 spine 的横向偏移为 `+n*laneWidth/2`。
- `laneGroupEnvelope()` 负责逐站测量车道相对 spine 的横向展幅，返回 `{center, halfW}`
  把 ribbon 平移到车道组中心（旧版用 `tz=p[2]` 误读 ENU-up 导致丢失 north 分量，
  已改为 `tz=-(p[1])` 垂直投影修复）。

下游 `json_to_xodr` / A* / scenario_loader / Three.js 渲染**只认这份契约**，
不关心契约来自 `.kmap` 还是 OSM 经 adapter 转出的 `.kmap`——这正是枢纽的价值。

---

## 6. 建筑域对称性（Buildings are symmetric with roads）

建筑不再"寄生"在 OSM 入口里（旧 `osm_to_map` 手搓、`net2map --buildings-from` 继承），
而是走**与道路同构**的两段式：

```
OSM 建筑 ──OSM2World(adapter)──▶ building DSL ──bld_compiler──▶ map.json.buildings[]
手写/工具 ────────────────────▶ building DSL
```

- **建筑原型（prototype）**：按用途/形态分类的模块（住宅塔楼、商业裙楼、厂房、地标…），
  每个原型是一份"网格 + 尺度约束"配方，落位时实例化。
- **实例（instance）**：在某 ENU 坐标、按原型 + 实际高度/朝向实例化。
- **网格引用**：重资产（精细 mesh）以文件引用方式挂到原型上，编译器只记录引用与变换，
  渲染器按需加载——与道路"类型→渲染模块"注册表同构。
- **编译合并**：`map_compiler.py` 同时消费道路 `.kmap` 与 building DSL，输出同一份
  `map.json`（roads + buildings 同契约）。道路上建筑对称地"属于引擎层"。

> 旧缺口（建筑只能从既有 OSM 图继承、不能独立产）在此被结构性消除：建筑有了自己的
> adapter + DSL + 编译通道，与道路平起平坐。

---

## 7. 验证与门禁（收敛机制不变，且更严格）

1. **几何单一实现**：车道派生仍在 `extract_city_map.py` 一份，所有 DSL 道路复用。
2. **单一校验闸**：`map_compiler.py` 出口过 `check()` 契约校验；adapter 产出的 `.kmap`
   必须能编译通过且几何与原先 map.json 一致。
3. **门禁命令**：
   - `python3 tools/map_compiler.py <src.kmap> -o maps/<id>/map.json` + `test_map_compiler.py`
     → 保证 DSL 产物与既有地图一致。
   - `python3 tools/osm_to_map.py --check maps/<id>`（仅作 adapter 产出对照，不再独立写图）。
   - `python3 tools/check_map_connectivity.py --map maps/osm_lujiazui_v2` → 车道链连通。
   - 改共享几何层后：`test_map_compiler.py` + 各 adapter 重产出 `.kmap` 重编译 + 至少一个
     地图 demo 全过。
4. **反回归**：任何"绕过 compiler 直写 map.json"的提交在 review 阶段即驳回（见 §9）。

---

## 8. 迁移路线（从旧 4 入口 → 新枢纽）

| 旧组件 | 旧角色（反模式） | 新角色 |
|--------|------------------|--------|
| `tools/net2map.py` | 入口④：OSM+SUMO → **直写 map.json** | 重构为 `osm2kmap`：OSM+SUMO → **`.kmap`** |
| `tools/osm_to_map.py` | 入口③：Overpass → **直写 map.json+建筑** | 降级为 OSM 适配参考 / 建筑 adapter 前身；不再写图 |
| `tools/grid_map_generator.py` | 入口②：网格 → **直写 map.json** | 改为产 `.kmap`（网格拓扑 → Road/Connection 声明） |
| `tools/map_compiler.py` | 入口①：`.kmap` → map.json | **升格为唯一编译器**，吞掉 ②③④ 的"写图"职责 |
| `tools/extract_city_map.py` | 共享几何层（①②③ 用，④ 不穿） | 保留为 compiler 内部的几何原语库（所有 DSL 道路复用） |
| `:` 内部 connector | net2map 路列表里的特例命名 | 显式归类为 `continue/merge/cross` 连接器 |
| `scenarios/osm_lujiazui_v2.json` 内嵌 map_file | 场景直接挂某张 OSM 图 | 改为 `reference map_id`，图本身改由 `.kmap` 编译产出 |

**分阶段（最小首切）**：
1. **道路先通枢纽**：`osm2kmap`（net2map 重构）→ `map_compiler` 产出 `osm_lujiazui_v2`，
   几何与现状逐车道比对一致 → 验证枢纽成立。
2. **建筑对称跟进**：OSM2World → building DSL → `bld_compiler` 并入 `map_compiler`，
   `osm_lujiazui_v2` 的建筑由 DSL 通道产出（与道路同契约）。
3. **清理并行入口**：`osm_to_map.py` / `grid_map_generator.py` 改为产 DSL；删除一切
   直写 map.json 的残留路径。

---

## 9. 反模式清单（明确禁止，review 红线）

- ❌ **OSM 直连运行时**：`net2map.py` / `osm_to_map.py` 直接 `json.dump(map.json)`。
  → 必须改为先产 `.kmap`，再交 `map_compiler.py`。
- ❌ **多编译器并存**：除 `map_compiler.py` 外任何程序写 map.json 契约。
- ❌ **渲染 `if/else + 正则` 分叉**：`edge.type` 用正则匹配选渲染分支（旧
  `tutorials/16_flowsim_scenario_design.md` 警告过的反模式）。→ 改用 `type → 模块注册表`。
- ❌ **`:` 命名特例**：路口内部 connector 以 `:` 混在 road 列表。→ 显式 Connection 类型。
- ❌ **场景内嵌地图几何**：场景脚本携带道路点。→ 只引用 `map_id`。
- ❌ **建筑寄生 OSM 入口**：建筑只能从 `--buildings-from` 继承。→ 独立 building DSL 通道。

---

## 10. 已知缺口（诚实清单，随枢纽推进而收敛）

1. **红绿灯契约**：SUMO `tlLogic` → `landmarks.traffic_lights` 的映射逻辑从旧 `net2map`
   迁入 `osm2kmap`（产出 `.kmap` 的 Elevation/Marking 同级声明），再由 compiler 落到契约。
   限制（runtime 单周期相位机、`TL_CACHE_MAX=16`）属运行时层，不在本模块范围。
2. **建筑 mesh 资产**：精细建筑网格的来源/格式（`.obj` vs `.gltf`、LOD）待 building DSL
   定型后明确；首切可用 OSM2World 默认产出。
3. **长回路路网**：沿真实拓扑的最长路径为启发式（环以守卫断开），非全局最优——属 route
   生成策略，与"是否经 DSL 枢纽"无关，枢纽化后不变。
4. **adapter 回归**：osm2kmap 重构后需单独跑 `check_map_connectivity.py` 验证车道链连通，
   因其不再穿 `extract_city_map.py` 共享层（几何由 SUMO 保，但连通性声明要翻译进 `.kmap`）。

---

> **本文件取代旧版"4 并行入口"设计。** 任何新的地图生成需求，先问"它怎么变成 DSL"，
> 而不是"它怎么直接写 map.json"。DSL 是唯一枢纽。
