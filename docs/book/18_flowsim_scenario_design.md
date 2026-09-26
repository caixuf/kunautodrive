# 第 20 章：FlowSim —— 给算法造一个能反复练车的世界

> **本章导读**：
> 算法上车之前，得先在仿真里跑够里程。可造出这样一个世界并不轻松：它既要摆得出多车道、交叉路口和匝道汇入，还得让路上的 NPC 看起来像真的在开车。
>
> 本章讲 FlowSim 怎么用一份 JSON 描述场景、路网怎么从直线拼成完整路线、车辆动力学有哪三套模型（以及**默认用的是哪一套**），以及仿真时间和墙上时间是怎么分开的。
>
> 先破除旧稿里几个会让人踩坑的误解——这些不是措辞问题，是照着写代码会出错的问题：
>
> 1. **默认不是动力学模型。** `physics_model` 默认 `"kinematic"`（`flowsim_node.cpp:137`）。动态模型和 Pacejka 都要显式开。
> 2. **NPC 永远用运动学模型。** `npc_ai.cpp:775` 无条件调 `step_bicycle(npc, dt, throttle, brake, 0.0)`——`steer` 传 0，它只被当作**纵向速度积分器**。动态模型是自车专属。
> 3. **dt 是 1/60 秒，不是 0.05。** `physics.cpp:3` 的注释写着「dt=0.05s（20Hz）」——**过时**。真实值在 `flowsim_time.h`，60 Hz。
> 4. **仿真不能快于实时。** 没有子步进、没有时间缩放，帧超时就是 0 睡眠硬追。
> 5. **红绿灯有 4 个相位，不是 3 个。** 多一个 `FlashingGreen`（闪烁绿）。
> 6. **NPC 默认不变道。** MOBIL 完整实现了但 `enable_mobil` 默认 `false`。
> 7. **`esmini_stub.cpp` 是全空桩，不是转换桥。** 旧稿说它解析 OpenDRIVE——完全说反了。
> 8. **`edge.type` 不控制摩擦系数。** 旧稿那张表里的 μ 值在代码里不存在。

---

## 1. 三个时钟：逻辑时间、墙上时间、实时时间

这是理解 FlowSim 的第一道门槛，也是最容易写错代码的地方。

