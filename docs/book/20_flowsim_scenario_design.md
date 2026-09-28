# 第 20 章：给算法造一个能反复练车的世界

> **v2 范式章节**（2026-09 整治后保留）。本章保留低行号密度、真技术书
> 风格，参照 `docs/book/README.md` 写作风格约束与 `docs/book/08_discovery.md`
> 范式示范。v1 行号清单版本归档于 `docs/_archive/book_v1/18_flowsim_scenario_design.md`。

算法上车之前，得先在仿真里跑够里程。可造出这样一个世界并不轻松：它既
要摆得出多车道、交叉路口和匝道汇入，还得让路上的 NPC（非玩家角色，即
场景中的交通车）看起来像真的在开车。

本章讲 FlowSim 怎么用一份 JSON 描述场景、路网怎么从直线拼成完整路线、
车辆动力学有哪三套模型（以及**默认用的是哪一套**），以及仿真时间和墙上
时间是怎么分开的。

先破除旧稿里几个会让人踩坑的误解 —— 这些不是措辞问题，是照着写代码会
出错的问题：

1. **默认不是动力学模型。** `physics_model` 默认 `"kinematic"`。动态
   模型和 Pacejka 都要显式开。
2. **NPC 永远用运动学模型。** `npc_ai` 无条件调
   `step_bicycle(npc, dt, throttle, brake, 0.0)` —— `steer` 传 0，它只
   被当作纵向速度积分器。动态模型是自车专属。
3. **dt 是 1/60 秒，不是 0.05。** 旧代码注释里「dt=0.05s（20Hz）」已
   经过时。真实值在 `flowsim_time.h`，60 Hz。
4. **仿真不能快于实时。** 没有子步进、没有时间缩放，帧超时就是 0 睡眠
   硬追。
5. **红绿灯有 4 个相位，不是 3 个。** 多一个 `FlashingGreen`（闪烁绿）。
6. **NPC 默认不变道。** MOBIL 完整实现了但 `enable_mobil` 默认
   `false`。
7. **`esmini_stub.cpp` 是全空桩，不是转换桥。** 旧稿说它解析
   OpenDRIVE —— 完全说反了。
8. **`edge.type` 不控制摩擦系数。** 旧稿那张表里的 μ 值在代码里不存
   在。

---

## 一、三个时钟：逻辑时间、墙上时间、实时时间

理解 FlowSim 的第一道门槛，是区分三个时钟：

`flowsim_time.h` 全文只有 7 行，但它自称「时间步唯一事实源」。**这条注
释本身就是一段事故记录** —— 曾经 `flowsim_node.cpp` 和 `scene_events.cpp`
各自 `#define` 了一个时间步，两份不一致，导致红绿灯编排时长差 3 倍。
这类 bug 极难查：现象是「红绿灯节奏不对」，根因却在两个文件对 `dt` 的
不同理解。

```
FLOWSIM_FREQUENCY_HZ  = 60.0
FLOWSIM_DT_SEC        = 1/60 ≈ 0.0167 s
FLOWSIM_DT_US         = 16666
```

| 时钟函数 | 仿真模式返回 | 用途 |
|---|---|---|
| `clock_now_us()` | **逻辑时间** `g_sim_time` | 物理 dt、红绿灯相位、编排、`sim/tick` 时间戳 |
| `clock_now_monotonic_wall_us()` | 真实 `CLOCK_MONOTONIC`（**永不被仿真影响**） | 帧耗时测量、sleep |
| `clock_now_realtime_us()` | `CLOCK_REREALTIME` | 绝对时间戳 |

逻辑时间在物理阶段的**最末尾**推进一格 —— 放在最末尾而不是开头，是为
了让本帧内所有读到 `clock_now_us()` 的代码（物理、碰撞、红灯、NPC AI）
看到同一个时间点。

### 为什么要用两个时钟测帧时间

自适应 sleep 必须用 `clock_now_monotonic_wall_us()` 而不是 `clock_now_us()`。
代码注释记录了踩过的坑：用逻辑时间测帧耗时，`t_frame_us` 恒等于步长，
sleep 永远是 0，节点会空转到 3000+ Hz 把消息总线打爆。

**这直接回答了一个常见误解：FlowSim 不能快于实时。**

