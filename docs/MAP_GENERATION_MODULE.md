# Map Generation 模块（地图生成：入口 × 共享几何层）

> 本模块负责**从多种输入生成 `map.json` 道路地图**：手写 DSL（.kmap）、程序化网格、
> 真实 OSM 数据。所有入口共享同一套**地图契约**，并通过同一道 `check` 校验闸。
>
> 涉及文件：
> - `tools/extract_city_map.py` — 共享几何层（入口①②③的车道几何单一事实源）
> - `tools/map_compiler.py` — 入口①：DSL 编译器（.kmap → map.json）
> - `tools/grid_map_generator.py` — 入口②：程序化网格生成器（city_grid）
> - `tools/osm_to_map.py` — 入口③：OSM 真实数据生成器（Overpass + 手搓车道几何）
> - `tools/net2map.py` — 入口④：OSM 真实数据生成器（**SUMO netconvert 车道级几何，
>   推荐用于真实城市图**；见 §3 入口④）

---

## 1. 架构位置（交互逻辑）

```
                      输入来源
      ┌──────────────┬──────────────┬─────────────────┬─────────────────┐
      ▼              ▼              ▼                 │  net2map_        │
  map_compiler   grid_map_    osm_to_map_            │  (SUMO           │
  (DSL .kmap)    generator    (Overpass/OSM)         │   netconvert)    │
      │              │              │                 │  ⚠ 不穿共享层    │
      └──────┬───────┴──────┬───────┘                 │  （自持几何）     │
             ▼              ▼                         └────────┬─────────┘
   ┌───────────────────────────────────┐                     │
   │  extract_city_map.py（共享几何层）  │                     │
   │  right_normal / offset_lane /      │                     │
   │  _markings / build_road / check    │  ← 入口①②③ 单一事实源
   └───────────────┬───────────────────┘                     │
                   ▼                                          ▼
              map.json（同契约）──────────────────────────────────┘
                   │
                   ▼
         json_to_xodr / A* / scenario_loader / 3D
```

**核心（入口①②③）**：几何逻辑只有一份（`extract_city_map.py`），三个入口只负责
"把各自的输入变成 `edge` 定义 / 道路结构"，车道派生与校验复用同一份实现。

**入口④ net2map 是例外**：它**不穿共享几何层**——直接消费 SUMO netconvert 产出的
车道级精确形状（含路口内部连接车道 shape），车道几何由 SUMO 负责而非 `extract_city_map.py`。
理由：osm_to_map 手搓车道几何在陆家嘴实测"很多路看着很奇怪"，而 SUMO 是被工业界验证过的
车道级几何引擎。net2map 与下游仍消费同一份 `map.json` 契约，故 json_to_xodr / A* /
3D 零改动。

---

## 2. 共享几何层（`extract_city_map.py`）

| 函数 | 职责 | 被谁用 |
|------|------|--------|
| `right_normal(dx,dy)` | 行进方向右法线（车道偏移方向基准） | 三个入口 |
| `offset_lane(center, offset)` | 中心线沿右法线偏移 → 车道中心线 | map_compiler / (build_road 内部) |
| `_markings(road_id, idx, is_opp, per_side, oneway)` | 按车道位置推导标线（双黄/虚线/实线） | map_compiler / (build_road 内部) |
| `build_road(edge, rid)` | 由 `edge`（nodes/type/lanes/lane_width/oneway/speed_limit）构建完整 road（centerline + lanes） | grid / osm |
| `check(dir)` | 校验 `maps/<id>/` 的 map.json 契约（roads/lanes/junctions…） | grid / osm（--check） |

> 注意分工：`grid`/`osm` 走 `build_road`（喂 `edge` 拿整条 road）；`map_compiler` 不调
> `build_road`，而是用 DSL 声明 Road 结构 + 复用底层 `right_normal/offset_lane/_markings`
> 派生前 lane 几何（它有自己的 road 组装，但 lane 派生仍是共享的）。

---

## 3. 三个生成器入口

| 入口 | 输入 | 独有部分 | 复用共享层 |
|------|------|----------|-----------|
| **map_compiler**（DSL） | `.kmap` 声明（Road/Point/Lane/Connection） | 词法/语法解析、Road 组装、内置校验 | `right_normal`/`offset_lane`/`_markings` |
| **grid_map_generator** | 网格参数（路网布局/段数） | 网格拓扑数学（拆段、直/左/右 successors、junctions） | `build_road`、`check` |
| **osm_to_map** | Overpass 查询（lat/lon/radius） | OSM 网络拉取、WGS84→ENU 投影、路口聚类、lane successors、**建筑解析** | `build_road`、`check`、`right_normal` |
| **net2map** ⭐ | SUMO `net.xml`（netconvert 吃 OSM 产出） | 复用 SUMO 车道级几何（含路口内部 connector）、lane successors 经内部 connector 精确串接、**全路网几何邻接兜底**（`generate_connectivity`） | 独立（不穿共享层），仅复用 `check` 契约 |