[`modules/adas_nodes/flowsim/flowsim_time.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/flowsim_time.h) 全文只有 7 行，但它自称「时间步唯一事实源」：

```c
#pragma once
/* flowsim 时间步唯一事实源。flowsim_node.cpp 与 scene_events.cpp 必须都包含本文件，
 * 禁止各自 #define（曾因两份定义不一致导致红绿灯编排时长差 3 倍）。 */
#define FLOWSIM_FREQUENCY_HZ   60.0
#define FLOWSIM_DT_SEC         (1.0 / FLOWSIM_FREQUENCY_HZ)   /* ~0.0167s */
#define FLOWSIM_DT_US          ((uint64_t)(FLOWSIM_DT_SEC * 1e6))  /* 16666 */
```

**这条注释本身就是一段事故记录。** 曾经 `flowsim_node.cpp` 和 `scene_events.cpp` 各自 `#define` 了一个时间步，两份不一致，导致红绿灯编排时长差 3 倍。这类 bug 极难查——现象是「红绿灯节奏不对」，根因却在两个文件对 `dt` 的不同理解。

### 1.1 三个时钟函数

| 函数 | 仿真模式下返回 | 用途 |
|---|---|---|
| `clock_now_us()` | **逻辑时间** `g_sim_time` | 物理 `dt`、红绿灯相位、编排、`sim/tick` 时间戳 |
| `clock_now_monotonic_wall_us()` | 真实 `CLOCK_MONOTONIC`（**永不被仿真影响**） | 帧耗时测量、sleep |
| `clock_now_realtime_us()` | `CLOCK_REALTIME` | 绝对时间戳 |

仿真模式在启动时打开（`flowsim_node.cpp:3232-3234`）：

```cpp
clock_set_sim_mode(true);
clock_set_sim_time(0);
clock_set_step_us(FLOWSIM_DT_US);
```

逻辑时间在物理阶段的**最末尾**推进一格（`:2586`）：

```cpp
clock_advance_us(FLOWSIM_DT_US);
```

**放在最末尾而不是开头**，是为了让本帧内所有读到 `clock_now_us()` 的代码（物理、碰撞、红灯、NPC AI）看到同一个时间点。

### 1.2 为什么要用两个时钟测帧时间

`flowsim_node.cpp:2627-2640` 的自适应 sleep：

```cpp
uint64_t t_frame_us = clock_now_monotonic_wall_us() - t_start;
uint64_t sleep_us_val = FLOWSIM_DT_US;
if (t_frame_us < sleep_us_val) {
    sleep_us_val = FLOWSIM_DT_US - t_frame_us;
} else {
    sleep_us_val = 0;  /* 帧超时：不休眠，下一帧立即开始追 */
}
...
co_await sleep_us(sleep_us_val);
```

这里**必须**用 `clock_now_monotonic_wall_us()` 而不是 `clock_now_us()`。代码注释（`:1765-1771`）记录了踩过的坑：用逻辑时间测帧耗时，`t_frame_us` 恒等于步长，sleep 永远是 0，节点会空转到 3000+ Hz 把消息总线打爆。

> [!IMPORTANT]
> **这直接回答了一个常见误解：FlowSim 不能快于实时。**
>
> - 没有子步进（帧超时不会补跑多个物理步）
> - 没有时间缩放参数
> - 没有无头批量模式
>
> 想跑 10 小时仿真，就得等 10 小时。这一点在评估 FlowSim 能否替代真实里程时非常关键。

### 1.3 帧耗时每 600 周期（约 10 秒）打一次

`flowsim_node.cpp:2634-2639` 打 `[PERF] cycle=... frame_time=... sleep=...`。排障时先看这行——它能告诉你系统是「物理太慢」还是「被别的东西挡住了」。

---

## 2. 场景描述：一份 JSON 能写什么

### 2.1 加载器

[`include/scenario_loader.h`](file:///home/caixuf/code/FlowEngine/include/scenario_loader.h)（396 行）+ [`src/core/scenario_loader.c`](file:///home/caixuf/code/FlowEngine/src/core/scenario_loader.c)（约 870 行），C 接口，`extern "C"`。

**只支持 JSON**（走 cJSON），没有 XML / YAML / 二进制。

顶层键的完整清单（解析位置 `scenario_loader.c:270-808`）：

| 键 | 目标 |
|---|---|
| `name` / `description` | 元信息 |
| `random_seed` | 默认 **42** |
| `duration_s` | 场景时长 |
| `ego` | `x, y, heading, init_speed, target_speed, wheelbase, length, width, max_steer` |
| `actors[]` | `id, type, x, y, vx, vy, len, wid, segment_id, s, l` |
| `pass_criteria` | `no_collision, max_duration_s, min_avg_speed_mps, min_distance_m` |
| `npc_lane_change` | bool，驱动 `enable_mobil` |
| `route` | `trigger_x, trigger_y, target_lane, target_speed, branch_id, type, label` |
| `road` | `curve_start_x, curve_length_m, curve_offset_m` |
| `road_network` | `edges[0].{type, lane_count, lanes, lane_width, oneway}` |
| `traffic_lights[]` | `id, x, y_lane, heading, red_s, yellow_s, green_s, phase_offset_s` |
| `etc_gates[]` | `id, x, y, heading, approach_speed, open_range_m` |
| `stop_lines[]` | `id, x, y`（**见 2.3，是死数据**） |
| `construction_zones[]` | `id, x, y, length, width` |
| `lighting` | `day` / `night` / `dusk` |
| `weather` | 白名单：`rain, snow, overcast, fog, clear` |
| `visibility_m` | 仅当 `10.0 ≤ v ≤ 5000.0` 接受，默认 **1000.0** |
| `scenarios[]` | 带 trigger 的子场景 |
| `choreography` | `loop_period_s`（默认 45.0）+ `beats[]` |
| `map_file` / `route_file` | 地图引用，由 `resolve_map_reference` 消费 |

容量上限（`scenario_loader.h:73-89`）：actors 64、红绿灯 16、ETC 4、停止线 16、施工区 4、choreography beats 16。

> [!NOTE]
> **溢出的处理不一致。** actor 溢出会 `LOG_WARN` 并明确报出丢了几个（`scenario_loader.c:349-352`）——这是为了修一个具体 bug：`straight_road.json` 的第 42 个 actor（唯一的行人）曾被静默丢弃。**但其他数组（红绿灯、闸门、停止线、施工区、beats）的溢出是静默截断**（`:536, 568, 598, 620, 685, 724, 767`）。
>
> 写场景时如果 NPC 行为诡异，先数一数数组有没有超上限。

### 2.2 仓库里的 23 个场景

```
auto_parking.json           dense_npc.json             parallel_parking_exam.json
beijing_guomao.json         lane_change_traffic.json   right_turn_side_parking.json
city_comprehensive.json     multi_light.json           road_driving_exam.json
city_grid_map.json          oncoming.json               safe_driving_exam.json
city_ring_map.json          osm_city_map.json           straight_road.json
cross_intersection.json     osm_lujiazui_v2.json        suite.json
curve_road.json             osm_munich.json             traffic_rules_exam.json
                           osm_zhengdong.json          urban_challenge.json
```

`suite.json` 是清单（manifest），不是场景。

### 2.3 停止线：解析了但从不生成

`stop_lines` 在 `scenario_loader.c:595-613` 被解析进 `ScenarioConfig.stop_lines`（上限 16），但 `populate_entities_from_scenario` **从不分配 `EntityType::StopLine`**。

`flowsim_node.cpp:1103` 里 `StopLine` 的唯一出现是构建障碍物列表时的排除过滤：

```cpp
e.type == flowsim::EntityType::StopLine) continue;
```

**全仓库没有 `stop_line_count` 消费者。** 旧稿说「FlowSim 生成停止线实体」是错的。

停止线目前在规划侧是**虚拟墙**（第 16 章 `st_graph.c` 里由红绿灯生成 S-T 禁行区），不需要实体。

---

## 3. 路网：从 JSON 到 OpenDRIVE

### 3.1 真实的加载链路

```
map.json 或场景内联 road_network.edges
        │
        ▼  system("python3 tools/json_to_xodr.py <场景> -o <tmp>/flowsim_<名>.xodr")
OpenDRIVE .xodr 文件
        │
        ▼  esmini RoadManager（RM_* C API）
FlowRoadNetwork 封装
```

**C 代码里不直接读 OpenDRIVE。** `flowsim_node.cpp:3109` 的 `convert_scenario_to_xodr`（定义在 `:324-415`）用 `system()` 调一个 **Python 子进程**做格式转换。转换失败 ⇒ `roads_loaded = false` ⇒ 仿真降级到世界坐标兜底路径。

> [!WARNING]
> **这是一个运行时依赖 Python 解释器和文件系统可写性的设计。** 好处是换地图格式只要改 Python 脚本；代价是场景加载路径上多了一个进程和一次文件 I/O。
>
> 另外：`esmini_stub.cpp` 是**全空桩**——每个 `RM_*` 调用都返回 `-1`/`0`。没有 esmini 的构建里 `RM_Init` 返回 −1 ⇒ 加载失败 ⇒ 降级。**旧稿说 `esmini_stub.cpp` 是「解析 OpenDRIVE 的转换桥」，恰好说反了。**

### 3.2 地图文件 schema

`maps/city_center/map.json`：

```json
{
  "schema_version": 1, "map_id": "...", "name": "...",
  "roads": [
    { "id": "r0", "nodes": [[x,y,z], ...], "type": "urban",
      "lanes": [ { "id": "l1", "index": 1, "width": 3.5, "direction": 1,
                   "centerline": [[x,y,z], ...] } ] }
  ],
  "junctions": [ ... ], "buildings": [ ... ], "landmarks": { ... }
}
```

**每条车道有自己的中心线折线**，不只是车道索引。这比「一条中心线 + 宽度」的信息量大得多——不对称道路、渐变车道宽都能表达。

9 个地图目录：`maps/{beijing_guomao, city_center, city_grid, city_ring, osm_lujiazui_v2, osm_munich, osm_test, osm_zhengdong, examples}`。

### 3.3 车道生成是对称双向的

`tools/json_to_xodr.py:825-856`：

> center（参考线 width=0）+ right N 条顺向 + left N 条对向（双向道路）

**所以 `lanes: 4` 意味着每侧 4 条、共 8 条可行驶车道。** `scenarios/straight_road.json:27` 写 `"lanes": 4` 而描述写「3000m 双向 4 车道」——**这个描述是含混的**，实际生成的是 4+4。

车道线遵循 **GB 5768**（`_lane_roadmark`，`json_to_xodr.py:719-729`）：最内侧黄色实线，其余白色虚线。

**OpenDRIVE 的车道 id 约定**（`road_network.h:29-32`）：`0` = 参考线，**正 = 左，负 = 右**。仓库注释指出 `json_to_xodr.py` 会把 `y<0 = 左车道, y>0 = 右车道` 归一化到 OpenDRIVE 语义。

### 3.4 关于 `edge.type`

旧稿给了一张表说 `edge.type` 决定摩擦系数（`highway` → μ=0.9、`urban` → μ=0.8 等）并影响 Three.js 渲染分支。**μ 那一列在代码里不存在。**

真实情况：`road_network.edges[0].type` 被解析，但车辆动力学里的摩擦只有两处来源——`pacejka_mu`（恒 0.7，`physics.cpp:271`）和稳定性护栏里硬编码的 `0.8`（`physics.cpp:204`）。**`edge.type` 与摩擦无关。**

`edge.type` 的真实用途是道路分类标签（`json_to_xodr.py` 用它决定几何原语和车道线），以及前端的渲染分支选择——后者在 `tools/flowboard/` 的 JS 里，不在 C++ 里。

### 3.5 车道横向偏移的唯一公式

`modules/adas_nodes/flowsim/lane_frenet.h:59`：

```
lane_center_t = sign(lane_id) · (|lane_id| − 0.5) · lane_width
```

`LANE_WIDTH_FALLBACK = 3.5`（`:38`）。

这里严格区分了**两种偏移语义**：
- `ref_offset`——相对参考线（`Entity::offset` 存的是这个）
- `lane_internal`——相对车道中心（esmini 的 `RM_SetLanePosition` 的 offset 参数是这个）

两者由 `lane_internal_from_offset`（`:77`）和 `offset_from_lane_internal`（`:98`）互转。**混淆这两者是路网相关 bug 的常见来源。**

### 3.6 三个核心抽象

| 类 | 文件 | 职责 |
|---|---|---|
| `FlowRoadNetwork` | `road_network.h:79-186` | esmini `RM_*` 的薄封装。`frenet_to_world` / `world_to_frenet` / `speed_limit`（默认兜底 **13.89 m/s** = 50 km/h，`:139`）/ `lane_width` / `detect_junctions`（聚类半径 15.0 m） |
| `RoadPosition` | `road_position.h:47-172` | **每车一个 esmini 位置句柄**，用 `RM_PositionMoveForward` 沿真实 OpenDRIVE 拓扑推进 |
| `Route` | `route.h:63-155` | 一条有序的 esmini road 链，按端点邻近 + 航向连续贪心构建（`tol=4.0`） |

**两个必须知道的坑：**

**其一，`frenet_to_world` 故意忽略 esmini 返回的航向。** `road_network.cpp:108-136` 用两点弦差分重算：

> esmini 的位置句柄在 `RM_SetWorldXYHPosition` 之后会带着过期的 heading 状态。探针实测：干净句柄 7.88°，被污染的句柄 239.5°。

**其二，`RoadPosition` 运行时必须用 `relocate()` 而不是 `init()`。** `road_position.h:73-88` 记录：`init()` 会 `RM_DeletePosition` + `RM_CreatePosition`，这会移动 esmini 内部的句柄数组，导致**所有 NPC 一起偏移约 200 米**。

还有个反直觉的细节（`road_position.h:38-45`，2026-08-14 核对 esmini 源码确认）：`junctionSelectorAngle` 的语义是 `0.0` = 直行、`π/2` = **左**、`3π/2` = **右**、`-1.0` = 随机。**esmini 自己的注释（`pi=直行, pi/2=右`）是错的**，头文件里明确标了出来。

### 3.7 Route 的 Hermite 桥接

`route.h:41-50` 支持**虚拟 Hermite 桥接段**，跨过 OSM 几何缺失的路口缝隙。曲率被限制在 $\lvert\kappa\rvert \le 1/R$。

场景有地图引用时优先走 A*（`build_route_via_astar`，`flowsim_node.cpp:2774`，在 `:3118` 被调用），否则回落到贪心 `Route::build()`。

> [!NOTE]
> **关于接缝平滑**：旧稿说 `Route::build()` 会检查端点距离 $\Delta d < 0.01$ m 和航向跳变 $\Delta\psi < 0.05$ rad，不连续就插三次样条。真实实现是按 `tol = 4.0` m（`route.h:69`）做端点邻近 + 航向连续的贪心串接，并**不**做曲率连续性检查——不平滑的接缝靠 Hermite 桥接段补。
>
> **关于 Lanelet2**：`road_network.h:148-160` 有一段注释描述「D2-03 加载真 Lanelet2 后，可在这里换成 `LaneletMap::laneletLayer.find`」。**这是 TODO 注释，不是已实现**。当前路径里没有 Lanelet2。

---

## 4. 车辆动力学：三套模型

### 4.1 派发点：只有自车能选

`flowsim_node.cpp:2165-2174`：

```cpp
if (strcmp(g.physics_model, "dynamic") == 0) {
    flowsim::step_bicycle_dynamic(ego, FLOWSIM_DT_SEC,
                                  ego.throttle, ego.brake, ego.steer);
} else if (strcmp(g.physics_model, "pacejka") == 0) {
    flowsim::step_bicycle_dynamic_pacejka(ego, FLOWSIM_DT_SEC,
                                          ego.throttle, ego.brake, ego.steer);
} else {
    flowsim::step_bicycle(ego, FLOWSIM_DT_SEC,
                          ego.throttle, ego.brake, ego.steer);
}
```

`g.physics_model` 默认 `"kinematic"`（`:137`），从节点参数解析（`:3028-3038`）。

**NPC 走的是无条件路径**（`npc_ai.cpp:774-775`）：

```cpp
// 4. 纵向积分：step_bicycle 只用来更新 speed（steer=0），位置随后覆盖
step_bicycle(npc, dt, throttle, brake, 0.0);
```

`steer` 传 **0.0**，位置随后被 `RoadPosition::advance` 或 `frenet_to_world` 覆盖。**`step_bicycle` 在这里纯粹是个纵向速度积分器。**

### 4.2 运动学模型 `step_bicycle`

`physics.cpp:138-169`：

```cpp
void step_bicycle(Entity& e, double dt, double throttle, double brake, double steer) {
    e.speed += longitudinal_accel(e, throttle, brake, e.speed) * dt;
    if (e.speed < 0.0 && throttle >= 0.0) e.speed = 0.0;
    if (e.speed < -4.0) e.speed = -4.0;
    if (e.speed >  60.0) e.speed = 60.0;

    update_steer(e, steer, dt);
    e.yaw_rate = (e.speed / e.wheelbase) * std::tan(e.steer);
    e.heading += e.yaw_rate * dt;
    normalize_heading(e);

    /* 位置积分：参考点在车身中心，带 half_wb·yaw_rate 的旋转项 */
    double half_wb = e.wheelbase * 0.5;
    double vx_rear = e.speed * std::cos(e.heading);
    double vy_rear = e.speed * std::sin(e.heading);
    e.x += vx_rear * dt - half_wb * std::sin(e.heading) * e.yaw_rate * dt;
    e.y += vy_rear * dt + half_wb * std::cos(e.heading) * e.yaw_rate * dt;
    e.vx = vx_rear - half_wb * std::sin(e.heading) * e.yaw_rate;
    e.vy = vy_rear + half_wb * std::cos(e.heading) * e.yaw_rate;
}
```

**最关键的一点：参考点是车身中心，不是后轴。** `half_wb · yaw_rate` 那一项就是全部意义所在——它把「中心的速度」和「后轴的瞬时旋转中心」区分开。

`test_entity_physics.cpp:228` 的 `test_rear_axle_no_slip` 就是这条性质的回归测试：断言后轴横向滑移 < 0.2 m/s。

`yaw_rate` 在运动学模式下是**纯代数输出**（`:149`），不是积分状态。

### 4.3 纵向模型（三套共用）

`physics.cpp:47-54`：

```cpp
static double longitudinal_accel(const Entity& e, double throttle, double brake, double v) {
    double drive_force = throttle * 5000.0;
    double sgn = (v > 0.0) ? 1.0 : (v < 0.0 ? -1.0 : 0.0);
    double brake_force = brake * 8000.0 * sgn;
    double drag_force  = e.drag_coeff * v * std::fabs(v);
    return (drive_force - brake_force - drag_force) / e.mass;
}
```

$$a = \frac{F_{drive} - F_{brake} - c_d\, v\lvert v\rvert}{m}$$

- 满油门 **5000 N**、满刹车 **8000 N**，都是硬编码；
- 空气阻力**不是** $\tfrac12\rho C_d A v^2$，而是没有那个 ½ 的 $c_d v\lvert v\rvert$（$v>0$ 时等于 $c_d v^2$）；
- **没有滚动阻力、没有变速箱、没有传动系**；
- **$v = 0$ 时刹车产生零力**（静摩擦），只有驱动力起作用。

这解释了为什么仿真里起步和刹停的时序总是和真车差一点——它本质上是一个一阶力平衡，没有传动系的迟滞。

### 4.4 转向执行器：一阶滞后 + 速率限制

`physics.cpp:56-72`：

```cpp
static void update_steer(Entity& e, double steer_cmd, double dt) {
    /* 正常行驶限幅 0.25rad（≈14°），掉头时（steer_override=true）放宽到 0.60rad（≈34°）。
     * 宽路掉头打一圈多（~0.50rad），窄路窄路打死方向盘（~0.55-0.60rad），
     * 详见 Python 扫描结果。 */
    const double steer_limit = e.steer_override ? 0.60 : 0.25;
    if (steer_cmd >  steer_limit) steer_cmd =  steer_limit;
    if (steer_cmd < -steer_limit) steer_cmd =  -steer_limit;

    double alpha = dt / (e.steer_tau + dt);
    double steer_next = e.steer + alpha * (steer_cmd - e.steer);

    double max_rate = e.steer_rate_max * dt;
    double d = steer_next - e.steer;
    if (d >  max_rate) steer_next = e.steer + max_rate;
    if (d < -max_rate) steer_next = e.steer - max_rate;
    e.steer = steer_next;
}
```

一阶滞后 $\alpha = \frac{dt}{\tau + dt}$（`steer_tau = 0.15 s`）+ 速率限制（`steer_rate_max = 0.6 rad/s`）+ 硬限幅 0.25 rad（`steer_override` 时 0.60）。

> [!IMPORTANT]
> **自车转向实际上经过了两级一阶滞后。** 物理步之前，节点还有一层 EPS 滤波（`flowsim_node.cpp:2159-2164`）：
> ```cpp
> const double eps_alpha = 0.4;
> ego.steer = eps_alpha * raw + (1.0 - eps_alpha) g.prev_steer;
> ```
> 所以完整链路是：0.4 系数滤波（60 Hz 下约 0.025 s 有效延迟）→ 物理层的 τ=0.15 s 滞后。倒挡或脱离路面时才绕过。
>
> 这解释了第 17 章提到的「转向包络与限幅」在仿真里为什么比真车更容易稳住——**仿真里转向比真车慢**。

### 4.5 动态模型（2 自由度）

线性轮胎版 `integrate_lateral_dynamics`（`physics.cpp:179-217`）：

```cpp
double alpha_f = clamp_slip(e.steer - std::atan2(e.v_y_body + a * e.yaw_rate, vx));
double alpha_r = clamp_slip(       - std::atan2(e.v_y_body - b * e.yaw_rate, vx));
e.F_yf = e.tire_stiffness_f * alpha_f;
e.F_yr = e.tire_stiffness_r * alpha_r;

double vy_dot = (e.F_yf + e.F_yr) / e.mass - vx * e.yaw_rate;
double r_dot  = (a * e.F_yf - b * e.F_yr) / e.yaw_inertia;

e.v_y_body += vy_dot * dt;
e.yaw_rate += r_dot  * dt;
```

$$\dot v_y = \frac{F_{yf} + F_{yr}}{m} - v_x r, \qquad \dot r = \frac{a F_{yf} - b F_{yr}}{I_z}$$

**这是平面单轨（bicycle）模型，不是三维刚体。** 没有侧倾/俯仰，`yaw_inertia` 是唯一的转动惯量。

$\dot v_y$ 里的 $-v_x r$ 是科氏/陀螺耦合项——**它也是模型发散的源头**（见 4.7）。

两种动态模型在 `LOW_SPEED_MS = 5.0 m/s` 以下都退化为运动学。

**它们的位置积分和运动学模型不同**：`physics.cpp:211-215` 直接把体坐标系速度投影到世界，**没有 `half_wb` 项**。参考点隐式地不一样——写对照测试时要注意。

### 4.6 Pacejka 96

`physics.cpp:84-91`：

```cpp
static double pacejka_lateral_force(double alpha, double mu, double Fz,
                                    double B, double C, double E) {
    double Bx = B * alpha;
    double y  = std::sin(C * std::atan(Bx - E * (Bx - std::atan(Bx))));
    return (mu * Fz) * y;
}
```

$$F_y = \mu F_z \sin\!\Big(C \arctan\big(B\alpha - E(B\alpha - \arctan(B\alpha))\big)\Big)$$

轴荷分配（`:104-106`）：

```cpp
double Fz_f = e.mass * GRAV * b / e.wheelbase;
double Fz_r = e.mass * GRAV * a / e.wheelbase;
```

`GRAV = 9.81` 是 `integrate_lateral_dynamics_pacejka` 内的 `constexpr`（`:95`）——**而线性版 `integrate_lateral_dynamics` 没有这个常量，在 `:204` 直接写死 `0.8 * 9.81`**。

**Pacejka 参数对三种车型完全相同**（`physics.cpp:264-273`，写在 switch **之前**）：

```cpp
e.pacejka_b = 8.0;
e.pacejka_c = 1.2;
e.pacejka_e = -0.2;
e.pacejka_mu = 0.7;
```

**峰值侧向加速度 ≈ 0.7 g。** μ 是唯一的湿度/雪地调节旋钮，而 FlowSim 本身**不会随天气改变它**（见第 6 节）。

### 4.7 稳定性护栏：一次发散的教训

`physics.cpp:196-205` 记录了 PR #72（2026-07）的事故：线性轮胎 + 滑移角硬饱和的组合下，**高速持续转向时 `v_y_body` 发散到 5×10⁴ m/s**（20 m/s 以上，25 秒左右）。

护栏：

```cpp
const double max_vy = std::fabs(vx) * std::tan(SLIP_ANGLE_MAX) * 1.5;  // SLIP_ANGLE_MAX = 0.12
const double max_r  = (0.8 * 9.81) / std::max(vx, 1.0);
```

> **这不是「模型无条件稳定」，而是「模型被钳住了」。** 任何关于收敛性的说法都要带上这个前提。

值得注意的是护栏里的摩擦上限是**硬编码的 0.8**，而不是 `pacejka_mu`（0.7）。两个模型用了不同的摩擦上限。

### 4.8 车辆参数表

`apply_vehicle_defaults`（`physics.cpp:258-320`）：

| | Car / Ego | SUV | Truck |
|---|---|---|---|
| 轴距 L (m) | 2.7 | 2.85 | 5.0 |
| 质量 (kg) | 1500 | 1800 | 8000 |
| 转动惯量 Iz (kg·m²) | 2250 | 3200 | 25000 |
| 侧偏刚度 Cf = Cr (N/rad) | 80000 | 90000 | 180000 |
| 阻力系数 | 0.4 | 0.45 | 0.6 |
| 尺寸 (m) | 4.6 × 2.0 | 4.8 × 2.0 | 8.0 × 2.4 |
| 最大制动 (m/s²) | 4.0 | 4.0 | 3.0 |

质心：`a = CG_FRONT_FRAC × L = 0.45L`，`b = 0.55L`（`physics.cpp:32`）。注释说明「质心略偏前，等刚度下呈不足转向」。

### 4.9 一个真实的 bug：场景配置的自车参数不生效

`flowsim_node.cpp:611-613`：

```cpp
ego.mass = 1500.0;
ego.drag_coeff = 0.3;
flowsim::apply_vehicle_defaults(ego);
```

`ego.type == EntityType::Ego` 落在和 `Car` 同一个 `case` 分支里（`physics.cpp:298-307`），而那个分支**硬编码** `mass = 1500.0, drag_coeff = 0.4`。

**所以第 611-612 行设的值两行后就被覆盖了。** `drag_coeff = 0.3` 是死代码。

同理，`:608-610` 设的 `wheelbase` / `length` / `width` 也被覆盖成 2.7 / 4.6 / 2.0。

> [!WARNING]
> **这意味着场景文件里 `ego.wheelbase` 根本不会反馈进自车动力学。** 它只通过 `publish_vehicle_state`（`:1087-1094`）广播给下游节点（控制节点会读它来标定），但**自车自己跑的还是硬编码的 2.7**。
>
> 如果你在场景里把轴距调成 3.5 m 想测试长轴距车的行为，自车会按 2.7 跑，而控制节点以为轴距是 3.5——**两者不一致，会产生本不该有的跟踪误差**。这和第 19 章那个 `max_steer` 0.35 vs 0.22 是同一类问题：**配置值在某一层被静默忽略。**

---

## 5. NPC 与场景事件

### 5.1 NPC 不是 Pure Pursuit

`npc_ai.cpp`（1191 行）里**没有 pure-pursuit**。NPC 的横向位置由路网强加，AI 只决定 `throttle / brake / offset`。

`step_npc_vehicle`（`npc_ai.cpp:446-1049`）每帧流程：

1. `uturn_yield` —— 对向来车遇自车掉头时让行（`:459-474`）
2. 碰撞冷却处理（`:475-485`）—— 速度 0、刹车 1，但**没有提前 return**
3. 横向控制：CutIn PID（`:503-556`）/ LaneChange 插值（`:557-587`）/ 强制 `target_offset = offset`（`:599-601`）
4. MOBIL 评估（`:643-721`）—— **默认关闭**
5. `find_lead` 同车道前车搜索（`:607`）
6. 状态机给出 `v_desired`（`:723-758`）
7. `v_desired` → 油门/刹车（`:760-772`）
8. `step_bicycle(npc, dt, throttle, brake, 0.0)` —— **只更新速度**（`:775`）
9. 位置来自 `RoadPosition::advance`（`:788-944`）或 `route_s` 推进 + `frenet_to_world`（`:945-1002`）或世界-Frenet 兜底（`:1003-1048`）

**位置状态机**（`entity.h:60-67`）有 7 个状态：`Cruise / Follow / StopForTL / LaneChange / CutIn / Stopped / Yield`。

### 5.2 一个「简化版 IDM」

`npc_ai.cpp:121-132`：

```cpp
static double idm_desired_speed(double v, double gap, double target_v, ...) {
    double safe_gap = cfg.idm_safe_gap_base + v * cfg.idm_safe_gap_time;
    double gap_error = gap - safe_gap;
    if (gap_error > 0) {
        return std::min(v + cfg.accel_rate * dt, target_v);
    }
    double brake = cfg.follow_decel_factor * std::exp(-gap_error / 2.0);
    return std::max(0.0, v - brake * dt);
}
```

$$d_{safe} = d_0 + T v$$

**它不是教科书的 IDM**：没有 $(\Delta v)^2$ 项，也没有 $s^*/s$ 比例。第 15 章讲的 CTG 跟车律和这里形式相同，但这是另一套独立实现。

> [!NOTE]
> 有一处例外：`mobil_idm_accel`（`npc_ai.cpp:198-211`）用的是**标准**自由流项 $a_{max}(1 - (v/v_0)^4)$。但它只在 MOBIL 开启时可达，而 MOBIL 默认关。

油门/刹车转换（`:760-772`）：

```cpp
throttle = std::min(1.0, dv / (cfg.accel_rate * dt + 0.01));
throttle = std::max(0.2, throttle);      /* 地板 0.2 */
brake = std::min(1.0, -dv / (cfg.brake_rate * dt + 0.01));
```

**油门地板 0.2** 意味着 NPC 永远不会完全松油门——这在跟停场景下会造成 NPC 缓慢爬行。

**死锁逃逸**（`:750-752`）：`gap < 2.0 && speed < 0.5` 时把 `gap` 伪造成 `10.0` 强制重新起步。

`NpcAiConfig` 默认值（`npc_ai.h:33-79`）：`idm_safe_gap_base 5.0`、`idm_safe_gap_time 1.5`、`accel_rate 1.5`、`brake_rate 3.5`、`ped_boundary 7.8`、`look_ahead 80.0`、`enable_mobil false`。

### 5.3 MOBIL：实现了但默认关

`enable_mobil{false}`（`npc_ai.h:79`），由 `scenario.npc_lane_change` 驱动（`flowsim_node.cpp:3103`）。`npc_ai.cpp:630-642` 的注释说明了关闭原因：「用户需求：演示场景中 NPC 各守其道不变道」。

**后果**：`NpcState::LaneChange` 在 MOBIL 关闭时从不被设置，所以 `:557-587` 的 LaneChange 分支是**事实上的死代码**（代码自己在 `:565-567` 承认了）。

**只有编排（choreography）驱动的 NPC 会变道**，走 CutIn PID。而 CutIn 直接绕过 `boundary_permissive`（`npc_ai.cpp:292`）。

无条件安全钳位（`:535-536, 585-586`）把 `offset` 硬钳在 $\mp 0.3$ m，NPC 永不越过中心线进入对向。

### 5.4 红绿灯：4 个相位

`scene_events.cpp:46-117`：

```cpp
double T = green_s + yellow_s + red_s;
double tp = std::fmod(sim_time_s + offset, T);

const double flashing_s   = std::min(TL_FLASHING_GREEN_SECONDS, green_s);
const double steady_green_s = green_s - flashing_s;
if (tp < steady_green_s)       → Green
else if (tp < green_s)          → FlashingGreen
else if (tp < green_s+yellow_s) → Yellow
else                            → Red
```

**闪烁绿**在绿灯末尾，模拟国内路口的「绿灯闪烁提示即将变黄」。

> [!WARNING]
> `entity.h:186` 的注释写 `phase_state // 0=绿 1=黄 2=红`——**这是错的**。实际枚举有 4 个值，`TLPhase` 在 `scene_events.h:28-33` 定义为 `Green / FlashingGreen / Yellow / Red`。
>
> 写代码时按注释理解相位会直接出错。

NPC 响应（`scene_events.cpp:145-196`）：**红灯或黄灯**都触发 `StopForTL`，刹车距离 $v^2/(2 a_{max}) + 5.0$ m（`:183-184`）。**NPC 对 ETC 闸门完全无反应**（`:179` 注释：「ETC 闸门：NPC 不响应闸栏杆」）。

互斥检测（`:95-116`）是个 O(n²) 循环，**只 `LOG_WARN`，不实现真正的互斥**（注释明说「只做检测+告警，不实现完整互斥机制」）。

红绿灯杆的摆放：esmini 加载时用 `world_to_frenet` 校正手填坐标（`:897`），杆立在**路缘外 1.5 m**（`:918`），横臂航向垂直于道路切线（`:934`）。

### 5.5 ETC 闸门：配置被忽略

`flowsim_node.cpp:977` 认真地把 `open_range_m` 存进了 `e.width`：

```cpp
e.width = eg->open_range_m;          /* open_range_m 存到 width */
```

**但 `tick_etc_gates` 从不读它**（`scene_events.cpp:119-143`）：

```cpp
double dist = gate.x - ego.x;
if (dist > 50.0)      → 关闭
else if (dist > 10.0) → 开启中, phase_timer += dt*0.5
else if (dist > 0)    → 打开,   phase_timer = 1.0
else                  → 通过后关闭, phase_timer -= dt*0.5
```

`50.0` / `10.0` 是**字面量**。而且 `dist` 是 `gate.x - ego.x`（假设闸门在世界 X 轴上，`:124` 注释），**不是 Frenet 距离**——在非东西向的道路上会直接错。

### 5.6 施工区：不是实体，是伪造的感知障碍

`construction_zones` 被解析（上限 4，默认 `length=30.0` m、`width=0.0` 表示占满车行道），**但不生成实体**。它在两处起作用：

1. **作为伪造的感知障碍发布**——`append_construction_obstacles`（`flowsim_node.cpp:1017-1053`），在 `front_x = cz->x - cz->length/2` 排一行 `type="construction"` 的障碍，**围栏厚度 4 m**（`:1020`）：

```cpp
const double wall_ol = 4.0;  /* 围栏纵向厚度：覆盖薄墙漏检 */
```

   这些障碍只存在于 JSON 载荷里，**不参与碰撞检测、不参与 NPC AI**（`:1009-1010` 注释）。4 m 厚度正是第 16 章讲的「薄墙漏检」的对策。

2. **掉头前缘查询**——`forward_construction_front_s`（`flowsim_node.cpp:1586-1604`）。

> **但第 2 条路径已经死了。** `:2430-2431` 的注释：「U-turn 触发已迁移到 behavior_planner → planning → control 链路。flowsim 只执行控制指令（含 gear），不再做掉头决策。」`forward_construction_front_s` **没有调用者**。
>
> 掉头从仿真里移走了，这是对的——**掉头是决策问题，不是被控对象问题**。

### 5.7 行人

`step_pedestrian`（`physics.cpp:171-175`）纯粹是 `x += vx·dt; y += vy·dt`。

`npc_ai.cpp:1068-1099`：两个速度都为 0 则冻结；到达 $\lvert y\rvert \ge$ `ped_boundary`（7.8 m）后停下等 `ped_wait_time`（3 s）再反向。默认尺寸 0.5 × 0.5 m（`flowsim_node.cpp:719-720`）。

**行人不会避让自车，也不会被自车影响**——它完全开环。

### 5.8 Poisson 动态车流

`step_poisson_traffic`（`flowsim_node.cpp:1607-1721`），由 `enable_poisson_traffic` 门控：

- 距自车 220 m 外剔除（`:1627`）
- 配额 `poisson_max_vehicles = 24`
- 到达概率 `poisson_rate · dt`，`poisson_rate = 0.25 veh/s`
- 75% 在前方 65–120 m 生成，25% 在后方 50–80 m（`:1642-1651`）
- 最小间距 25 m（`:1659`）
- 车型随机：**10% 卡车 / 20% SUV / 70% 轿车**（`:1671-1673`）
- 速度抖动 0.85–1.10 倍（`:1712`）

NPC 回收（`recycle_npc`，`npc_ai.cpp:330-444`）：驶过路线末端的顺向 NPC 循环到 `ego_route_s - back`（`back = 50 + (id % 10) * 20` m）。对向（`route_dir < 0`）循环到 `total - back`。碰撞后 `crash_cooldown = 0.5 s`。

### 5.9 碰撞不是冲量响应

`collision.cpp` 的处理是：两车速度归零，沿 `route_s` **分离 2.0 m**（`:120`），或用 AABB 近似的最小平移向量。**没有冲量、没有恢复系数、没有质量加权。**

护栏碰撞（`:186-201`）：保留 0.3 倍速度，`crash_cooldown = 0.3 s`。自由落体 `z` 下限 200 m（`:225`）。

---

## 6. 天气与光照：25 行代码

### 6.1 全部实现

[`modules/adas_nodes/sensor_model_weather.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/sensor_model_weather.c) **整个文件 25 行**：

```c
double sensor_model_camera_visibility(double visibility_m) {
    double factor = visibility_m / 200.0;
    if (!(factor >= 0.1)) factor = 0.1;   /* 兼收 NaN / 负值 */
    if (factor > 1.0) factor = 1.0;
    return factor;
}

double sensor_model_weather_attenuation(double visibility_m, const char* weather) {
    double att = 1.0 - sensor_model_camera_visibility(visibility_m);
    if (weather && (strstr(weather, "rain") || strstr(weather, "fog") ||
                    strstr(weather, "snow"))) {
        if (att < 0.3) att = 0.3;   /* 降水/雾霾散射地板 */
    }
    if (att > 1.0) att = 1.0;
    if (att < 0.0) att = 0.0;
    return att;
}
```

`!(factor >= 0.1)` 这个写法是为了**同时捕获 NaN**——因为 `NaN >= 0.1` 恒为 false，取反后为 true。这个细节值得学。

### 6.2 它建模了什么，没建模什么

| 说法 | 事实 |
|---|---|
| 建模了雨/雾/雪 | **只是一个 `strstr` 子串测试**，把衰减地板设在 0.3。没有雨滴、没有散射积分、没有强度、没有累积 |
| 建模了**太阳角度** | **完全没有。** 全仓库不存在任何太阳高度角变量 |
| 天气影响抓地力（μ） | **不。** FlowSim 不会随天气改变 `pacejka_mu`，恒为 0.7 |
| 光照 | 只是个 3 值枚举，前端渲染提示。后端唯一的用途是自动开灯 |

而且 `strstr` 是**子串**匹配，所以 `"dense_fog"` 会匹配 `"fog"`。测试 `test_weather_attenuation_dense_fog`（`tests/test_adas_nodes_logic.c:417`）利用的正是这个特性。

### 6.3 光照的三个枚举值

`ScenarioLighting`（`scenario_loader.h:238-242`）：`DAY=0, NIGHT=1, DUSK=2`，无法识别的字符串回落 DAY。

`scenario_loader.h:230-242` 的注释给出了三个值对应的**前端**渲染参数（日间 环境光 0.20 + 平行光 1.20；夜间 0.04 + 0.15 + headlights；黄昏 0.12 + 0.50 + 暖色调）。**这些是 JS 渲染器的常量，不是后端物理。**

后端唯一的用法（`flowsim_node.cpp:2572-2583`）：

```cpp
const bool dark = g.scene_pub_cfg.lighting != SCENARIO_LIGHT_DAY;
const bool weather_lights = weather=="rain"||=="storm"||=="snow"||=="fog"||=="sandstorm"
                            || visibility_m <= 500.0;
const bool foggy = weather=="fog"||=="sandstorm"||=="storm" || visibility_m <= 200.0;  // GB 4785
```

> [!WARNING]
> **这里的天气字符串和白名单对不上。** `scenario_loader.c:661-663` 的白名单只有 `rain/snow/overcast/fog/clear`，**没有 `storm` 和 `sandstorm`**。场景里写 `"storm"` 会在加载时被静默回落成 `"clear"`，于是 `weather_lights` 永远不会被这个字符串触发。只有游戏模式覆盖路径（`flowsim_node.cpp:1938` 读 `/tmp/flow_environment.json`）能设这两个值。

`environment/state` 每 30 周期发布一次 = **0.5 秒**（不是 60 Hz，`:2600`）。

### 6.4 传感器侧的默认值不一致

`scenario_loader` 的 `visibility_m` 默认 **1000.0**，而 `sensor_model_node.c:309` 的读取默认是 **200.0**。两个默认值差 5 倍。

---

## 7. flowsim 节点：话题与主循环

### 7.1 发布的话题

| 话题 | 频率 |
|---|---|
| `sim/tick` | 60 Hz |
| `vehicle/state` | 60 Hz |
| `road/ref_path` | 60 Hz |
| `road/traffic_lights` | 60 Hz |
| `localization/lane_match` | 60 Hz |
| `scene/frame` | 60 Hz |
| `road/geometry` | **每 150 周期 = 2.5 s**（`:79`）+ 路网/车道数变化时立即 |
| `environment/state` | **每 30 周期 = 0.5 s** |
| `sim/collision` | **事件驱动**（仅涉及自车时） |
| `world/buildings` | **只在 init 时一次** |

**订阅**：只有 `control/cmd`（`:3238`），走常驻 `BusQueueBridge`（`:1745`，深度 1，drop-oldest）。

发现 QoS（`:3242-3257`）：`road/geometry` 用 **1.0 s** 租约（最慢，因为 2.5 s 才重发一次），`vehicle/state`/`localization/lane_match`/`scene/frame` 用 20.0，`environment/state` 用 2.0。

### 7.2 主循环的十个阶段

```
Step 1   自车控制源 + 物理                    :2085-2239
Step 2   check_npc_scene_events              :2435
Step 2.5 apply_scenario_scripts              :2442
Step 2.8 step_poisson_traffic                :2454
Step 3   step_npc_vehicle / pedestrian        :2456-2466
Step 4   碰撞检测 + 响应                     :2469-2485
Step 4.2 车 ↔ 建筑 OBB                       :2492-2531
Step 4.5 apply_guardrail                     :2536-2542
Step 4.6 apply_gravity                       :2546-2552
Step 5   交通灯 / ETC / 编排 tick            :2555-2565
Step 5.5 VehicleActor::update_all_lights     :2572-2583
Step 6   clock_advance_us(FLOWSIM_DT_US)      :2586
Step 7   发布全部                             :2590-2618
         自适应 sleep                         :2627-2640
         cycle++                             :2642
         每 60 周期的不变量检查               :2673-2726
```

**Step 2 在 Step 3 之前**很关键：NPC 必须先知道「这个路口红灯了」，才能在 AI 里决定停下。

### 7.3 `CONTROL_STALE_TIMEOUT_US`

`flowsim_node.cpp:75`：

```cpp
#define CONTROL_STALE_TIMEOUT_US  2000000ULL
```

**2 秒 = 120 帧（@60 Hz）。** 三处使用：

1. `:1851-1859` —— 无新指令时把 `use_internal_cruise` 永久置 true
2. `:1874-1907` —— 桥自愈：回调计数真的停滞 **且** 指令陈旧 **且** 已在内部巡航，才重连（10 s 去抖，`:1891-1892`）
3. `:2093-2119` —— 真正的失效保护：指令超过 2 s 就 `throttle=0, brake=1.0, steer=0`，每 300 周期打一次 `[FSAFE]`

### 7.4 一个被改过的阈值

`flowsim_node.cpp:70-74` 和 `:2085-2092` 记录了设计历史：

> 原本是 **500 ms**；2026-08-03 提到 2000 ms，因为在 15 个任务、约 34% 消息总线丢包率下，2~3 帧的间隔就触发 FSAFE，交替的 `brake=1.0` / `throttle` 产生走走停停振荡，把车速钉在 0。

这与第 18 章的 `safety_raw_command_timeout_expired`（2 s）是**同一个量级的选择**，也与第 19 章执行器看门狗（3 s）构成 `2 < 3` 的分层。

### 7.5 不变量自检与 ASCII 调试视图

每 60 周期（约 1 s）跑一次静态/动态不变量检查，失败计数在清理时打成机器可 grep 的标记 `[INV] summary total=N`（`:3344-3346`），`demo_evaluator.py` 见到非零就判 FAIL。

每 100 周期（约 1.67 s）重新生成一份 ASCII 俯视调试图到临时文件（`:2727-2729`）。

**这是仿真层的自验证设施**，第 22 章的评估器直接消费它。

### 7.6 实体池：128 槽固定表

`Entity` 是 **AOS、128 槽固定池**（`entity.h:27, 216-282`），自车恒为 0 号（`flowsim_node.cpp:1738` 声明，`:592` 首个分配）。

`Entity` **非拷贝、可移动**（`entity.h:198-201`），因为 `RoadPosition` 持 esmini 句柄。`EntityPool::alloc` 找到第一个 `active == false` 的槽后做 `entities_[i] = Entity{}` + move-assign（`:234-235`），这会通过 `RoadPosition::operator=` **释放旧的句柄**。

**字段复用**（`scene_events.h:40-49` 有文档）：TrafficLight 实体借用其他字段存相位时长——

| 字段 | 实际含义（对 TrafficLight） |
|---|---|
| `Entity::throttle` | 绿灯秒数 |
| `Entity::brake` | 黄灯秒数 |
| `Entity::steer` | 红灯秒数 |
| `Entity::target_vx` | 相位偏移 |

写入在 `flowsim_node.cpp:879-882`，读取在 `scene_events.cpp:62-65`。`Entity::width` 还被 TrafficLight 借用来存车道中心 y（`flowsim_node.cpp:926`），被 ETCGate 借来存 `open_range_m`（`:977`）。

`flowsim_cleanup` 必须在 `RM_Close` 前把每个 `road_pos` 句柄清零（`:3352-3354`），否则静态析构顺序会导致 `RM_DeletePosition` 作用在已关闭的 RM 上 → **SIGABRT**。

---

## 8. 确定性与可复现性

### 8.1 有种子，但**不保证可复现**

- `ScenarioConfig::random_seed` 解析于 `scenario_loader.c:307-308`，默认 **42**
- 节点参数可覆盖（`:3020-3021`），但场景非零时场景赢（`:3095`）
- `srand(g.random_seed)` **只在 init 调一次**（`flowsim_node.cpp:3105`）

`rand()` 的消费点全部在 flowsim 里：NPC 出生转向意图（`:726-727`，60% 直行 / 20% 左 / 20% 右）、过路口重掷（`:910-911`）、Poisson 采样（`:1639-1649`）、动态车流车型（`:1671`）、速度抖动（`:1712, 1718`）、路口转向重掷（`npc_ai.cpp:910`）。

### 8.2 为什么仍然不确定

1. **glibc 的 `rand()` 是全局 LCG**，调用顺序依赖运行时条件。比如 `step_poisson_traffic`（`:2454`）在 NPC 循环（`:2456`）之前跑，但只在开关打开时才抽取——**翻转这个开关会移动后续每一次抽取**；
2. `rand()` 本身是劣质 RNG，不是 PCG/xorshift；
3. **`npc_request_state` 和 `find_lead` 按槽位顺序遍历 `EntityPool`**，而槽位分配依赖 `alloc()` 找空位——又依赖碰撞导致的释放。单帧不同就会级联；
4. **循环由墙上时钟驱动**。帧超时后逻辑时间仍推进固定的 16.67 ms，所以物理在给定输入序列下是确定的，**但输入序列来自异步消息总线**；
5. NPC 行为在给定池状态下完全确定，**自车不确定**——它由其他节点通过总线驱动。

> **正确的表述是：「提供了用于可复现 NPC 出生决策的种子」，而不是「仿真是确定性的」。**

---

## 9. 测试

| 测试 | 位置 | 注册 |
|---|---|---|
| `test_entity_physics` | [`test_entity_physics.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/test_entity_physics.cpp)（439 行，17 个测试） | `CMakeLists.txt:1206-1215`，ctest `entity_physics_tests`——**依赖 `ESMINI_RMLIB_TEST`**，否则 CMake `WARNING`（`:1218`） |
| `test_road_network` | [`test_road_network.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/test_road_network.cpp) | `modules/adas_nodes/CMakeLists.txt:403-438`，两个 ctest（`flowsim_road_network` / `_cross`），用 CTest `FIXTURES` 先跑 `tools/json_to_xodr.py` |
| `test_adas_nodes_logic` | [`test_adas_nodes_logic.c`](file:///home/caixuf/code/FlowEngine/tests/test_adas_nodes_logic.c) | 含 4 个天气测试（`:396-420`） |
| `test_modules` | [`test_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_modules.c) | `scenario_load` 的三个用例（`:936, 994, 1041`）+ `scenario_to_json` 往返（`:1068`） |

### 9.1 值得注意的物理断言

- `test_dynamic_turn:179` —— `CHECK(yaw_ss < yaw_kin)` 证明**不足转向**（动力学稳态横摆 < 运动学预测 $v/L\tan\delta$）
- `test_dynamic_lowspeed_degrade:223-225` —— 5 m/s 以下动力学与运动学**逐位相同**
- `test_rear_axle_no_slip:259` —— 后轴横向滑移 < 0.2 m/s
- `test_steer_override_full_lock:300` —— 掉头直径 $\approx 2\sqrt{R_{rear}^2 + (L/2)^2}$，$R_{rear} = L/\tan(0.60)$

### 9.2 一个必须限定的前提

**所有物理测试都用 `double dt = 0.05;`**（`:86, 107, 126, 143, 162, 189, 216, 240, 269, 275, 321, 353, 375, 396, 412`）——**和生产运行的 1/60 s 不是同一个步长**。

`physics.cpp:3` 的 `dt=0.05s（20Hz）` 注释也是同一个过时值的残留。`test_dynamic_lowspeed_degrade` 的「逐位相同」结论只在那个步长下验证过。

### 9.3 测试空白

| 未覆盖 | 原因 |
|---|---|
| `npc_ai.cpp` | **无专门单测**，只有 `test_adas_nodes_logic` 的间接覆盖 |
| 碰撞响应 | `docs/FLOWSIM_PHYSICS.md:27` 提到 `test_collision_events.cpp`，**这个文件不存在**（文档过时） |
| `scene_events.cpp` / `route.cpp` / `building.cpp` / `sim_digest.cpp` / `scene_pub.cpp` | 无专门测试 |

---

## 10. 死代码清单

| 项 | 位置 | 状态 |
|---|---|---|
| `stop_lines` 场景键 | `scenario_loader.c:595-613` | 解析后从不生成实体 |
| `forward_construction_front_s()` | `flowsim_node.cpp:1586-1604` | **无调用者**（掉头已迁到规划链路） |
| `is_dynamic` 局部变量 | `flowsim_node.cpp:2255` | 赋值后从不读；而且只测 `"dynamic"` 不测 `"pacejka"` |
| `ego.drag_coeff = 0.3` | `flowsim_node.cpp:612` | 两行后被 `apply_vehicle_defaults` 覆盖成 0.4 |
| `g.has_control_input` | `:298, 316, 227` | 写入、复位，从不读 |
| `e.width = open_range_m` | `:977` | 闸门 tick 用硬编码 50.0/10.0，不读它 |
| `NpcState::LaneChange` 分支 | `npc_ai.cpp:557-587` | MOBIL 关闭时是死代码（代码自己承认） |
| `entity.h:186` 相位注释 | — | 说 3 相位，实际 4 相位 |
| `sim_digest.h:45-46, 86-89` | — | 字段自述「从未被填充」/「无读取者」 |
| `road_network.h:149-160` | — | Lanelet2 迁移的 TODO 注释，未实现 |

---

## 11. 源码与资源对照

| 模块 / 层次 | 源码文件路径 | 核心符号 / API | 架构职责与设计要点 |
|---|---|---|---|
| **物理内核** | [`physics.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/physics.cpp)<br>[`physics.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/physics.h) | `step_bicycle`（`:138`）<br>`step_bicycle_dynamic`（`:219`）<br>`step_bicycle_dynamic_pacejka`（`:241`）<br>`update_steer`（`:56`）<br>`apply_vehicle_defaults`（`:258`） | 三套模型，前向欧拉。**默认运动学**；动态模型自车专属。运动学参考点在**车身中心**（`half_wb·yaw_rate` 项）。稳定性护栏钳住 PR #72 的发散 |
| **时间步** | [`flowsim_time.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/flowsim_time.h) | `FLOWSIM_DT_SEC`（1/60）<br>`FLOWSIM_DT_US`（16666） | **唯一事实源**，注释记录了「两份定义导致红绿灯差 3 倍」的事故 |
| **实体池** | [`entity.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/entity.h) | `struct Entity`（`:78-207`）<br>`EntityPool`（`:216-282`）<br>`EntityType`（`:31-39`）<br>`NpcState`（`:60-67`） | 128 槽 AOS 固定池，自车恒为 0 号。**非拷贝可移动**。存在**字段复用**（红绿灯借用 `throttle`/`brake`/`steer` 存相位秒数） |
| **路网** | [`road_network.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/road_network.h)<br>[`road_position.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/road_position.h)<br>[`lane_frenet.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/lane_frenet.h) | `FlowRoadNetwork`<br>`RoadPosition::advance`/`relocate`<br>`lane_center_t`（`:59`） | esmini 薄封装。**进程级单例，非线程安全**。`frenet_to_world` 重算航向规避 esmini 状态污染；运行时必须用 `relocate` 不能用 `init` |
| **NPC AI** | [`npc_ai.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/npc_ai.cpp)<br>[`npc_ai.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/npc_ai.h) | `step_npc_vehicle`（`:446`）<br>`idm_desired_speed`（`:121`）<br>`mobil_gain`（`:224`）<br>`recycle_npc`（`:330`） | **不是 pure-pursuit**。简化 IDM 无 $(\Delta v)^2$ 项。**MOBIL 默认关闭**。`offset` 硬钳 $\mp 0.3$ m |
| **场景事件** | [`scene_events.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/scene_events.cpp) | `tick_traffic_lights`（`:46`）<br>`tick_etc_gates`（`:119`）<br>`check_npc_scene_events`（`:145`） | 红绿灯 **4 相位**含 `FlashingGreen`；ETC 用硬编码距离且 **NPC 完全不响应**；互斥只检测不实现 |
| **场景加载** | [`include/scenario_loader.h`](file:///home/caixuf/code/FlowEngine/include/scenario_loader.h)<br>[`src/core/scenario_loader.c`](file:///home/caixuf/code/FlowEngine/src/core/scenario_loader.c) | `scenario_load` | 纯 JSON（cJSON）。23 个场景文件。**actor 溢出有 WARN，其他数组静默截断** |
| **OpenDRIVE 生成** | [`tools/json_to_xodr.py`](file:///home/caixuf/code/FlowEngine/tools/json_to_xodr.py) | `road_to_xml`（`:768`） | **Python 子进程**，运行时 `system()` 调用。车道生成对称双向（`lanes: 4` → 4+4）。车道线遵循 GB 5768 |
| **天气** | [`modules/adas_nodes/sensor_model_weather.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/sensor_model_weather.c) | `sensor_model_camera_visibility`<br>`sensor_model_weather_attenuation` | **全文件 25 行**。能见度比值 + `strstr` 地板 0.3。**无太阳角度模型，无天气-μ 耦合** |
| **仿真节点** | [`flowsim_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim_node.cpp) | `FlowSimTask::run`<br>`publish_vehicle_state`（`:1055`）<br>`convert_scenario_to_xodr`（`:324`）<br>`CONTROL_STALE_TIMEOUT_US`（`:75`，2 s） | 60 Hz，**墙钟钉死不能快进**。订阅仅 `control/cmd`。10 步主循环；`environment/state` 0.5 s、`road/geometry` 2.5 s |
| **测试** | [`test_entity_physics.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/test_entity_physics.cpp)<br>[`test_road_network.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/test_road_network.cpp) | ctest `entity_physics_tests`（17 用例）<br>ctest `flowsim_road_network` / `_cross` | 动力学/运动学/Pacejka 三套都覆盖。**但测试用 dt=0.05 而非生产的 1/60**；`npc_ai` 与碰撞无测试 |

---

## 12. 思考题

1. **让仿真跑得比实时快**：
   第 1.2 节指出当前实现墙钟钉死在 60 Hz，帧超时就是 0 睡眠硬追，没有子步进也没有时间缩放。想跑 10 小时里程就得等 10 小时。
   (a) 请设计一个时间缩放机制。这会连带影响三件事：消息总线的交互（自车指令来自别的节点）、红绿灯/编排的 `sim_time` 推进、以及发布频率。请分别分析每一件在「仿真快于实时」时会怎样失效。
   (b) 逐子步进（把一个 16.67 ms 的帧拆成 N 个更小的物理步直到追上墙钟）和单纯「让 dt 变大」两种加速方式，对第 4.5 节那个科氏耦合项的稳定性影响分别是什么？哪一个更安全？
   (c) 更根本的问题是：**加速仿真对「E2E 策略评估」到底有没有用？** 参照第 23 章世界模型那套论述——闭环仿真里最难的不是让时间流得快，而是让被测策略的行为改变其他交通参与者的反应。请论证：在这个仓库的架构下（NPC 逻辑全是本地启发式，不接受自车的动作输入），加速仿真能得到什么、得不到什么。

2. **修掉那个自车参数 bug，并推广这个思路**：
   第 4.9 节发现 `ego.drag_coeff = 0.3` 被 `apply_vehicle_defaults` 覆盖成 0.4，`ego.wheelbase` 同样被覆盖成 2.7——**场景配置的自车参数根本不进自车动力学**。
   (a) 修复有两种思路：把 `apply_vehicle_defaults` 改成「只填未设置的字段」（即尊重调用方的预赋值），或者把场景参数的覆盖挪到 `apply_vehicle_defaults` **之后**。哪一种更好？为什么？
   (b) 更重要的追问：**这个问题怎么系统性地避免？** 第 19 章的 `max_steer` 0.35 vs 0.22、第 17 章的 `CONTROL_DT_S` 与 40 Hz 循环不一致、这里的三处，是同一类病——**某个常量在多层各有一份，且某一层会静默覆盖另一层**。请设计一个通用机制（构建期？启动期？运行期？）来捕获这类不一致，并论证它在三种时机各自的可行性。
   (c) 具体地：如果要让仿真支持「长轴距车」场景（`ego.wheelbase = 3.5`），而控制节点从 `vehicle/state` 读到 3.5、自车动力学用 3.5——这需要改几处？请列出完整清单，并指出其中哪些改动会破坏现有的 17 个物理测试。

3. **把 NPC 做成反应式的**：
   第 5.1 节指出 NPC 逻辑全是本地启发式，`RoadPosition::advance` 沿路网推进，**完全不接收自车的动作**。这意味着闭环仿真里，自车撞上去和绕过去，NPC 的反应完全一样。
   (a) 请设计一个最小的反应式扩展：NPC 需要知道自车的什么信息？举三个例子说明「知道」和「不知道」会导致完全不同的场景（追尾、抢行、让行）。
   (b) 这和第 23 章里 X-World / Waymo World Model 的「动作条件化」是什么关系？Cosmos 3 报告区分了「预测式潜空间世界模型」和「生成式世界模型」两类——FlowSim 目前属于哪一类？它距离「动作条件化」差什么？
   (c) 一个反直觉的追问：第 5.8 节说 Poisson 动态车流会剔除 220 m 外的车、配额 24 辆。这意味着**车流的规模是固定的、有限的**。对于「评估策略在密集车流中的表现」，这种封顶会不会系统性地高估策略能力？如果会，该怎么量化这个偏差？

4. **天气模型的诚实性**：
   第 6.2 节指出 `sensor_model_weather.c` 只有 25 行，天气只是一个 `strstr` 子串测试加 0.3 地板；**没有太阳角度，没有天气-μ 耦合**；而且 `flowsim_node.cpp:2572` 比较的 `storm`/`sandstorm` 根本不在加载器白名单里。
   (a) 假设要做一个**最小可信**的天气模型，你需要哪些物理量？按「实现成本 / 对感知性能的影响」排序，说明哪几个是必须的、哪几个可以先不做。
   (b) 天气应该影响哪些下游？至少包括：相机可见度、激光雷达点云密度、**轮胎抓地力**、以及第 18 章安全层的 TTC 阈值。请说明为什么「雨天应该让安全阈值更保守」这件事，在当前架构里很难做对——是不是因为安全层的阈值是硬编码的（见第 18 章 9.2 节那 30+ 个字面量）？
   (c) 最后一个方法论问题：**一个知道自己的天气模型是 25 行查表法的仿真器，用它来评估感知算法的鲁棒性，结论有效吗？** 请给出一个判据——在什么条件下，你可以放心地说「这个策略在我这个仿真里通过了，所以它对天气是鲁棒的」；什么条件下这个推论是无效的。