- 没有子步进（帧超时不会补跑多个物理步）
- 没有时间缩放参数
- 没有无头批量模式

**想跑 10 小时仿真，就得等 10 小时。** 这一点在评估 FlowSim 能否替代
真实里程时非常关键 —— 想用 1 小时算力跑完 1 周里程的，要换设计。

---

## 二、场景描述：一份 JSON 能写什么

加载器在 `include/scenario_loader.h` + `src/core/scenario_loader.c`，
C 接口，`extern "C"`。**只支持 JSON**（走 cJSON），没有 XML / YAML /
二进制。

顶层键的完整清单：

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
| `stop_lines[]` | `id, x, y`（**见下文，是死数据**） |
| `construction_zones[]` | `id, x, y, length, width` |
| `lighting` | `day` / `night` / `dusk` |
| `weather` | 白名单：`rain, snow, overcast, fog, clear` |
| `visibility_m` | 仅当 `10.0 ≤ v ≤ 5000.0` 接受，默认 **1000.0** |
| `choreography` | `loop_period_s`（默认 45.0）+ `beats[]` |
| `map_file` / `route_file` | 地图引用 |

容量上限：actors 64、红绿灯 16、ETC 4、停止线 16、施工区 4、choreography
beats 16。

### 溢出的处理不一致

actor 溢出会 `LOG_WARN` 并明确报出丢了几个 —— 这是为了修一个具体 bug：
`straight_road.json` 的第 42 个 actor（唯一的行人）曾被静默丢弃。**但其
他数组（红绿灯、闸门、停止线、施工区、beats）的溢出是静默截断**。

写场景时如果 NPC 行为诡异，先数一数数组有没有超上限。

### 停止线：解析了但从不生成

`stop_lines` 被解析进 `ScenarioConfig.stop_lines`（上限 16），但
`populate_entities_from_scenario` **从不分配 `EntityType::StopLine`**。

`flowsim_node.cpp` 里 `StopLine` 的唯一出现是构建障碍物列表时的排除过
滤：`if (e.type == StopLine) continue;`。**全仓库没有 `stop_line_count`
消费者。** 旧稿说「FlowSim 生成停止线实体」是错的。

停止线目前在规划侧是**虚拟墙**（第 16 章 `st_graph.c` 里由红绿灯生成
S-T 禁行区），不需要实体。

---

## 三、路网：从 JSON 到 OpenDRIVE

### 真实的加载链路

```
map.json 或场景内联 road_network.edges
        │
        ▼  system("python3 tools/json_to_xodr.py <场景> -o <tmp>/flowsim_<名>.xodr")
OpenDRIVE .xodr 文件
        │
        ▼  esmini RoadManager（RM_* C API）
FlowRoadNetwork 封装
```

C 代码里不直接读 OpenDRIVE。FlowSim 用 `system()` 调一个 **Python 子进
程**做格式转换。转换失败 ⇒ `roads_loaded = false` ⇒ 仿真降级到世界坐
标兜底路径。

**这是一个运行时依赖 Python 解释器和文件系统可写性的设计。** 好处是换
地图格式只要改 Python 脚本；代价是场景加载路径上多了一个进程和一次文件
I/O。

另外：`esmini_stub.cpp` 是**全空桩** —— 每个 `RM_*` 调用都返回 `-1` /
`0`。没有 esmini 的构建里 `RM_Init` 返回 −1 ⇒ 加载失败 ⇒ 降级。旧稿说
`esmini_stub.cpp` 是「解析 OpenDRIVE 的转换桥」，恰好说反了。

### 车道生成是对称双向的

`tools/json_to_xodr.py` 的生成规则：

> center（参考线 width=0）+ right N 条顺向 + left N 条对向（双向道路）

**所以 `lanes: 4` 意味着每侧 4 条、共 8 条可行驶车道。** `scenarios/
straight_road.json` 写 `"lanes": 4` 而描述写「3000m 双向 4 车道」——
这个描述是含混的，实际生成的是 4+4。

OpenDRIVE 的车道 id 约定（`road_network.h`）：`0` = 参考线，**正 = 左，
负 = 右**。仓库注释指出 `json_to_xodr.py` 会把 `y<0 = 左车道, y>0 =
右车道` 归一化到 OpenDRIVE 语义。

### 关于 `edge.type`

