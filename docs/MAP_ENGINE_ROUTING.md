# Map Engine 与 A* 路由 — 现状与经验沉淀

> 本文是地图引擎方向的**可复用参考**（字段契约、工具命令链、运行时接口、经验坑），
> 与 [map_engine_boundary.md](map_engine_boundary.md)（边界与缺口分析）配套。
> 改动地图/路由相关代码前先读本文件，避免重复踩坑。

## 1. 三层结构（谁做什么）

代码里**没有统一的 `MapEngine` 类**，职责分三层：

| 层 | 位置 | 职责 | 归属 |
|----|------|------|------|
| 数据契约 | `maps/<id>/map.json` + `routes.json`、`.kmap` DSL、`tools/map_compiler.py`/`extract_city_map.py`/`grid_map_generator.py` | 静态道路事实源、路线定义、校验 | 自研（较成熟） |
| 加载/转换 | `src/core/scenario_loader.c:38` `resolve_map_reference()`、`tools/json_to_xodr.py:716` `convert()` | 读 map.json 注入 `road_network.edges`、转 OpenDRIVE | 自研（薄，仅跑一次） |
| 运行时引擎 | `modules/adas_nodes/flowsim/road_network.{h,cpp}`（esminiRMLib 封装）+ `route.{h,cpp}` | 几何/车道/高程查询、Frenet↔World、中央有序 route | **外部 esmini** |

> **边界铁律**：esmini 负责几何插值、Frenet↔World 互转、高程、OpenDRIVE `<link>`
> 拓扑解析、3D 渲染几何——**不要重造**。自研 `MapEngine` 只做四件事：运行时统一加载
> 可查询存储、网格拓扑转向后继、路由接入主循环、验证策略（按区域抽样）。

## 2. 数据契约字段级参考

### map.json（静态道路事实源）
- 顶层：`schema_version`、`map_id`、`name`、`roads[]`、`connections[]`、`junctions[]`、`landmarks{traffic_lights,stop_lines,construction_zones}`。
- 每条 `road`：`id`、`type`(urban/ramp_curve/...)、`speed_limit`、`oneway`、`centerline[]`（节点数组，**不采样**，避免 5m 采样膨胀成数百 KB，见 `extract_city_map.py:25-28`）、`lanes[]`、可选 `elevation_profile`。
- 每条 `lane`：`id`、`index`、`width`、`direction`(+1/-1)、`centerline[]`（由中心线沿右法线偏移生成）、`markings[]`、`successors[]`。
- **命名约定**：`id` = `<road>.lane.<n>`；正向 lane index 1..N，对向 lane index 101..100+N（`map_compiler.py:84`、`extract_city_map.py:107`）。`map.json` **不得含 `source_scenario`**（必须脱离旧场景独立，`check` 会拦截）。
- 好样例：`maps/city_center/map.json`（含下穿隧道 `elevation_profile` 三折线 s/z）。大地图：`maps/city_grid/map.json`（129KB，5km×5km 网格，但 lane `successors` 目前全空，见 §5 坑 1）。

### routes.json（路线定义）
- 顶层：`map_id`、`routes[]`、`reserved_turns[]`。每条 route：`id`、`name`、`kind`(main/urban/underpass/spine/on_ramp/...)、`road_chain[]`（**顺序道路链 = 当前"预设线性行驶"唯一依据**）、`lane_direction`、`validated`/`draft`（主线不带 draft）。

### .kmap DSL（声明式地图语言，`map_compiler.py` 编译为 map.json）
- 结构：`Map{id;name; Road X{type;lanes;laneWidth;speedLimit;oneWay;Point{x;y;z};Elevation{s;z};Lane{...}} Connection{from;to;type}}`。
- lane 两种声明：简洁式 `lanes:N`+`oneWay`（编译器自动派生全部 lane）或显式 `Lane{}` 块覆盖（index/direction/successors/Markings），可混用（`map_compiler.py:29-32`）。
- 样例：`maps/examples/city_ring_minimal.kmap`、`city_ring_full.kmap`、`maps/city_center/city_center.kmap`。

## 3. 工具链命令（可复现）

```
# kmap → map.json（校验由 map_compiler 内置）
python3 tools/map_compiler.py <src.kmap> -o maps/<id>/map.json

# 从旧场景 JSON 提炼独立地图 + 校验（--check 只校验不生成）
python3 tools/extract_city_map.py <in.json> -o maps/<id>/
python3 tools/extract_city_map.py --check maps/<id>

# map.json + routes.json → OpenDRIVE .xodr（供 esmini）
python3 tools/json_to_xodr.py <scenario.json>   # 内部按 road_chain 选路

# 转换正确性三件套（改地图/路由后必跑）
python3 tools/xodr_compat_check.py --runtime-test test_road_network
./build/bin/test_road_network
# 大地图 / 网格生成
python3 tools/grid_map_generator.py ...
```

## 4. 运行时接口契约（road_network.h）

