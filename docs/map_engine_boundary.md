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
2. **网格拓扑**：交叉口转向后继（左/右/直）的生成与维护——当前
   `extract_city_map.py` 只做顺序链 + `reserved` 占位，无真实转向连接。
3. **路由接入主循环**：车道级 A* 当前已实现但**未接入**主仿真。
4. **验证策略**：大地图路线爆炸，"主线验证才放行"需改为按区域抽样。

## 3. 关键缺口：A* 路由未在主线

- 已实现：`src/algorithms/scenario_router.c` —— 车道级 A* 图（successor / 左邻 /
  右邻建图，`router_build_topology` @ :142，`router_astar` @ :257）。
- 现状：仅被 `modules/adas_nodes/navigation_node.c`（:20/:57/:399）使用，
  **主 flowsim 仿真循环未调用**。即"车辆在地图里怎么找路"在主循环里是缺位的，
  目前车辆沿预设 `road_chain` 线性行驶。

### 接入主循环的最小改动分析（待办，不在此文档落地）
1. 在主循环初始化阶段（flowsim_node 建图后）调用 `router_build_topology`，
   用 `map.json` 的 lane 后继 + `json_to_xodr` 产出的路口连接构建全局图。
2. 为每辆 ego/NPC 在需要（起点→终点、或路口前）调用 `router_astar` 求车道路径。
3. 把 A* 输出的车道序列转为沿车道中心线的轨迹目标，喂给现有纵向/横向控制。
4. 注意：当前 `flowsim_node.cpp` / `collision.cpp` 有未提交 WIP，接入前应先 rebase
   干净，避免冲突。

## 4. 与 5km×5km 大地图的关系

大地图可行性的真正前提不是几何生成（程序化网格即可），而是：
- 自研 `MapEngine` 的边界立起来（§2）；
- 网格拓扑真实连线（交由 `json_to_xodr` 的路口 `<junction>` 增强承担）；
- A* 接入主循环（§3）。

三者齐备后，把"网格生成器产出的 map.json"→"xodr 拓扑增强"→"A* 路由接入"
串成一条可验证的主线。