旧稿给了一张表说 `edge.type` 决定摩擦系数（`highway` → μ=0.9、
`urban` → μ=0.8 等）并影响渲染分支。**μ 那一列在代码里不存在。**

真实情况：`road_network.edges[0].type` 被解析，但车辆动力学里的摩擦只
有两处来源 —— `pacejka_mu`（恒 0.7）和稳定性护栏里硬编码的 `0.8`。
**`edge.type` 与摩擦无关。**

`edge.type` 的真实用途是道路分类标签（`json_to_xodr.py` 用它决定几何
原语和车道线），以及前端的渲染分支选择 —— 后者在 `tools/flowboard/`
的 JS 里，不在 C++ 里。

### 三个核心抽象

| 类 | 职责 |
|---|---|
| `FlowRoadNetwork` | esmini `RM_*` 的薄封装。`frenet_to_world` / `world_to_frenet` / `speed_limit`（默认兜底 13.89 m/s = 50 km/h）/ `lane_width` / `detect_junctions`（聚类半径 15.0 m） |
| `RoadPosition` | **每车一个 esmini 位置句柄**，用 `RM_PositionMoveForward` 沿真实 OpenDRIVE 拓扑推进 |
| `Route` | 一条有序的 esmini road 链，按端点邻近 + 航向连续贪心构建（`tol=4.0`） |

**两个必须知道的坑：**

**其一，`frenet_to_world` 故意忽略 esmini 返回的航向。** 用两点弦差分
重算，因为 esmini 的位置句柄在 `RM_SetWorldXYHPosition` 之后会带着过期
的 heading 状态。探针实测：干净句柄 7.88°，被污染的句柄 239.5°。

**其二，`RoadPosition` 运行时必须用 `relocate()` 而不是 `init()`。** 注
释记录：`init()` 会 `RM_DeletePosition` + `RM_CreatePosition`，这会移动
esmini 内部的句柄数组，导致**所有 NPC 一起偏移约 200 米**。

还有个反直觉的细节（2026-08-14 核对 esmini 源码确认）：
`junctionSelectorAngle` 的语义是 `0.0` = 直行、`π/2` = **左**、`3π/2`
= **右**、`-1.0` = 随机。esmini 自己的注释（`pi=直行, pi/2=右`）是错
的，头文件里明确标了出来。

---

## 四、车辆动力学：三套模型

### 派发点：只有自车能选

```
if (physics_model == "dynamic")     step_bicycle_dynamic(ego, ...)
else if (physics_model == "pacejka") step_bicycle_dynamic_pacejka(ego, ...)
else                                  step_bicycle(ego, ...)           // kinematic（默认）
```

`g.physics_model` 默认 `"kinematic"`，从节点参数解析。

**NPC 走的是无条件路径：**

```c
step_bicycle(npc, dt, throttle, brake, 0.0);
```

`steer` 传 **0.0**，位置随后被 `RoadPosition::advance` 或 `frenet_to_world`
覆盖。**`step_bicycle` 在这里纯粹是个纵向速度积分器。**

### 运动学模型 `step_bicycle`

`step_bicycle` 的关键性质：**参考点是车身中心，不是后轴。**
`half_wb · yaw_rate` 那一项就是全部意义所在 —— 它把「中心的速度」和「后
轴的瞬时旋转中心」区分开。回归测试
`test_rear_axle_no_slip` 断言后轴横向滑移 < 0.2 m/s。

`yaw_rate` 在运动学模式下是**纯代数输出**，不是积分状态。

### 纵向模型（三套共用）

$$a = \frac{F_{\text{drive}} - F_{\text{brake}} - c_d\, v|v|}{m}$$

- 满油门 **5000 N**、满刹车 **8000 N**，都是硬编码；
- 空气阻力**不是** $\tfrac12\rho C_d A v^2$，而是没有那个 ½ 的
  $c_d v|v|$；
- **没有滚动阻力、没有变速箱、没有传动系**；
- **$v = 0$ 时刹车产生零力**（静摩擦），只有驱动力起作用。

这解释了为什么仿真里起步和刹停的时序总是和真车差一点 —— 它本质上是一
个一阶力平衡，没有传动系的迟滞。

### 转向执行器：一阶滞后 + 速率限制

