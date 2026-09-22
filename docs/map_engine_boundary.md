# Map Engine 边界与 A* 接入分析

> 目的：在把地图从小验证地图（city_ring / city_center，~6 路）扩展到 5km×5km
> 市区网格之前，先厘清"地图引擎"的边界——哪些归外部 esmini、哪些必须由我们自研
> 的 `MapEngine` 承担，以及当前最关键的缺口（A* 路由未接入主仿真循环）。

## 1. 现状：三层结构

| 层 | 位置 | 职责 | 归属 |
|----|------|------|------|
| 数据契约 | `maps/<id>/map.json` + `routes.json`、kmap DSL、`map_compiler.py` / `extract_city_map.py` | 静态道路事实源（road/lane/junction/connection/landmark）、路线定义、校验 | 自研（已完成，较成熟） |
| 加载/转换 | `src/core/scenario_loader.c:38` `resolve_map_reference()` → 注入 `road_network.edges`；`tools/json_to_xodr.py:719` → `.xodr` | 读 map.json、转场景、转 OpenDRIVE | 自研（薄，仅跑一次） |
| 运行时引擎 | `modules/adas_nodes/flowsim/road_network.h`（esminiRMLib C API 封装）、`frenet_to_world` / `world_to_frenet`、lane 查找、elevation | 几何/车道/高程查询、拓扑（`<link>`） | **外部 esmini** |

**结论：没有统一的 `MapEngine` 类。** 几何/拓扑/高程实际由 esmini 提供，
`road_network.h` 只是薄封装；`map.json` 是离线/输入格式，运行时并不作为可查询
存储存在。各 flowsim 文件（`flowsim_node.cpp`、`npc_ai.cpp`、`collision.cpp`、
`route.cpp`、`scene_pub.cpp`）零散直接调 `frenet_to_world` / `world_to_frenet`。

## 2. esmini 拥有 vs 自研 MapEngine 拥有

### esmini 负责（不要重造）
- 道路/车道几何插值、Frenet↔World 互转、高程剖面查询。
- OpenDRIVE `<link>` 拓扑的解析与车道后继寻址（直行通过路口）。
- 3D 渲染所需的道路/路口几何（经 `json_to_xodr` 喂入）。

### 自研 `MapEngine` 应负责（当前缺失或零散）
1. **运行时统一加载**：启动时把 `map.json` 读成可查询的数据结构
   （按 id 查 road/lane、按空间范围查邻近车道），而不是仅一次性转成场景 JSON。
2. **网格拓扑**：`extract_city_map.py` 对场景提炼仍是顺序链 + `reserved` 占位。
   `maps/city_grid` 已由 `grid_map_generator.py` 写入真实直/右/左 `lanes[].successors`
   （边界缺支路则省略）。`python3 tools/grid_map_generator.py --check maps/city_grid`
   会把后继集合与该规则逐条比对。
3. **路由接入主循环**：车道级 A* 已接入主 flowsim 循环（M1+M2，见 §3）。
4. **验证策略**：大地图路线爆炸，"主线验证才放行"需改为按区域抽样。

## 3. A* 接入主循环（2026-08 已落地，M1+M2）

- 已实现：`src/algorithms/scenario_router.c` —— 车道级 A* 图（successor / 左邻 /
  右邻建图，`router_build_topology` @ :142，`router_astar` @ :257，2D 网格扩容
  `ROUTER_MAX_LANES` 4096 + `router_build_from_map_json`）。
- 已接入：`flowsim_node` 初始化阶段 `build_route_via_astar()` 建全局图 + ego 起终点
  A* → `Route::build_from_chain` → ref_path 沿 A* 车道链 → planning/control 跟随。
- 前置依赖（同批落地）：`scenario_loader.resolve_map_reference` 修 map_file/route_file
  相对路径 bug、保留 `lanes[].successors`、按 route_file/route_id 过滤 roads 到与
  xodr 同集合同编号；`ScenarioConfig.road_network_json` 暴露解析后的 road_network。
- M3-lite（单路口重规划冒烟）：`tests/test_junction_reroute.c` 从 city_grid 切出
  十字路口，`router_astar` 证明路口前改去左/右出路，路径用转向 successor，与沿同一
  大道直行不同；去掉转向 successor 后横向目标不可达。
  `ctest --test-dir build -R junction_reroute --output-on-failure`
- 待办（M3 全量）：每辆 NPC 在路口前按需重路由仍未进主循环；ego 仍是一次性起终点 + 静态链。

## 4. 与 5km×5km 大地图的关系

大地图可行性的真正前提不是几何生成（程序化网格即可），而是：
- 自研 `MapEngine` 的边界立起来（§2）；
- 网格拓扑真实连线：city_grid 的 lane successors 已落地（§2）；xodr `<junction>` /
  laneLink 由 `json_to_xodr` 承担（见 MAP_ENGINE_ROUTING §5 坑 9）；
- A* 接入主循环（§3）。

city_grid 上这条链已经能校验：生成器 successors → xodr junction/laneLink →
单路口 A* 重规划冒烟（§3）。统一 `MapEngine` 类和 NPC 车队重路由仍未做。
