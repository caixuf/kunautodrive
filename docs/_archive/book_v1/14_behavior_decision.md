<!-- @archive-banner -->

> **归档说明**：本章是 docs/book v1（2026-09 前后"按源码逐条重建"产物）的版本。
> 内容大量绑定代码行号、文件名、函数符号，与代码强耦合 —— 维护成本高且易过时。
> v2 重写计划：见 `docs/book/README.md` 的写作风格约束 + 真技术书范式。
> 本归档文件保留供历史参考；引用时用 `docs/_archive/book_v1/` 而非 `docs/book/`。

# 第 15 章：行为决策 —— 跟车、变道，还是掉头

> **本章导读**：
> 前面有辆车挡住了你。你脑子里其实同时转着两个问题——**走不走**，和**往哪走**。前者是离散的（跟车、变道、让行、掉头，选项就那么几个），后者才是连续的（一旦决定变道，方向盘得画出一条平滑曲线过去）。自动驾驶把这两件事拆成上下两层，上层只管「选哪一个」，下层管「怎么走过去」。
>
> 本章讲上层：[`modules/adas_nodes/behavior_planner_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/behavior_planner_node.cpp)（1940 行，20 Hz）。它是整条链路上**唯一一个纯离散决策器**——输入是「我在第几车道、前车多远、邻道空不空、前方有没有红灯」，输出是「现在做哪一件事」。
>
> 先破除旧稿里几处需要修正的描述：
>
> 1. **8 个状态里有 3 个是不可达的。** `STOP` / `YIELD` / `EMERGENCY` 从来不是任何转移规则的**目标**——它们只是初始值。实际能到达的只有 **5 个**：`CRUISE` / `FOLLOW` / `LEFT_CHANGE` / `RIGHT_CHANGE` / `U_TURN`。
> 2. **状态 ID 不是 `SM_EVENT_USER_BASE + n`。** 注释这么说（`:46-47`），但值是硬编码的 `200..207`，而 `SM_EVENT_USER_BASE` 是 **16**。
> 3. **没有 RSS（责任敏感安全模型）。** 后向安全用的是一条纯运动学的判据 `max(rear_safe_min_m, v_rear · rear_safe_time_s)`，没有那个四分式。
> 4. **`evaluate_noa_navigation()` 这个函数不存在。** 导航触发变道不在决策节点，在规划节点里。
> 5. **「变道冷却 5 秒」也不准确**——`lane_change_cooldown_s` 默认 3.0 s；30 s 那个是掉头专用的（`cooldown × 6`）。

---

## 1. 位置：整条链路的「翻译层」

### 1.1 话题契约

订阅八路（`behavior_planner_node.cpp:1858-1865`）：

| 回调 | 话题 | 提供什么 |
|---|---|---|
| `on_fusion` | `fusion/localization` | 自车 x/y、航向、速度 |
| `on_vehicle_state` | `vehicle/state` | 车速、挡位 |
| `on_tracked_objects` | `perception/tracked_objects` | 带关联 ID 的跟踪目标 |
| `on_raw_obstacles` | `perception/obstacles` | 原始障碍物点 |
| `on_road_geometry` | `road/geometry` | 车道数、车道宽 |
| `on_traffic_lights` | `road/traffic_lights` | 信号灯相位 |
| `on_ref_path` | `road/ref_path` | 参考线折线 |
| `on_scene_frame` | `scene/frame` | 施工区 |

发布两路：

| 话题 | 内容 | 频率 |
|---|---|---|
| `planning/behavior` | 二进制 `Behavior`（22 B） | 20 Hz |
| `behavior/state` | 原始 JSON 文本（**未类型化**） | 0.4 Hz |

**输出的 `Behavior` 消息只有 22 字节**（[`msg/adas_msgs.msg:228-235`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg)），因为它只携带「离散决策」本身：

```
enum BehaviorCommand {          # 8 个取值
    BEH_CRUISE       = 0
    BEH_FOLLOW       = 1
    BEH_LEFT_CHANGE  = 2
    BEH_RIGHT_CHANGE = 3
    BEH_STOP         = 4
    BEH_YIELD        = 5
    BEH_EMERGENCY    = 6
    BEH_U_TURN       = 7
}
```

这个设计的意义：**决策器到规划器之间没有任何连续量的传递**。规划器拿到的只有「你要变道，目标是右边」这一类离散意图，具体怎么变、变多快，是规划器自己的事。

> 状态→命令的映射在 `beh_state_to_cmd`（`:281-290`）。它能返回 `BEH_STOP` / `BEH_YIELD` / `BEH_EMERGENCY`——**但由于这三个状态不可达，这三个命令也永远发不出去**。

### 1.2 频率是编译进去的

```cpp
LOG_INFO("behavior", "FlowCoro behavior planner started (20Hz)");   /* :685 */
```

而所有定时器累加的都是**硬编码的 0.05**（`:722-724`）：

```cpp
g.state_timer    += 0.05;
if (g.cooldown > 0.0) g.cooldown -= 0.05;
if (g.uturn_cooldown > 0.0) g.uturn_cooldown -= 0.05;
```

自适应 sleep（`:1725-1729`）：

```cpp
uint64_t t_frame_us = clock_now_us() - t_start;
uint64_t sleep_us_val = (t_frame_us < 50000) ? (50000 - t_frame_us) : 0;
if (should_stop()) break;
co_await sleep_us(sleep_us_val);
```

> **20 Hz 不是配置来的。** `config/pipeline.json:196-219` 的 `behavior_planner` 块**根本没有 `params` 键**。数字 `20.0` 只在 `discovery_advertise`（`:1875`）里作为服务发现的心跳周期出现过。
>
> 想改频率只能改代码重编译。这与第 3 章讲的一致：`config_manager` 把 `params` 解析进 `g.<字段>`，而这里压根没传。

---

## 2. 状态机：8 个状态，5 个活着

### 2.1 `ReflectiveStateMachine` 的「反射」是什么

[`include/state_machine.h:121-156`](file:///home/caixuf/code/FlowEngine/include/state_machine.h) 定义的 `ReflectiveStateMachine` 有 20 多个字段，核心是三组：

```c
121: typedef struct {
123:     StateId current; StateId previous; EventId last_event; uint64_t entered_at_us;
129:     TransitionRecord history[SM_HISTORY_DEPTH]; uint32_t history_head, history_count;  /* 环形缓冲，深度 8 */
134:     const TransitionRule* static_table; int static_size;
136:     TransitionRule* dynamic_rules; int dynamic_count, dynamic_cap;
141:     StateAction on_entry, on_exit; TransitionGuard guard;
146:     StateDebugHook debug_hook; bool trace_enabled; SmIllegalPolicy illegal_policy;
154:     char task_name_buf[64]; const char* task_name;
```

**「反射」指的是「用同一张表同时驱动转移和自省」，不是「订阅消息」。** 转移引擎 `find_transition`（[`src/core/state_machine.c:256-277`](file:///home/caixuf/code/FlowEngine/src/core/state_machine.c)）扫同一张表两遍（先非 `is_auto`，后 `is_auto`），而查询 API——`statem_can_transition`、`statem_allowed_events`、`statem_state_name`、`statem_all_states`——读的是同一张表。

决策节点真正用到的反射有三处：

1. **`statem_allowed_events`**（`:1712`）→ 打进 `[SM] state=... allowed=[...]` 日志（每 50 帧一次）。这是**运行时自省当前状态能接受哪些事件**——排障时非常有用。
2. **历史环形缓冲**（`:1682-1696`）→ 每 50 帧导出到 JSON。
3. **`statem_state_name`**（`:1463`）→ 状态名转字符串。

`trace_enabled = true`（`:1791`）让 `state_machine.c:354-359` 每次转移都 `LOG_INFO`。加上 `beh_debug_hook`（`:322-328`）打 `[BEH] <rule_desc>`，**每次状态转移在日志里都有两条记录**。

> `statem_add_transition` / `statem_remove_transition`（头文件 `:347, 356`）提供了运行时扩展能力，**决策节点从不用**——只用静态表。

### 2.2 8 个状态，19 条规则，3 个死状态

`behavior_planner_node.cpp:49-58` 的状态枚举：

```cpp
49: enum BehState {
50:     BEH_ST_CRUISE       = 200,
51:     BEH_ST_FOLLOW       = 201,
52:     BEH_ST_LEFT_CHANGE  = 202,
53:     BEH_ST_RIGHT_CHANGE = 203,
54:     BEH_ST_STOP         = 204,
55:     BEH_ST_YIELD        = 205,
56:     BEH_ST_EMERGENCY    = 206,
57:     BEH_ST_U_TURN       = 207,
58: };
```

事件枚举（`:60-68`）：

```cpp
60: enum BehEvent {
61:     BEH_EV_BLOCKED        = 200,  /* 本车道有前车且暂无变道条件 */
62:     BEH_EV_LOST_LEAD      = 201,  /* 前车消失或间距充足 */
63:     BEH_EV_OVERTAKE_LEFT  = 202,
64:     BEH_EV_OVERTAKE_RIGHT = 203,
65:     BEH_EV_COMPLETED      = 204,
66:     BEH_EV_TIMEOUT        = 205,
67:     BEH_EV_UTURN_TRIGGER  = 206,
68: };
```

> **注意状态和事件共用同一组数字。** `BEH_ST_LEFT_CHANGE == BEH_EV_BLOCKED == 200`。无害——它们是 `find_transition` 的两个独立参数——但**不能当成一个枚举来讲**。

转移表在 `:71-105`，**19 条规则**：

| # | from | event | to | 行 |
|---|---|---|---|---|
| 1 | CRUISE | BLOCKED | FOLLOW | `:73` |
| 2 | CRUISE | OVERTAKE_LEFT | LEFT_CHANGE | `:75` |
| 3 | CRUISE | OVERTAKE_RIGHT | RIGHT_CHANGE | `:76` |
| 4 | FOLLOW | LOST_LEAD | CRUISE | `:78` |
| 5 | FOLLOW | OVERTAKE_LEFT | LEFT_CHANGE | `:80` |
| 6 | FOLLOW | OVERTAKE_RIGHT | RIGHT_CHANGE | `:81` |
| 7 | LEFT_CHANGE | COMPLETED | CRUISE | `:83` |
| 8 | LEFT_CHANGE | TIMEOUT | CRUISE | `:84` |
| 9 | RIGHT_CHANGE | COMPLETED | CRUISE | `:85` |
| 10 | RIGHT_CHANGE | TIMEOUT | CRUISE | `:86` |
| 11 | CRUISE | UTURN_TRIGGER | U_TURN | `:88` |
| 12 | FOLLOW | UTURN_TRIGGER | U_TURN | `:92` |
| 13 | STOP | UTURN_TRIGGER | U_TURN | `:93` |
| 14 | YIELD | UTURN_TRIGGER | U_TURN | `:94` |
| 15 | LEFT_CHANGE | UTURN_TRIGGER | U_TURN | `:99` |
| 16 | RIGHT_CHANGE | UTURN_TRIGGER | U_TURN | `:100` |
| 17 | U_TURN | COMPLETED | CRUISE | `:102` |
| 18 | U_TURN | TIMEOUT | CRUISE | `:103` |
| — | | | `TRANSITION_TABLE_END` | `:104` |

`static_size` 由 `state_machine.c:233-237` 数到 `from == -1` 为止。

> [!IMPORTANT]
> **8 个状态里 3 个不可达。**
>
> 规则 13、14 用了 `STOP` 和 `YIELD` 作为 `from`，但**没有任何规则的 `to` 是它们**。`EMERGENCY`（206）更彻底——它连一条规则都没有。
>
> `beh_state_to_cmd`（`:281-290`）和 `beh_state_str`（`:293-305`）能返回它们，但事件永远不会被设成能到达它们的值。`grep BEH_ST_STOP` 在整个文件里只出现在 `:54`（定义）、`:93`（作 from）、`:285`、`:299`（字符串映射）。
>
> **所以真实可达的状态只有 5 个**：`CRUISE` / `FOLLOW` / `LEFT_CHANGE` / `RIGHT_CHANGE` / `U_TURN`。讲这一章时如果说「8 态状态机」，得同时说明 3 个是占位。

### 2.3 实际能走通的五条路径

```
                    ┌──────────────────────────────────────────┐
                    │                                          │
   [CRUISE] ──BLOCKED──► [FOLLOW] ──LOST_LEAD──► [CRUISE]       │
       │                    │                                  │
       │                    │                                  │
  OVERTAKE_LEFT      OVERTAKE_LEFT/RIGHT              UTURN_TRIGGER
  /OVERTAKE_RIGHT          │                                  │
       │                    ▼                                  │
       │            [LEFT/RIGHT_CHANGE]                       │
       │              │            │                          │
       │         COMPLETED / TIMEOUT                           │
       │              │            │                          │
       └──────────────┴────────────┴──────► [CRUISE]           │
                                                │              │
                                          UTURN_TRIGGER        │
                                                ▼              │
                                            [U_TURN] ───────────┘
                                          COMPLETED/TIMEOUT
```

**注意变道完成后回的是 `CRUISE` 而不是 `FOLLOW`。** 这意味着变道结束后车会重新评估一次前车——如果变道是为了超车，那新车道上的前车仍会被检测到，下一拍就发 `BLOCKED` 回 `FOLLOW`。这个「多走一拍」的设计避免了状态在变道期间累积复杂上下文。

---

## 3. 跟车律：常量时距

### 3.1 前车检测

`:780-827`：

```cpp
780: double lead_lat_tol = lw * 0.5 + 0.6;
781: double best_gap = 1e9;
782: double lead_speed = g.target_speed;
...
796: if (has_ego_projection && project_to_ref(g.obs_x[i], g.obs_y[i], obs_projection)) {
798:     if (!projection_near_road(..., lc, lw)) continue;
803:     along = obs_projection.s - ego_projection.s;
804:     lat = std::fabs(obs_projection.d - current_lane_d);
805: } else {   /* 兜底：车头坐标系投影 */
808:     along = rx * fwd_x + ry * fwd_y;
809:     lat   = std::fabs(-rx * fwd_y + ry * fwd_x);
810: }
811: if (lat > lead_lat_tol) continue;
812: if (along < 0.0) continue;
813: if (along < best_gap) { best_gap = along; ... }
823:     lead_speed = g.obs_vx[i] * fwd_x + g.obs_vy[i] * fwd_y;
```

三个设计要点：

- **横向容差 `lw*0.5 + 0.6`**——车道宽的一半加 0.6 m 余量。在 3.5 m 车道上是 **2.35 m**。
- **优先用参考线投影**（Frenet s/d），投影失败才退回车头坐标系。`projection_near_road`（`:623-629`）的判据是 `residual <= lane_count * lane_width * 0.5 + 2.0`。
- **前车速度投影到车头方向**（`:823`）而不是直接用 `obs_vx`——这样倒车时（速度为负）符号才正确。

### 3.2 CTG 公式

`:829-851`：

```cpp
832: double desired_gap = g.acc_standoff + g.acc_time_headway * g.ego_v;
833: double follow_speed = lead_speed;
...
840: const double kFollowMaxRange = 200.0;
841: if (best_gap < 1e8 && best_gap <= kFollowMaxRange) {
842:     double gap_err = best_gap - desired_gap;
843:     if (gap_err >  g.acc_gap_err_clamp) gap_err =  g.acc_gap_err_clamp;
844:     if (gap_err < -g.acc_gap_err_clamp) gap_err = -g.acc_gap_err_clamp;
845:     follow_speed = lead_speed + g.acc_k_gap * gap_err;
846:     if (follow_speed < 0.0) follow_speed = 0.0;
850:     if (follow_speed > g.cfg_cruise_speed) follow_speed = g.cfg_cruise_speed;
851: }
```

$$d_{desired} = d_0 + T \cdot v_{ego}$$

$$v_{follow} = \text{clamp}\Big(0,\ v_{cruise},\ v_{lead} + k_{gap}\cdot\text{clamp}\big(\Delta - d_{desired},\ \pm c\big)\Big)$$

| 参数 | 默认 | 范围 | 物理含义 |
|---|---|---|---|
| `acc_standoff` | **5.0 m** | 0.5–20.0 | 静止最小间距 |
| `acc_time_headway` | **1.5 s** | 0.5–4.0 | 时距 |
| `acc_k_gap` | **0.4 s⁻¹** | 0.0–2.0 | 间距误差→速度的比例增益 |
| `acc_gap_err_clamp` | **8.0 m/s** | 1.0–30.0 | 误差限幅 |

**为什么是 CTG 而不是固定距离**：固定距离（比如 30 m）在低速时太远、高速时太近。CTG 让**时间**而非空间恒定——20 m/s 时 `d = 5 + 30 = 35 m`，2 m/s 时 `d = 5 + 3 = 8 m`。而 `k_gap` 是比例控制：`Δ = 0` 时 `v_follow = v_lead`，**车会自己收敛到期望间距**。

**两个限幅各有必要**：

- **`acc_gap_err_clamp = 8.0 m/s`**：前车突然刹停时 `Δ` 可能到 −35 m，不限幅的话 `k_gap × (−35) = −14 m/s`，指令速度会变成负数。限幅后最多是 `v_lead − 3.2 m/s`。
- **`kFollowMaxRange = 200.0`**：超过 200 m 的「前车」不参与跟车。远处的车用不着跟。

> **稳态性质**：当 `Δ = 0` 时 `follow_speed = lead_speed`，而 PID（第 17 章）去追这个速度，所以**间距会渐近收敛到 `d_0 + T·v`**。这不是精确的证明——真实收敛速度取决于下游 PID 的增益——但方向是对的。

### 3.3 「被堵住」和「值得超」

`:857-873`：

```cpp
857: double blocked_range = fmax(g.blocked_range_min, desired_gap * g.blocked_range_mult);
861: bool in_follow = (statem_current(&g.sm) == BEH_ST_FOLLOW);
862: bool blocked = (best_gap < (in_follow ? blocked_range * g.follow_hysteresis : blocked_range));
863: double rel_speed = g.ego_v - lead_speed;
864: if (rel_speed < 0.0) rel_speed = 0.0;
865: double min_gap = g.min_overtake_gap_base + rel_speed * g.min_overtake_gap_speed_mult;
866: if (min_gap > g.min_overtake_gap_cap) min_gap = g.min_overtake_gap_cap;
873: bool worthwhile = blocked && (best_gap > min_gap);
```

三个判据：

**判据一：被堵住**（`blocked`）

$$r_{blocked} = \max\big(r_{min},\ k \cdot d_{desired}\big)$$

`blocked_range_min = 30.0 m`，`blocked_range_mult = 3.5`。在 20 m/s 时 `d_desired = 35`，`r = 122.5 m`——**122 米内有车就算堵**。这不叫「被堵住」，这叫「进入跟车模式」。所以名字有点误导，实际语义更接近「**进入跟车状态的触发距离**」。

**判据一之半：迟滞**（`follow_hysteresis = 1.3`）

```cpp
bool blocked = (best_gap < (in_follow ? blocked_range * 1.3 : blocked_range));
```

**已经在 FOLLOW 时，阈值放大 1.3 倍。** 这是标准的迟滞设计：进入用短阈值（122.5 m），退出用长阈值（159 m）。没有它，车会在 122.5 m 这个边界上反复横跳。

**判据二：最小超车间距**

$$d_{min} = \min\big(d_{cap},\ d_{base} + (v_{ego} - v_{lead})_{+} \cdot k_{rel}\big)$$

`min_overtake_gap_base = 25.0 m`，`min_overtake_gap_speed_mult = 2.0`，`min_overtake_gap_cap = 90.0 m`。

**这个公式的含义是：相对速度越大，需要的跟车距离越大。** 20 m/s 巡航、10 m/s 的前车，相对 10 m/s ⇒ `d_min = 25 + 20 = 45 m`。上限 90 m 防止高速时把阈值推到不可达。

**判据三：值得超**（`worthwhile`）

```cpp
bool worthwhile = blocked && (best_gap > min_gap);
```

**注意这是 AND。** 间距比 `d_min` 还小的时候**不许超**——那时候超过去就是贴着前车走，比跟着更危险。

---

## 4. 变道：三条件与门

### 4.1 三个条件的与

`:1029-1030`：

```cpp
1029: bool left_ok  = left_same_side && left_rear_safe && (left_gap > min_gap * g.lc_gap_mult);
1030: bool right_ok = right_same_side && right_rear_safe && (right_gap > min_gap * g.lc_gap_mult);
```

| 条件 | 含义 |
|---|---|
| `*_same_side` | 邻道是**同向**的（`first_legal_lane` 判据） |
| `*_rear_safe` | 后方来车不构成威胁 |
| `gap > min_gap × 1.5` | 前方空隙比最小超车间距再宽 50% |

`lc_gap_mult = 1.5`（`:240`）。**变道要求的前方空隙比跟车时更严**——跟车只要 `d_min`，变道要 `1.5 d_min`。这个 1.5 倍的余量给了轨迹插值时间。

### 4.2 后向安全：不是 RSS

**这是旧稿最需要修正的地方。** 旧稿写了一个四分式的责任敏感安全模型（RSS），代码里没有。

真实的判据在 `:965`（左）和 `:1019-1022`（右）：

**左侧**：
```cpp
rrs > 0 ? max(rear_safe_min_m, rrs * rear_safe_time_s) : rear_safe_min_m
```

**右侧**：
```cpp
max(min_gap, rear_safe_min_m)
```

| 参数 | 默认 | 范围 |
|---|---|---|
| `rear_safe_min_m` | **15.0 m** | 5.0–50.0 |
| `rear_safe_time_s` | **3.0 s** | 1.0–8.0 |

**左右不对称**：左侧用**速度 × 时距**（真正的运动学判据），右侧用**固定的 `min_gap`**。注释（`:1013-1018`）说这是有意为之，为了并线安全。

> **这个不对称值得注意。** 右侧的判据不随后车速度增长，所以一辆 25 m/s 逼近的后车，只要距离 ≥ `min_gap`（最高 90 m）就允许右变道。左侧更严格。反过来说，**如果后车很快，右侧的判据会给出「可以变道」的结论**——这依赖规划器和控制器的横向包络去兜底。
>
> 这不是 bug（右变道通常是往慢车道变，后车速度差小），但它是一个不对称的物理假设，值得记住。

### 4.3 双向道路守卫

`:1034-1048` 用 `planning_coord::first_legal_lane(lc, road_oneway)` 算出最内侧合法车道：

```cpp
// planning_coordinates.h:22-24
inline int first_legal_lane(int lane_count, bool road_oneway) {
    return road_oneway ? 0 : lane_count / 2;
}
```

**双向道路时 `lane_count/2` 以下的车道是对向车道，禁止进入。** 4 车道时 `first_legal = 2`，所以只有索引 2、3 可用（`nearest_own_lane` 也用同一下界）。

「误入对向车道」的恢复（`:1042-1047`）只允许移到**最近的合法车道**，并把两个 gap 都设成 `1e9`（禁用超车判断）。

### 4.4 红绿灯门控

`:643-671`：

```cpp
643: static bool carriageway_ahead_stop_light(void) {
644:     if (!g.has_traffic_lights || g.tl_count <= 0) return false;
645:     double v = g.ego_v; if (v < 0.0) v = 0.0;
650:     double stop_range = v * v / 8.0 + 3.0 + 20.0;
651:     if (stop_range < 60.0) stop_range = 60.0;
652:     for (int i = 0; i < g.tl_count; i++) {
653:         if (g.tl_state[i] == TL_GREEN || g.tl_state[i] == TL_FLASHING_GREEN) continue;
655:         if (!traffic_light_controls_direction(g.tl_lane_offset[i], g.on_return)) continue;
...
667:         if (dx <= 0.0 || dx > stop_range) continue;
668:         return true;
```

$$r_{stop} = \max\Big(60,\ \frac{v^2}{8} + 23\Big)\ \text{m}$$

**这个函数只做门控，不停车。** 返回 true 的含义是「**不许发起变道或并线**」，理由是「前方要停车了，变过去会被迫急刹」。

- 刹车距离用 `v²/8`——**隐含减速度 4 m/s²**（$a = v^2/(2d)$ ⇒ $a = 8/2 = 4$）
- `+3.0` 是反应时间对应的距离（按 0.3 s 算）
- `+20.0` 是余量
- 下限 60 m

绿灯**和闪烁绿**都算通行（`:653`），所以第 20 章讲的四相位里，「绿灯即将变黄」的那段闪烁期**不阻止变道**。

`traffic_light_controls_direction`（`include/traffic_light.h:122-126`）：`|lane_offset| < 0.25` 说明是本车道灯（两个方向都管），否则按 `on_return` 判断左右。

> **停车本身不在这一层。** 红灯的实际制动发生在规划节点的 `st_graph_plan` 里——那里把红绿灯变成 S-T 图上的**虚拟墙**（`planning_node.cpp:2484` 的 `wall_margin = 5.0`）。决策器只负责「别变道」，规划器负责「别撞墙」。

### 4.5 施工区：只影响掉头

施工区在决策节点里**只用于缩短掉头触发点**（`:1154-1161`）：

```cpp
1154: for (int i = 0; i < g.cz_count; ++i) {
1155:     const double front = construction_zone_front_x(g.cz_x[i], g.cz_len[i]);
1156:     if (front <= g.ego_x || front >= uturn_ref_x) continue;
1158:     const double half_w = 0.5 * g.cz_wid[i] + 1.0;
1159:     if (std::fabs(g.ego_y - g.cz_y[i]) > half_w) continue;
1160:     uturn_ref_x = front;
1161: }
```

`construction_zone_front_x(cx, len) = cx - 0.5*len`（`construction_zones.h:51-54`）。逻辑是「如果施工区前沿比路端更近，就把掉头触发点提前到施工区前沿」。

> **施工区对跟车和变道零影响。** 「按施工区限速」这件事在规划节点（`planning_node.cpp:623` 的 `construction_forward_space`）。

### 4.6 一个真实的不一致：冷却没有覆盖所有路径

三条发起变道的路径：

| 路径 | 行 | 有 `g.cooldown <= 0` 检查？ |
|---|---|---|
| CRUISE → 超车 | `:1316-1317` | **❌ 没有** |
| FOLLOW → 超车 | `:1375-1377` | ✅ 有 |
| 并线归位（CRUISE） | `:1334-1337` | ✅ 有 |

CRUISE→超车的那条分支：

```cpp
1316: if (worthwhile && adj_idx >= 0 && !carriageway_ahead_stop_light()) {
1317:     ev = (left_ok ? BEH_EV_OVERTAKE_LEFT : BEH_EV_OVERTAKE_RIGHT);
```

**漏掉了冷却检查。** 后果：在 CRUISE 状态下，变道完成 → 回 CRUISE（`state_timer` 归零、冷却 3 s 递减中）→ 若此时仍 `worthwhile`，**可以立刻再发起一次变道**。

而变道完成后回的是 CRUISE 不是 FOLLOW（见 2.3），所以这条路径确实会被走到。

> 这就是「变道画龙」的一个可能来源。旧稿说加冷却就解决了问题——**冷却确实有**（`lane_change_cooldown_s = 3.0`，`:1841`），但 CRUISE 这条路径绕过了它。

---

## 5. 掉头：一段专门的逻辑

掉头是这个节点里最复杂的部分，占了 `:1086-1268` 加上完成判定和冷却，约 200 行。

### 5.1 触发条件

三个必要条件：

**条件一：东西向路线**（`:1102-1105`）

```cpp
1102: bool route_ew = std::fabs(ref_x[last] - ref_x[0]) > std::fabs(ref_y[last] - ref_y[0]);
1105: if (!route_ew) { /* 不做掉头 */ }
```

**只有东西向占优的路线才做掉头。** 这是个务实的简化——掉头轨迹的生成（第 16 章讲的规划节点 `:2084-2196`）是按 X 轴方向硬编码的。

**条件二：必须在内侧车道**（`:1077-1078`）

```cpp
1077: const int inner_lane = lc / 2;
1078: const bool at_inner_lane = (current_idx <= inner_lane);
```

4 车道时 `inner_lane = 2`，所以**只能在索引 0、1、2 上发起**。如果车已经在索引 3（最外侧），要变两次道才能掉头。

**条件三：位置 + 速度**（`:1220-1229`）

```cpp
1220: } else if (g.ego_x > uturn_ref_x - 30.0 && g.ego_v <= 5.0) {
1222: } else if (g.ego_x > road_end_x - g.uturn_approach_dist_m && g.ego_v <= g.uturn_max_trigger_speed) {
```

两条触发路径：

| 路径 | 条件 | 参数 |
|---|---|---|
| 接近路端 | `ego_x > road_end_x - 120.0` 且 `ego_v ≤ uturn_max_trigger_speed`（5.0） | `uturn_approach_dist_m = 120.0`，`uturn_max_trigger_speed = 5.0` |
| 兜底 | `ego_x > uturn_ref_x - 30.0` 且 `ego_v ≤ 5.0` | **30.0 和 5.0 都是硬编码** |

**掉头必须低速触发。** 120 米是**减速区**而非「到点才触发」——车在 120 m 外就开始为掉头做准备。

### 5.2 返程的不对称

返程（`on_return = true`）的判据完全不同：

```cpp
1253: } else if (g.ego_x < 30.0 && g.ego_v <= 7.0) {
```

| | 去程 | 返程 |
|---|---|---|
| 位置 | `ego_x > road_end_x - 120` | `ego_x < 30.0` |
| 速度门限 | **5.0** m/s | **7.0** m/s |
| 区域大小 | 3 × 120 = **360 m** | 3 × approach |

**返程的速度门限是 7.0 而不是 5.0**，而参数 `uturn_max_trigger_speed` 默认是 5.0。这个 7.0 是**硬编码的**——注释没有解释为什么。

### 5.3 自然速度剖面

`:1168-1180`：

```cpp
1169: const double a_dec = 1.0;  /* 硬编码 */
     v_env = sqrt(2.0 * a_dec * (dist_ref - approach) + v_trigger * v_trigger);
     v_env = std::min(v_env, g.cfg_cruise_speed);
```

$$v_{env} = \sqrt{2 a_{dec}\,(d_{ref} - d_{approach}) + v_{trigger}^2}$$

这是「**按最舒适减速度能在触发点前停下所需的速度**」的反解。用 1.0 m/s² 的舒适减速度。

**它的作用是限制掉头期间的速度上限。** 车在 360 m 的掉头区域里，速度不能超过「还能刹住」的包络。

### 5.4 完成判定与冷却

完成（`:1493-1495`）：

```cpp
1493: const double uturn_target_h = g.uturn_entry_on_return ? 0.0 : M_PI;
1495: if (std::fabs(std::fabs(hn) - uturn_target_h) < 0.15) ev = BEH_EV_COMPLETED;
```

**看航向角**：`|-|h| - π| < 0.15 rad`（去程目标是 π，返程目标是 0）。容差 0.15 rad ≈ 8.6°。

冷却（`:1502-1503` 和 `:1514-1515`）：

```cpp
1502: g.uturn_cooldown =
1503:     g.lane_change_cooldown_timeout_s * 6.0;  /* 30s */
```

用的是 **`lane_change_cooldown_timeout_s`（5.0 s，超时后冷却），不是 `lane_change_cooldown_s`（3.0 s，变道完成冷却）**。`5.0 × 6.0 = 30 s`，与注释一致。

> **那个 `× 6.0` 是硬编码的乘数**，而且它和被复用的那个参数**语义不对齐**：这个乘数的意图显然是「掉头是变道的 6 倍耗时，所以冷却也放大 6 倍」——但它乘的是**超时**冷却（5 s），不是变道完成冷却（3 s）。若要复用 `lane_change_cooldown_s`，30 s 就该是 `3.0 × 10`。
>
> 两条路径（COMPLETED 和 TIMEOUT）设的都是 30 s。注释记录了原因（`:1500-1501`）：
>
> > 让触发条件再次满足 → 连环掉头（2026-08-03 demo8：COMPLETED 9s 后重触发，车越跑越偏）
> > 防止 planning 轨迹/车位置异常时反复进入掉头死循环（实测 2026-08-03：失败后每帧重触发，卡死 3 分钟）

**两次实测故障，都靠这 30 秒冷却兜住。**

---

## 6. 事件决策：一拍之内发生什么

`:1061-1526` 的事件计算块是整个节点最长的一段。它的结构是**按当前状态分支**：

```
if (CRUISE)        → 判断是否被堵 / 是否值得超 / 是否该并线
if (FOLLOW)        → 同上，额外检查掉头接近
if (LEFT_CHANGE)   → 检查完成（横向偏差 < 阈值）或超时
if (RIGHT_CHANGE)  → 同左
if (U_TURN)        → 检查航向对准
```

### 6.1 硬门控

`:1528-1550` 在发出事件前有一道硬校验：**拒绝无效的变道目标**。比如目标车道索引越界、或者目标等于当前车道，直接退回不产生事件。

### 6.2 超时兜底

`:1562-1585` 有一段注释为「P5 post-timeout immediate BLOCKED」的逻辑：**变道超时退回 CRUISE 的同一拍，再立刻发一个 `BLOCKED` 回 FOLLOW**。

理由是：变道超时通常意味着没变成功，前面大概率还有车。如果只退回 CRUISE，下一拍才会发 BLOCKED——**中间这一拍的目标速度还是巡航速度**，会往前冲一段。

### 6.3 事件的发出

`:1552-1560`：

```cpp
bool ok = statem_send_event(&g.sm, ev, nullptr);
if (ok) g.state_timer = 0.0;
```

**只有转移成功才重置 `state_timer`。** 状态计时器用于超时判定（`lane_change_timeout_s = 8.0 s`，`uturn_timeout_s = 40.0 s`）。

---

## 7. 热重载：17 个参数

注册在 `:1823-1856`，逐帧重读在 `:698-714`。**默认值取自结构体字段而非字面量**（`:1822` 的注释解释了原因）：

| 参数 | 默认 | 范围 | 作用 |
|---|---|---|---|
| `behavior.cruise_speed` | 15.0 | 1.0–50.0 | 巡航目标速度 |
| `behavior.acc_standoff` | 5.0 | 0.5–20.0 | CTG 静止间距 |
| `behavior.acc_time_headway` | 1.5 | 0.5–4.0 | CTG 时距 |
| `behavior.acc_k_gap` | 0.4 | 0.0–2.0 | 间距→速度增益 |
| `behavior.acc_gap_err_clamp` | 8.0 | 1.0–30.0 | 误差限幅 |
| `behavior.blocked_range_mult` | 3.5 | 1.0–10.0 | 触发距离倍数 |
| `behavior.blocked_range_min` | 30.0 | 5.0–100.0 | 触发距离下限 |
| `behavior.follow_hysteresis` | 1.3 | 1.0–3.0 | 进出跟车迟滞 |
| `behavior.lane_change_timeout_s` | 8.0 | 3.0–20.0 | 变道超时 |
| `behavior.lane_change_cooldown_s` | 3.0 | 1.0–10.0 | 变道冷却 |
| `behavior.lc_gap_mult` | 1.5 | 1.0–5.0 | 变道所需空隙倍数 |
| `behavior.rear_safe_min_m` | 15.0 | 5.0–50.0 | 后向安全下限 |
| `behavior.rear_safe_time_s` | 3.0 | 1.0–8.0 | 后向安全时距 |
| `behavior.same_lane_tol_offset` | 0.6 | 0.1–2.0 | 同车道横向容差偏置 |
| `behavior.uturn_approach_dist_m` | 120.0 | 20.0–200.0 | 掉头接近距离 |
| `behavior.uturn_max_trigger_speed` | 5.0 | 3.0–12.0 | 掉头触发速度上限 |
| `behavior.uturn_timeout_s` | 40.0 | 5.0–90.0 | 掉头超时 |

> 这是第 3 章讲的「三处都通」纪律的正面例子：`params_json` 解析（`:1796-1818`，处理 `target_speed` / `min_overtake_gap_base` / `min_overtake_gap_cap` / 四个 ACC 参数）、`param_register_*`（默认值用 `g.<字段>`）、逐帧 `param_get_float`——**三处都做了**。
>
> 对比第 20 章讲的：另外 14 个节点没有这个能力。

**注意 `min_overtake_gap_base` 和 `min_overtake_gap_cap` 不在这个表里**——它们只能从 `params_json` 初始化，**不能运行时改**。这是因为它们的值不进 `param_registry`。

---

## 8. 死代码与不一致清单

### 8.1 不可达状态

| 状态 | 证据 |
|---|---|
| `BEH_ST_STOP (204)` | 从不是任何规则的 `to`。只在 `:54`（定义）、`:93`（作 from）、`:285`/`:299`（字符串映射）出现 |
| `BEH_ST_YIELD (205)` | 同上，`:94` 作 from |
| `BEH_ST_EMERGENCY (206)` | **连一条规则都没有** |

**连带后果**：`BehaviorCommand` 里的 `BEH_STOP` / `BEH_YIELD` / `BEH_EMERGENCY` 三个值（`msg/adas_msgs.msg:222-224`）**永远不会被发布**。

### 8.2 硬编码的魔法数字

| 值 | 位置 | 说明 |
|---|---|---|
| `30.0` m, `5.0` m/s | `:1220` | 掉头兜底触发，**未参数化** |
| `7.0` m/s | `:1253` | 返程速度门限，与 `uturn_max_trigger_speed = 5.0` 不一致 |
| `1.0` m/s² | `:1169, 1241` | 掉头自然减速度 |
| `× 6.0` | `:1503, 1515` | 掉头冷却乘数 |
| `60.0` m | `:651` | 红灯门控距离下限 |
| `+20.0` m | `:650` | 红灯门控余量 |
| `200.0` m | `:840` | 跟车最大作用距离 |
| `1.0` m / `+1.0` | `:2561, 2654, 2668` | 掉头接近距离 / 递增步长 / 完成容差 |
| `0.5` | `:1141-1142, 1158` | 静态障碍速度阈值 / 施工区横向余量 |
| `0.05` ×3 | `:722-724` | 定时器步长（**没有从频率参数推导**） |

### 8.3 文档腐化

`:46-47` 的注释说「使用 `SM_EVENT_USER_BASE+` 区域」，但值是硬编码 `200..207`，而 `SM_EVENT_USER_BASE` 是 **16**（`state_machine.h:59`）。

---

## 9. 测试：零单测

**`behavior_planner_node.cpp` 没有任何单元测试。**

- 不在任何 `add_executable` 里（`CMakeLists.txt:1107-1117`、`modules/adas_nodes/CMakeLists.txt`）
- `tests/test_adas_nodes_logic.c` 里没有 behavior 测试
- `docs/HANDOFF_2026-09-21.md:202` 和 `docs/HANDOFF_2026-09-21b.md:224` 都明确记录「`behavior_planner_node.cpp` 仍零单测」

**只有间接覆盖**：回归基线 JSON 记录一个 `behavior_state` 字符串字段，比如 `tests/baseline/straight_road.json:116-117` 的 `"behavior_state": "CRUISE", "behavior_obs_count": 19`。

> **这意味着本章描述的所有阈值——CTG 的四个参数、迟滞 1.3、超车 1.5 倍、掉头 120 m——一个都没有单元测试。** 改任何一个都可能悄悄改变行为，只有跑完整回归才知道。
>
> 对比第 3 章：那个节点的 `param_get_float` 调用有测试（`tests/test_new_modules.c`）。这里的 17 个 `param_get_float` 完全没有。

---

## 10. 源码与资源对照

| 模块 | 源码文件 | 核心符号 | 职责与要点 |
|---|---|---|---|
| **行为决策节点** | [`modules/adas_nodes/behavior_planner_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/behavior_planner_node.cpp) | `BehaviorTask::run`（`:684`）<br>`BehState`（`:49`）<br>`BehEvent`（`:60`）<br>`BEH_TRANSITIONS`（`:71-105`）<br>`carriageway_ahead_stop_light`（`:643`） | 1940 行，20 Hz 编译进去。21 步主循环。CTG 跟车律、变道三条件与门、掉头专门逻辑 |
| **状态机框架** | [`include/state_machine.h`](file:///home/caixuf/code/FlowEngine/include/state_machine.h)<br>[`src/core/state_machine.c`](file:///home/caixuf/code/FlowEngine/src/core/state_machine.c) | `ReflectiveStateMachine`（`h:121-156`）<br>`TransitionRule`（`h:64-70`）<br>`find_transition`（`c:256-277`）<br>`statem_allowed_events`（`c:416-435`） | 「反射」= 用同一张表驱动转移和自省，**不订阅消息**。历史环形缓冲深度 8。`statem_add_transition` 存在但本节点不用 |
| **消息定义** | [`msg/adas_msgs.msg:217-235`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) | `BehaviorCommand`（8 值）<br>`Behavior`（22 B） | 其中 3 个命令值因对应状态不可达而永不发布 |
| **坐标工具** | [`modules/adas_nodes/planning_coordinates.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/planning_coordinates.h) | `first_legal_lane`（`:22-24`）<br>`nearest_own_lane`（`:34-37`）<br>`project_to_path`（`:39-72`） | 双向道路守卫用 `first_legal_lane` = `oneway ? 0 : lc/2` |
| **红绿灯工具** | [`include/traffic_light.h`](file:///home/caixuf/code/FlowEngine/include/traffic_light.h) | `TL_GREEN` / `TL_FLASHING_GREEN` / `TL_YELLOW` / `TL_RED`（`:38-43`）<br>`traffic_light_controls_direction`（`:122-126`） | 四相位。**闪烁绿算通行**，不阻止变道 |
| **施工区** | [`modules/adas_nodes/construction_zones.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/construction_zones.h) | `construction_zones_parse_from_scene_root`（`:22-48`）<br>`construction_zone_front_x`（`:51-54`） | **只用于缩短掉头触发点**，不影响跟车/变道 |
| **参数** | 第 3 章 | 17 个 `param_register_float`（`:1823-1856`）<br>17 次逐帧 `param_get_float`（`:698-714`） | 「三处都通」的正面例子。但 `min_overtake_gap_*` 不在 registry 里 |
| **测试** | — | **零单测** | 只有回归基线里的 `behavior_state` 字符串 |

---

## 11. 思考题

1. **三个不可达状态是设计还是遗漏？**：
   `STOP`（停车）、`YIELD`（让行）、`EMERGENCY`（紧急停车）三个状态有枚举、有字符串映射、有对应的 `BehaviorCommand` 值，但**没有任何转移规则指向它们**。规则 13、14 把它们当 `from`，这暗示原本设计有进入它们的路径。
   (a) 如果要激活 `EMERGENCY`（比如 TTC 低于阈值时切进去），需要加哪几条规则？触发事件应该从哪来？注意第 18 章安全层的 TTC 判断已经在做了——**决策层再做一个会不会是重复防护？** 两层的分工应该怎么划？
   (b) `YIELD`（让行）在真实自动驾驶里是必需的：路口对向来车、无保护左转、行人横穿。设计一个让行状态需要哪些转移规则和事件？特别注意：`on_return` 字段说明代码已经考虑了「返程时的让行方向」，这暗示让行逻辑曾经存在过。
   (c) 更根本的问题：**为什么这三个状态被保留下来而不是删掉？** 有人会说「接口预留」，有人会说「没清理干净」。请从「消息协议稳定性」的角度论证——如果下游（规划器）已经按 8 个命令值写了代码，现在删掉三个值是不是就破坏了兼容性？

2. **CRUISE→超车漏掉冷却检查**：
   第 4.6 节发现三条发起变道的路径里，只有 CRUISE→超车（`:1316`）漏了 `g.cooldown <= 0.0`。
   (a) 先确认这个判断：仔细读 `:1316` 和 `:1375`，看看除了冷却之外还有没有别的差异。然后构造一个具体的时序，说明在什么工况下「CRUISE→超车绕过冷却」会导致两次连续变道。
   (b) 修法有两种：在 `:1316` 加上 `g.cooldown <= 0.0`，或者把 CRUISE→超车也统一走「先发 BLOCKED 进 FOLLOW，再由 FOLLOW 发起超车」的路径。第二种更符合 2.3 节的「变道完成回 CRUISE」的循环设计。评估两种改法各自会改变哪些现有行为。
   (c) 更普适的问题：**怎么在 CI 里发现这类「三条相似路径里有一条漏了检查」的不一致？** 请设计一个静态检查：给定一组标记为「变道发起」的代码位置，验证它们包含同一组前置条件。难点是什么？（提示：C++ 里这些条件是散落的 `if` 子句，不是结构化的。）

3. **左右后向安全的不对称**：
   第 4.2 节指出左侧用 `v_rear × rear_safe_time_s`（随速度增长），右侧用固定的 `min_gap`（不随速度增长）。
   (a) 先量化这个差异：在什么后车速度下，两侧判据给出不同的结论？用默认参数（`rear_safe_min_m=15`, `rear_safe_time_s=3.0`, `min_overtake_gap_base=25`, `cap=90`）算出来。
   (b) 注释说这是有意为之（并线安全）。请论证这个理由是否站得住：**并线时后车速度差真的更小吗？** 如果车从中间道并到最外道，后车是从后方快速接近的同一批车——速度差并不会因为并线方向而变小。
   (c) 设计一个统一的判据。要求它 (i) 是纯运动学的、不需要加速度模型；(ii) 随后车速度单调增长；(iii) 在低速后车时与现有的 `rear_safe_min_m` 兼容。然后说明：改成统一判据后，哪些现有场景的行为会变化？

4. **给决策器写单测**：
   第 9 节说这个节点零单测，17 个参数一个都没有测试。假设要补，按下述顺序做：
   (a) **先做纯函数提取。** 决策逻辑里哪几段可以抽成不依赖全局状态 `g` 的纯函数？（提示：CTG 公式 `lead_speed + k_gap · clamp(Δ − d_desired, ±c)`、`blocked_range` 计算、`worthwhile` 判据、左右后向安全判据、`stop_range` 计算。）定义签名，并说明怎么验证提取后行为不变。
   (b) **为 CTG 写至少 8 个用例。** 特别地：验证稳态性质（`Δ = desired_gap` 时 `follow_speed == lead_speed`）、验证两个限幅各自的作用（把 `Δ` 推到 ±100 m 看输出被限在哪）、验证 `kFollowMaxRange = 200` 的边界（199 m 和 201 m 各什么结果）。每个用例锁住什么性质？
   (c) **为状态机写一个可达性测试。** 从 `CRUISE` 出发，用 BFS 枚举所有可达状态，断言结果**恰好是 5 个**（不含 STOP/YIELD/EMERGENCY）。这个测试的价值不只是「防止有人加状态」——请说明：如果将来有人 legitimately 地激活了 `YIELD` 状态，这个测试会怎么误报，以及怎么设计成「有加状态的 PR 时它应该失败并提示更新预期集」。