正常行驶限幅 **0.25 rad**（≈14°），掉头时（`steer_override=true`）放宽
到 **0.60 rad**（≈34°）。宽路掉头打一圈多（~0.50 rad），窄路打死方向
盘（~0.55-0.60 rad），来自 Python 扫描结果。

一阶滞后 $\alpha = dt / (\tau + dt)$（`steer_tau = 0.15 s`）+ 速率限
制（`steer_rate_max = 0.6 rad/s`）+ 硬限幅。

**自车转向实际上经过了两级一阶滞后。** 物理步之前，节点还有一层 EPS
滤波：$\text{ego.steer} = 0.4 \cdot \text{raw} + 0.6 \cdot \text{prev\_steer}$。
所以完整链路是：0.4 系数滤波（60 Hz 下约 0.025 s 有效延迟）→ 物理层
的 τ=0.15 s 滞后。**仿真里转向比真车慢**，所以仿真里的转向包络与真车
不完全可比。

### Pacejka 96 与稳定性护栏

Pacejka 魔术公式给出：

$$F_y = \mu F_z \sin\!\Big(C \arctan\big(B\alpha - E(B\alpha - \arctan(B\alpha))\big)\Big)$$

参数对三种车型完全相同（写在 switch **之前**）：B=8.0, C=1.2, E=−0.2,
μ=0.7。**峰值侧向加速度 ≈ 0.7 g**。μ 是唯一的湿度/雪地调节旋钮，
而 FlowSim 本身**不会随天气改变它**（见下文）。

稳定性护栏记录的教训（PR #72，2026-07）：线性轮胎 + 滑移角硬饱和的组
合下，高速持续转向时 `v_y_body` 发散到 5×10⁴ m/s（20 m/s 以上，25 秒
左右）。护栏是必须的：

```
max_vy = |vx| · tan(SLIP_ANGLE_MAX) · 1.5
max_r  = (0.8 · 9.81) / max(vx, 1.0)
```

**这不是「模型无条件稳定」，而是「模型被钳住了」。** 任何关于收敛性的
说法都要带上这个前提。

值得注意的是护栏里的摩擦上限是**硬编码的 0.8**，而不是 `pacejka_mu`
（0.7）。两个模型用了不同的摩擦上限。

### 一个真实的 bug：场景配置的自车参数不生效

`flowsim_node.cpp` 早早设了 `ego.mass = 1500.0; ego.drag_coeff = 0.3;`
然后调 `apply_vehicle_defaults(ego)`。而 `ego.type == EntityType::Ego`
落在和 `Car` 同一个 `case` 分支里，那个分支**硬编码** `mass = 1500.0,
drag_coeff = 0.4`。

**所以前两行设的值两行后就被覆盖了。** `drag_coeff = 0.3` 是死代码。
同理，`wheelbase` / `length` / `width` 也被覆盖成 2.7 / 4.6 / 2.0。

**这意味着场景文件里 `ego.wheelbase` 根本不会反馈进自车动力学。** 它
只通过 `publish_vehicle_state` 广播给下游节点（控制节点会读它来标定），
但自车自己跑的还是硬编码的 2.7。

**这是和第 19 章那个 `max_steer` 0.35 vs 0.22 是同一类问题**：配置值
在某一层被静默忽略。如果你在场景里把轴距调成 3.5 m 想测试长轴距车的
行为，自车会按 2.7 跑，而控制节点以为轴距是 3.5 —— 两者不一致，会产生
本不该有的跟踪误差。

---

## 五、NPC 与场景事件

### NPC 不是 Pure Pursuit

`npc_ai.cpp`（1191 行）里**没有 pure-pursuit**。NPC 的横向位置由路网
强加，AI 只决定 `throttle / brake / offset`。

每帧流程：

1. `uturn_yield` —— 对向来车遇自车掉头时让行
2. 碰撞冷却处理 —— 速度 0、刹车 1，但**没有提前 return**
3. 横向控制：CutIn PID / LaneChange 插值 / 强制 `target_offset = offset`
4. MOBIL 评估 —— **默认关闭**
5. `find_lead` 同车道前车搜索
6. 状态机给出 `v_desired`
7. `v_desired` → 油门/刹车
8. `step_bicycle(npc, dt, throttle, brake, 0.0)` —— 只更新速度
9. 位置来自 `RoadPosition::advance` 或 `frenet_to_world`