- **封装 esminiRMLib C API**（`RM_Init`/`RM_SetLanePosition`/`RM_GetPositionData`），不是 `roadmanager::OpenDrive` C++ 类。
- **进程全局单例**：`RM_Init` 替换上次路网 → `FlowRoadNetwork` 实例必须单线程使用（`flowsim_node.cpp` 主循环独占）。Position handle 是 per-instance RAII，`load()` 后创建。
- `frenet_to_world(road,lane,s,offset,out)`：`RM_SetLanePosition(handle, roadId, laneId, laneOffset, s, align)`——**注意 laneOffset 在 s 之前**；`out.z` 即高架 elevation。
- `world_to_frenet(x,y,out)`：`RM_SetWorldXYHPosition` 反算。
- **OpenDRIVE 坐标系约定**：lane 0=参考线、正=左、负=右；场景 JSON 的 y<0=左/y>0=右由 json_to_xodr 统一映射；双向 road 的 lanes 是"双向合计"、`n_per_side = lane_count//2`（旧实现误当每侧车道数导致 8 条 drivable lane 的 bug，见 `json_to_xodr.py:654-658`）。

## 5. 经验坑速查（改地图/路由必读）

1. **大地图路线爆炸**：原 `city_grid/routes.json` 的 main 把 52 条大道顺序串成一条链；`extract_city_map.py` 的 successors 只是 edge 顺序拼接（:133-136）。大地图必须靠真实转向拓扑 + A* 图搜索，而非枚举长 `road_chain`。已把 main 改为 `ns_avenue_00` 全程段链。
2. **lane successors 为空 = 无拓扑（已修复）**：原 city_grid 所有 lane `successors:[]`。已由 `grid_map_generator.py` 拆段建模：每条大道在交叉口拆成独立 road 段（`ns_avenue_00_seg_00`），每段 lane 填直/左/右 `successors`（指向 `目标段.lane.<idx>`），顶层 `junctions[]` 用 fork 表达每个进入方向的直/左/右。`--check` 通过（1300 段 / 2600 junctions / ~1.5 万 successors 引用）。
3. **交叉口转向是占位**：`extract_city_map.py` 对 cross_roads 只产 `reserved` junction，无真实连接；真正的左/右/直转向要依赖 json_to_xodr 的 `<junction>` 增强（属 MapEngine 应自研、当前缺失）。
4. **A* 当前是 toy 图**：`navigation_node.c` 建的是单 road(road_id=0) 的 lane 换道图（:394-401）；`scenario_router.c` `heuristic()` 只用端点 x 中点一维欧氏（:84-99）；容量上限 256 lane/512 edge/64 path——直接用于整张 city_grid 会超限，需扩容或分层。
5. **A* 未接入主 flowsim 循环**：`router_astar`/`router_build_topology` 只被 `navigation_node.c` 使用；`flowsim_node.cpp` 只调 `Route`（沿预设 `road_chain` 端点连续性拼接），无任何 scenario_router 引用。
6. **行进方向唯一事实源 = flowsim `ref_path.reverse`**：navigation/behavior/control 全消费它，杜绝基于 dx/heading 猜测在掉头/绕圈时翻转的正反馈环（`navigation_node.c:248-275`）。
7. **esmini 单线程 + 参数顺序**：`RM_Init` 全局单例；`RM_SetLanePosition` laneOffset 在 s 前；双向 road 每侧车道数 = `lane_count//2`。
8. **验证链路**：`xodr_compat_check.py --runtime-test` + `test_road_network` + `extract_city_map.py --check` 三件套；`sim_digest` 提供静态/动态 invariant + ASCII 俯视，是"改地图/路由后不破坏几何"的回归抓手。
9. **json_to_xodr 网格 junction（已部分落地）**：`load_scenario` 已做**字符串 road id → 数字索引映射**（map.json 用字符串段 id，json_to_xodr 内部用数字 edge id），city_grid 拆段拓扑可转出结构化 `<junction>`（每进入方向一个 fork，含直/左/右 connection），`xodr_compat_check` 静态检查 PASS（1300 roads / 2600 junctions / 5200 lanes，0.24s）。**待后续**：esmini 运行时校验（`test_road_network` 需编译）+ `laneLink` 车道级转向映射 + 段两端连 junction 的 OpenDRIVE 归属语义。

## 6. A* 接入主循环（待办设计要点）

1. 主循环初始化阶段（flowsim_node 建图后）调 `router_build_topology`，用 map.json lane 后继 + json_to_xodr 路口连接建全局图。
2. 每辆 ego/NPC 在需要时（起点→终点、或路口前）调 `router_astar` 求车道路径。
3. 把 A* 输出的车道序列转为沿车道中心线的轨迹目标，喂给现有纵向/横向控制。
4. 注意：`flowsim_node.cpp` / `collision.cpp` 曾有未提交 WIP，接入前先 rebase 干净。
