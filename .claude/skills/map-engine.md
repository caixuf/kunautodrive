---
name: map-engine
description: 地图引擎 + A* 路由经验：map.json/routes.json/kmap 字段契约、工具命令链（map_compiler/extract_city_map/json_to_xodr/xodr_compat_check）、esmini road_network 接口约束、A* 未接入主循环的现状与改造要点。改动地图/路由相关代码时使用。
---

# Map Engine & A* 路由

沉淀自 map 系列 commit。改动地图/路由前先看 [MAP_ENGINE_ROUTING.md](../../docs/MAP_ENGINE_ROUTING.md)（字段级参考 + 命令链）。

## 什么时候用

- 新增/修改 `maps/<id>/map.json`、`routes.json`、`.kmap`、`scenario_loader.c`、`json_to_xodr.py`、`road_network.{h,cpp}`、`scenario_router.c`
- 做大地图（5km×5km 网格）、交叉口转向、A* 路由接入

## 三层职责（别重造 esmini）

1. **数据契约**：`maps/<id>/map.json`+`routes.json`、`.kmap` DSL、`map_compiler.py`/`extract_city_map.py`/`grid_map_generator.py` —— 自研。
2. **加载/转换**：`scenario_loader.c:38` `resolve_map_reference()`（注入 `road_network.edges`）、`json_to_xodr.py:716`（转 OpenDRIVE）—— 自研，薄。
3. **运行时引擎**：`flowsim/road_network.{h,cpp}` 封装 esminiRMLib（Frenet↔World/几何/高程）—— **esmini 拥有，不要重造**。

## 必守约定

- lane id = `<road>.lane.<n>`；正向 index 1..N，对向 101..100+N。
- `map.json` **不得含 `source_scenario`**（必须独立，check 拦截）。
- 双向 road 的 lanes 是"双向合计"，`n_per_side = lane_count//2`。
- esmini `RM_Init` 是**进程全局单例**，`FlowRoadNetwork` 必须单线程（主循环独占）。
- `RM_SetLanePosition(handle, roadId, laneId, laneOffset, s, align)` —— **laneOffset 在 s 前**。
- 行进方向唯一事实源 = flowsim `ref_path.reverse`，别基于 dx/heading 猜测。

## 改完必跑（验证三件套）

```
python3 tools/extract_city_map.py --check maps/<id>
python3 tools/xodr_compat_check.py --runtime-test test_road_network
./build/bin/test_road_network
# 回归抓手：sim_digest 的静态/动态 invariant + ASCII 俯视
```

## 关键现状 / 坑

1. **A* 未接入主 flowsim 循环**：`scenario_router.c` 只被 `navigation_node.c` 用，且那里建的是单 road 换道 toy 图；`flowsim_node.cpp` 只走 `Route`（预设 `road_chain` 线性拼接）。
2. **大地图路线爆炸**：原 `city_grid/routes.json` 把 52 条大道顺序串成一条链；大地图要真实转向拓扑 + A*，别枚举长 `road_chain`。已把 main 改为 `ns_avenue_00` 全程段链。
3. **lane successors 为空 = 无拓扑（已修复）**：`grid_map_generator.py` 已拆段建模——每条大道在交叉口拆成独立 road 段（`ns_avenue_00_seg_00`），每段 lane 填直/左/右 successors，顶层 `junctions[]` 用 fork 表达每个进入方向。`--check` 通过（1300 段 / 2600 junctions）。
4. **交叉口转向是占位**：`extract_city_map.py` 只产 `reserved` junction，真实左/右/直靠 json_to_xodr 的 `<junction>` 增强。
5. **json_to_xodr 网格 junction（已部分落地）**：`load_scenario` 已做字符串 road id → 数字索引映射，city_grid 可转出结构化 `<junction>`（每进入方向 fork 含直/左/右 connection），`xodr_compat_check` 静态检查 PASS。待后续：esmini 运行时校验（需编译 test_road_network）+ laneLink + 段两端连 junction 的归属语义。
6. **A* 容量上限 256 lane/512 edge/64 path**，直接用于整张 city_grid 会超限，需扩容或分层。

## A* 接入主循环（设计要点）

主循环建图后 `router_build_topology` 建全局图 → 每辆 ego/NPC 需要时 `router_astar` 求车道路径 → 转成沿车道中心线的轨迹目标喂纵向/横向控制。接入前先 rebase 干净（flowsim_node/collision 曾有未提交 WIP）。