**位置状态机**有 7 个状态：`Cruise / Follow / StopForTL / LaneChange /
CutIn / Stopped / Yield`。

### 一个「简化版 IDM」

$$d_{\text{safe}} = d_0 + T v$$

它不是教科书的 IDM：没有 $(\Delta v)^2$ 项，也没有 $s^*/s$ 比例。第
15 章讲的 CTG 跟车律和这里形式相同，但这是另一套独立实现。

油门/刹车转换里有**油门地板 0.2**，意味着 NPC 永远不会完全松油门——
这在跟停场景下会造成 NPC 缓慢爬行。**死锁逃逸**在 `gap < 2.0 && speed <
0.5` 时把 `gap` 伪造成 `10.0` 强制重新起步。

### MOBIL：实现了但默认关

`enable_mobil{false}`，由 `scenario.npc_lane_change` 驱动。注释说明了
关闭原因：「用户需求：演示场景中 NPC 各守其道不变道」。

**后果**：`NpcState::LaneChange` 在 MOBIL 关闭时从不被设置，所以
LaneChange 分支是事实上的死代码（代码自己在注释里承认了）。

**只有编排（choreography）驱动的 NPC 会变道**，走 CutIn PID。无条件安
全钳位把 `offset` 硬钳在 $\pm 0.3$ m，NPC 永不越过中心线进入对向。

### 红绿灯：4 个相位

```
T = green_s + yellow_s + red_s
tp = (sim_time_s + offset) mod T

if tp < steady_green_s   → Green
else if tp < green_s     → FlashingGreen      ← 闪烁绿，模拟国内路口的「绿灯闪烁提示即将变黄」
else if tp < green_s+yellow_s  → Yellow
else                       → Red
```

**闪烁绿**在绿灯末尾，模拟国内路口的「绿灯闪烁提示即将变黄」。

> `entity.h` 的注释写 `phase_state // 0=绿 1=黄 2=红` —— **这是错的**。
> 实际枚举有 4 个值。写代码时按注释理解相位会直接出错。

NPC 响应：**红灯或黄灯**都触发 `StopForTL`，刹车距离 $v^2/(2a_{max}) + 5$
m。**NPC 对 ETC 闸门完全无反应**（注释明说「ETC 闸门：NPC 不响应闸栏
杆」）。

### ETC 闸门：配置被忽略

`flowsim_node.cpp` 认真地把 `open_range_m` 存进了 `e.width`，但
`tick_etc_gates` 从不读它。`50.0` / `10.0` 是**字面量**。而且 `dist`
是 `gate.x - ego.x`（假设闸门在世界 X 轴上），**不是 Frenet 距离**——
在非东西向的道路上会直接错。

### 施工区：不是实体，是伪造的感知障碍

`construction_zones` 被解析（上限 4），**但不生成实体**。它在两处起作
用：

1. **作为伪造的感知障碍发布** —— 在 `front_x = cz->x − cz->length/2`
   排一行 `type="construction"` 的障碍，**围栏厚度 4 m**。这些障碍只
   存在于 JSON 载荷里，**不参与碰撞检测、不参与 NPC AI**。4 m 厚度正
   是第 16 章讲的「薄墙漏检」的对策。
2. **掉头前缘查询** —— 但这条路径**已经死了**。注释：「U-turn 触发
   已迁移到 behavior_planner → planning → control 链路。flowsim 只执行
   控制指令（含 gear），不再做掉头决策。」**掉头从仿真里移走了，这是
   对的** —— 掉头是决策问题，不是被控对象问题。

### 行人

`step_pedestrian` 纯粹是 `x += vx·dt; y += vy·dt`。到达边界后停下等
`ped_wait_time`（3 s）再反向。**行人不会避让自车，也不会被自车影响**
—— 它完全开环。

### 碰撞不是冲量响应

碰撞处理是：两车速度归零，沿 `route_s` **分离 2.0 m**，或用 AABB 近似
的最小平移向量。**没有冲量、没有恢复系数、没有质量加权。** 护栏碰撞
保留 0.3 倍速度，`crash_cooldown = 0.3 s`。

---

## 六、天气与光照：25 行代码

`modules/adas_nodes/sensor_model_weather.c` **整个文件 25 行**。`rain /
snow / fog` 都共用一个「散射地板 0.3」分支 —— 真正的相机散射模型没有。

