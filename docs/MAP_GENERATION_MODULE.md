# Map Generation 模块（地图生成：入口 × 共享几何层）

> 本模块负责**从多种输入生成 `map.json` 道路地图**：手写 DSL（.kmap）、程序化网格、
> 真实 OSM 数据。所有入口共享同一套**车道几何逻辑**（`extract_city_map.py`），
> 产出同一份 `map.json` 契约，并通过同一道 `check` 校验闸。
>
> 涉及文件：
> - `tools/extract_city_map.py` — 共享几何层（单一事实源）
> - `tools/map_compiler.py` — 入口①：DSL 编译器（.kmap → map.json）
> - `tools/grid_map_generator.py` — 入口②：程序化网格生成器（city_grid）
> - `tools/osm_to_map.py` — 入口③：OSM 真实数据生成器（osm_test / osm_lujiazui）

---

## 1. 架构位置（交互逻辑）

```
                      输入来源
      ┌──────────────┬──────────────┬─────────────────┐
      ▼              ▼              ▼                 │
  map_compiler   grid_map_    osm_to_map_             │
  (DSL .kmap)    generator    (Overpass/OSM)          │
      │              │              │                 │
      └──────┬───────┴──────┬───────┘                 │
             ▼              ▼                         │
   ┌───────────────────────────────────┐              │
   │  extract_city_map.py（共享几何层）  │              │
   │  right_normal / offset_lane /      │              │
   │  _markings / build_road / check    │  ← 单一事实源 │
   └───────────────┬───────────────────┘              │
                   ▼                                  │
              map.json（同契约）────────────────────────┘
                   │
                   ▼
         json_to_xodr / A* / scenario_loader / 3D
```

**核心：几何逻辑只有一份**（`extract_city_map.py`），三个入口只负责"把各自的输入变成
`edge` 定义 / 道路结构"，车道派生与校验复用同一份实现。

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

**产出地图**：DSL → `city_ring`；grid → `city_grid`（1300 路/5200 车道）；
OSM → `osm_test`（52 路/73 建筑）、`osm_lujiazui`（491 路/564 建筑）。

**注意**：建筑（`buildings[]`）只有 OSM 入口生成；DSL/网格是纯道路地图。

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

---

## 7. 验证与门禁

- `python3 tools/osm_to_map.py --check maps/<id>` / `grid_map_generator` / `map_compiler`
  → 共享 `check` 校验。
- `python3 tools/map_compiler.py <src.kmap> -o maps/<id>/map.json` + `test_map_compiler.py`
  → 保证 DSL 产物与既有地图一致。
- 改共享几何层（build_road 等）后：`test_map_compiler.py` + 各入口 `--check` +
  `json_to_xodr` + 至少一个地图 demo 全过。