**产出地图**：DSL → `city_ring`；grid → `city_grid`（1300 路/5200 车道）；
OSM → `osm_test`（52 路/73 建筑）、`osm_lujiazui`（491 路/564 建筑）；
SUMO → `osm_lujiazui_v2`（net2map 生成，车道级几何更准，建筑经 `--buildings-from` 继承）。

**注意**：建筑（`buildings[]`）仅 OSM 入口自制（osm_to_map 解析 / net2map `--buildings-from`
继承）；DSL/网格是纯道路地图。

> **⭐ 真实城市图推荐路径**：`osm_bbox_clip.py`（按 bbox 裁 OSM）→ `netconvert` 出
> `net.xml` → `net2map.py` 出 `map.json` + `routes.json` + 可选 `scenario`。车道几何由
> SUMO 保证，比 osm_to_map 手搓更稳。已知缺口：红绿灯契约待升级（SUMO tlLogic 暂未映射，
> 见 §9）；建筑需从既有 OSM 图继承。

---

## 4. 为什么不会漂移（收敛机制）

1. **车道几何单一实现**：`right_normal`/`offset_lane`/`_markings`/`build_road` 只在
   `extract_city_map.py` 一份——三个入口没有各自重写 lane 派生，不存在"画法不一致"。
2. **同一校验闸**：`check()` 是共享的，任何入口的产出都要过同一道契约检查。
3. **同一份 map.json 契约**：下游（json_to_xodr / A* / scenario_loader / 3D）只认
   map.json，不关心它来自哪个入口。

---

## 5. 接入指南（新增生成器）

新增一个生成器 = 只写"输入 → `edge` 列表"这一层，其余不重写：

1. 构造 `edge` 定义：`{name, type, speed_limit, oneway, lane_width, lanes, nodes}`。
2. 调 `build_road(edge, rid)` 生成 road（lane 几何/标线自动派生）。
3. 填 lane `successors` + `junctions[]`（路口拓扑，按入口各自的建模方式）。
4. 组装 `map_doc` + `routes` → 写 `maps/<id>/map.json`。
5. 用共享 `check(dir)` 过校验。

不必碰 `right_normal`/`offset_lane`/`_markings` 的几何实现——除非确实要改几何，
改之前先过 `test_map_compiler.py`（保证 DSL 与 city_ring 一致）。

---

## 6. 已知槽点 / 待办

- [ ] **入口层 plumbing 重复**：三个生成器各有 `main()/CLI/参数/输出` 管道，
      OSM 有网络、grid 有网格数学、DSL 有解析——可考虑抽一个公共 `write_map()` 骨架。
- [ ] **map_compiler 是"半共享"**：它不调 `build_road`，而是复用底层原语自组装——
      若 build_road 改动，需确认 DSL 产物仍一致（靠 `test_map_compiler.py` 兜底）。
- [ ] **`extract_city_map.py` 场景提取路径偏历史遗留**：与 DSL 对 city_ring 有两条
      手写通道重叠，可评估是否统一到 DSL。
- [ ] **net2map 红绿灯未映射**：SUMO `tlLogic` 暂未写入 `landmarks.traffic_lights`
      （现有契约是直路 x/y_lane 模型，塞 SUMO 信号相位进去是死数据）。待 TL 契约升级
      （路口中心 + 相位）后再接。见 §9。
- [ ] **net2map 建筑靠继承**：自身不拉建筑，需 `--buildings-from <既有OSM图 map.json>`；
      理想是复用 osm_to_map 的建筑解析或独立 OSM 建筑源。

---

## 9. net2map 已知缺口（成熟度诚实清单）

`net2map.py` 是真实城市图推荐入口，但相较"完全成熟"仍有：

1. **红绿灯（最高优先）**：`landmarks.traffic_lights` 契约待升级为「路口中心 + 相位」
   模型后，才能把 SUMO `tlLogic` 接入；当前 `net2map` 输出空 `traffic_lights`。
2. **建筑**：需从既有 OSM 图继承（`--buildings-from`），不能独立产建筑。
3. **`main` 路线链**：沿真实可驾驶拓扑（`_build_successor_graph` + 最长路径）生成，
   已通过 `check_map_connectivity.py` 车道链连通；若路网存在长回路，最长路径为启发式
   估计（环以守卫断开），非全局最优。
4. **文档/契约**：net2map 不穿 `extract_city_map.py` 共享几何层，属架构特例，改动共享层
   时 net2map 不自动回归（需单独跑 `check_map_connectivity.py --map maps/osm_lujiazui_v2`）。

---

## 7. 验证与门禁

- `python3 tools/osm_to_map.py --check maps/<id>` / `grid_map_generator` / `map_compiler`
  → 共享 `check` 校验。
- `python3 tools/map_compiler.py <src.kmap> -o maps/<id>/map.json` + `test_map_compiler.py`
  → 保证 DSL 产物与既有地图一致。
- 改共享几何层（build_road 等）后：`test_map_compiler.py` + 各入口 `--check` +
  `json_to_xodr` + 至少一个地图 demo 全过。
