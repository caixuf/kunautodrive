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

1. **A* 已接入主 flowsim 循环（2026-08，M1+M2）**：`flowsim_node.build_route_via_astar()` 初始化建图（`router_build_from_map_json` 从 `scenario->road_network_json` 读 `lanes[].successors`）→ ego 起终点 A* → 车道链去重 road → `Route::build_from_chain` → ref_path 沿 A* 车道链 → planning/control 跟随。失败回退旧 `Route::build()`（无 map 场景必走回退）。前置：`scenario_loader` 修 map_file/route_file 相对路径 bug + 保留 lanes[] + 按 route_file/route_id 过滤 roads 到与 xodr 同集合同编号。
2. **大地图路线爆炸**：原 `city_grid/routes.json` 把 52 条大道顺序串成一条链；大地图要真实转向拓扑 + A*，别枚举长 `road_chain`。已把 main 改为 `ns_avenue_00` 全程段链。
3. **lane successors 为空 = 无拓扑（已修复）**：`grid_map_generator.py` 已拆段建模——每条大道在交叉口拆成独立 road 段（`ns_avenue_00_seg_00`），每段 lane 填直/左/右 successors，顶层 `junctions[]` 用 fork 表达每个进入方向。`--check` 通过（1300 段 / 2600 junctions）。
4. **交叉口转向是占位**：`extract_city_map.py` 只产 `reserved` junction，真实左/右/直靠 json_to_xodr 的 `<junction>` 增强。
5. **json_to_xodr 网格 junction（已部分落地）**：`load_scenario` 已做字符串 road id → 数字索引映射，city_grid 可转出结构化 `<junction>`（每进入方向 fork 含直/左/右 connection），`xodr_compat_check` 静态检查 PASS。待后续：esmini 运行时校验（需编译 test_road_network）+ laneLink + 段两端连 junction 的归属语义。
6. **A* 容量已扩容**：`ROUTER_MAX_LANES` 4096 / 2D 启发式 / `router_add_lane_xy`；`router_build_from_map_json` 直接吃 road_network JSON（字符串 lane id → int，road_id = edge 数字 id 与 xodr 对齐）。

## A* 接入主循环（已落地）

主循环建图后 `router_build_from_map_json` 建全局图 → ego 起终点 `router_astar` → 车道链去重 road → `Route::build_from_chain` → ref_path 沿 A* 车道链喂 planning/control。M3 待办：每辆 NPC / 路口前按需重路由（当前 ego 一次性路由 + 静态链）。