`lighting` 字段影响前端渲染分支，不影响仿真器本身。

---

## 七、我们踩过的坑

**1. 60 Hz vs 20 Hz 的红绿灯编排事故。**
`flowsim_node.cpp` 和 `scene_events.cpp` 各自 `#define` 了一个时间步，
两份不一致，导致红绿灯编排时长差 3 倍。修复：抽出 `flowsim_time.h` 唯
一事实源。**经验之谈：常量分散在多个文件里总会漂移，物理参数必须有一
份唯一的源**。

**2. 用逻辑时间测帧耗时 = 3000 Hz 空转打爆消息总线。**
自适应 sleep 必须用墙上时间，否则 `t_frame_us` 恒等于步长，sleep 永远
是 0。**经验之谈：测帧时间必须用不被仿真影响的时钟**。

**3. 方向感知踩了世界系 dx 的坑（与第 18 章同源）。**
同车道跟车判据用世界系 `dx = obs_x − ego_x`，掉头返程同向防撞完全失
效。修复：沿车头方向投影 ahead。**同一个坐标系陷阱在感知侧和 TTC 侧
各暴露一次**，根因相同：用世界坐标判断「相对位置」。

**4. 施工区豁免是字符串精确匹配的（与第 18 章同源）。**
施工区感知点类型名 `obs_type == "construction"` 决定豁免，感知侧一旦
改名就静默失效，掉头立刻 40 s 超时。**经验之谈：跨模块契约用枚举而不
是字符串**。

**5. `ego.wheelbase` 被静默覆盖（与第 19 章 `max_steer` 同型）。**
场景文件里写的自车参数在 `apply_vehicle_defaults` 后被硬编码覆盖，下
游节点按配置来标定，自车按硬编码跑。**经验之谈：默认值要在最末位才
写，配置值要在最早位写；写反了就是静默覆盖**。

**6. dry-run 报「永远健康」**（与第 19 章同源）。
NPC AI 的某个状态机分支也命中了「不发帧就报健康」的反模式 —— 把「发
帧成功」与「帧计数」绑定后，dry-run 退化成「我健康但什么都没干」。

**7. 红绿灯 4 个相位被注释写成 3 个。**
`entity.h` 的 `phase_state` 注释与枚举不一致 —— 新人按注释理解会漏掉
`FlashingGreen`。**经验之谈：枚举与它的字符串表示要在同一处维护**。

**8. 仿真不能快进。**
想跑 10 小时仿真就得等 10 小时。**经验之谈：评估 FlowSim 能否替代真
实里程时，先想清楚仿真时间预算是否够**。如果需要加速，要换设计（比如
并行多场景，而不是改 FlowSim）。

---

## 八、与行业坐标

| | CARLA | LGSVL / SVL | esmini standalone | KunAutoDrive FlowSim |
|---|---|---|---|---|
| 物理模型 | UE4 PhysX | 内置 | 无 | kinematic / dynamic / Pacejka |
| 路网 | OpenDRIVE | OpenDRIVE | OpenDRIVE | JSON → Python → OpenDRIVE |
| NPC AI | SUMO 接入 | 简单规则 | 无 | 内置简化版 IDM + MOBIL（默认关）|
| 仿真加速 | 可调 | 可调 | 可调 | 不可 |
| 与上游耦合 | 弱（黑盒） | 弱（黑盒） | 强（同进程） | 强（同进程 + Pub/Sub）|

CARLA 与 LGSVL 走 UE 引擎绑定，物理来自商业引擎；esmini 是个独立路网
播放器，物理模型空缺。FlowSim 处于「中等保真度 + 与上游算法紧耦合」的
形态 —— 这意味着物理不是黑盒，可以为算法定制（掉头、施工区），代价是
不能像 CARLA 那样独立演进。

---

## 九、本章不写什么

- **不再讨论场景 JSON 的完整 schema 校验** —— `tools/scenarioctl.py
  validate` 是权威工具，本书只讲它检查不到的语义陷阱。
- **不再讨论 esmini 的 OpenDRIVE 实现细节** —— esmini 自己的文档是更
  权威的来源。
- **不再重复具体 NPC 跟车参数** —— 默认值在 `NpcAiConfig` 里，本书
  只讲默认值的选择原则。