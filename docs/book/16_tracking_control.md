# 第 17 章：跟踪控制 —— 横向几何级联、LTV-MPC 与机动跟踪器

> **本章导读**：
> 规划层交出来的只是一条「该往哪儿走」的期望轨迹；能不能真的贴着它走，取决于转向角打多深、油门踩多狠、什么时候收油。这一章讲的 [`modules/adas_nodes/control_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/control_node.cpp)（1586 行）是 KunAutoDrive 整条链路上唯一一个把「意图」翻译成「力与力矩」的地方。
>
> 但要先说破三件事——这本书很多读者是从自动驾驶教科书的通用叙述里来的，而源码的现实和那个叙述差距不小：
>
> 1. **横向控制器不叫教科书里的 Stanley。** 网上到处流传的 `δ = ψ_e + arctan(k·e_y/(v+k_soft))` 这个公式，在本仓库里**一行都不存在**，既没有 `k_soft` 这个软化常数，也没有那个 `arctan(k·e/v)` 的形式。实际落地的是一条**横向速度 PD → 期望航向角 ψ_des → 转向角级联**，外加两项前馈、一路一阶低通。
> 2. **这个 MPC 不是 QP，仓库里也没有任何 QP 求解器。** 没有 OSQP，没有内点法，没有 ADMM。`ltv_mpc_solve` 是一个 **3 状态、1 控制的离散仿射 LQR**，靠后向 Riccati 递推 O(N) 闭式解算。所谓 `max_steer` / `max_dsteer` 约束，是在**解完之后做饱和截断**，不是在优化里当约束处理。
> 3. **MPC 默认是关的。** `use_ltv_mpc{0}`，且没有任何 `config/*.json` 打开过它。原因见第 5 节：它在变道场景的回归里 3 次 3 次挂。
>
> 本章逐行拆解这三条，以及掉头/倒车这类「参考线是断的」工况到底怎么跟。

---

## 1. 控制节点的位置：整条管线的最末端

### 1.1 话题契约

控制节点是插件（[`modules/adas_nodes/CMakeLists.txt`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/CMakeLists.txt) 里 `add_library(control_node SHARED control_node.cpp)`，编译期带 `-DFLOWCORO_INTEGRATION`），入口是文件末尾的 `NodePlugin s_plugin`（`control_node.cpp:1569-1582`）：

```cpp
s_plugin.name    = "control";
s_plugin.version = "1.0.0";
s_plugin.desc    = "PID longitudinal controller + ACC [FlowCoro]";
```

它订阅四路（回调注册在 `control_node.cpp:1490-1493`）：

| 话题 | 回调 | 提供的量 |
|---|---|---|
| `fusion/localization` | `on_fusion` | 自车 x/y、航向、横摆角速度 |
| `vehicle/state` | `on_vehicle_state` | 车速、挡位、**轴距实车标定值** |
| `planning/trajectory` | `on_trajectory` | 期望轨迹点（x, y, v, κ, t_rel） |
| `planning/behavior` | `on_planning_behavior` | 行为档位、目标车道 |

发布四路（`transport_advertise` 在 `control_node.cpp:1494-1497`）：

| 话题 | 内容 | 频率 |
|---|---|---|
| `control/raw_cmd` | 二进制 `ControlRaw` | 40 Hz |
| `control/raw_cmd/text` | `snprintf` 文本 | 40 Hz |
| `control/cte` | JSON `{cte, speed, seq}` | 40 Hz |
| `control/debug` | JSON 全量内部状态 | **每 10 周期 ≈ 2 Hz** |
| `control/ldw` | 车道偏离告警 | 仅告警帧 |

> [!NOTE]
> `ControlContext::s_inputs[]`（`control_node.cpp:1308`）这个数组里**漏了 `vehicle/state`**，尽管 1491 行确实订阅了它。这个数组是给数据流看板用的，漏登记会让链路图少一条边，但不影响运行。

### 1.2 控制循环的真实频率：40 Hz，而数学是 20 Hz

主循环在 `CoroutineTask` 的 `run()` 里（`control_node.cpp:506`）：

```cpp
/* BusQueueBridge 常驻订阅，替代 select_for 每次循环反复注册/退订 */
BusQueueBridge ctrl_bridge(bus(),
    {TOPIC_FUSION_LOCALIZATION, TOPIC_PLANNING_TRAJECTORY});
while (!should_stop()) {
    (void)co_await ctrl_bridge.recv_any_for(50000);   /* 50ms 超时兜底 */
    if (should_stop()) break;

    /* 40Hz rate limit */
    uint64_t _ctrl_now = clock_now_us();
    if (_ctrl_now - g.last_ctrl_us < 25000) continue;   /* ← 25ms，硬上限 */
    g.last_ctrl_us = _ctrl_now;
    ...
```

注意这个循环是**消息驱动**的：它不是自由跑的定时器，而是被 `fusion` 或 `planning` 的消息唤醒，再靠 25 ms 的时间戳比较把频率压到 40 Hz。规划节点 20 Hz 发轨迹，所以稳态下大约每来一条轨迹执行一次有效控制。

> [!WARNING]
> **这里有一个真实的不一致，读者应当知情**：
> 频率是 40 Hz，但**积分和微分用的步长是硬编码的 0.05**（`control_node.cpp:866` 与 `870`）：
> ```cpp
> g.integral   += pid_error * 0.05;                  /* :866 */
> double derivative = (pid_error - g.prev_error) / 0.05;  /* :870 */
> ```
> 而同文件第 62 行的注释明明白白写着 `CONTROL_DT_S 0.05 /* 控制环周期: 20Hz */`，`CONTROL_DT_S` 也确实被传给了 `ManeuverTracker::tick()`（`:732`）。也就是说，40 Hz 跑的循环里，PID 的 I 项和 D 项都按 20 Hz 在累积/微分。
> 后果是可预测的：实际采样周期是 0.025 s，积分每帧只累 0.05 s 的量，**积分增益被系统性放大了一倍**；微分则是拿 0.05 的分母去除 0.025 s 间隔的差，**微分增益被砍掉一半**。后者反而起了额外阻尼作用，掩盖了问题，所以这个不一致至今没暴露成 bug。
> 相比之下 MPC 的步长是老老实实的：`cfg.dt = 0.025`（`ltv_mpc.c:125`，注释写明「匹配 control_node 40Hz 控制周期」）。

---

## 2. 纵向：一个带镜像处理的 PID

### 2.1 控制量：油门/刹车，不是加速度

先明确一件事：**KunAutoDrive 的纵向控制器不输出加速度，它输出油门开度和刹车压力**。`ControlRaw` 消息里根本没有 `accel` 字段（见 [`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) 第 189-202 行的 `ControlRaw` 定义）：

```
struct ControlRaw {
    uint32 seq            # 指令序号
    float  throttle       # 油门 [0.0, 1.0]
    float  brake          # 制动 [0.0, 1.0]
    float  steering       # 转向角（rad）：正值右转，负值左转
    float  speed          # 当前速度 (m/s)
    float  target         # 目标速度 (m/s)
    float  error          # 速度误差 (m/s)
    float  cte            # 横向跟踪误差 (m)
    char   mode[24]       # ACCEL/BRAKE/HOLD/ROAD_GUARD 等
    uint8  turn_signal    # 0=off, 1=left, 2=right
    bool   hazard
    Gear   gear           # DRIVE=1/REVERSE=-1/NEUTRAL=0
}
```

这一点值得反复强调，因为它决定了 PID 的**量纲和整定逻辑完全不同**。经典纵向 PID 整定假设 `u = K_p·e` 出来的是 m/s²，配上车辆质量就是力；这里 `u` 直接就是归一化的油门，所以 `K_p` 的物理意义是「每秒多少开度对应多少 m/s 的速度误差」。

### 2.2 PID 本体与不对称标定

代码在 `control_node.cpp:854-885`：

```cpp
/* PID 纵向 — 倒车时镜像速度符号，让 PID 始终在正域工作。
 * 倒车：target=-3.0, current=-1.0 → pid_target=3.0, pid_current=1.0
 * → error=2.0 → output>0 → throttle=+ → 最后取反为负油门。 */
double pid_target = acc_target;
double pid_current = g.current_speed;
bool is_reverse = (g.gear == GEAR_REVERSE);
if (is_reverse) {
    pid_target  = -acc_target;
    pid_current = -g.current_speed;
}
double pid_error = pid_target - pid_current;

g.integral += pid_error * 0.05;
if (g.integral >  500) g.integral =  500;    /* 上限不对称 */
if (g.integral < -200) g.integral = -200;    /* 下限不对称 */

double derivative = (pid_error - g.prev_error) / 0.05;
double output = g.kp * pid_error + g.ki * g.integral + g.kd * derivative;

if (output > 0) {
    throttle = output / 5000.0;               /* 加速：除 5000 */
    if (throttle > 1.0) throttle = 1.0;
    brake = 0;
    mode = (pid_error < 1.0) ? "HOLD" : (is_reverse ? "REV_ACCEL" : "ACCEL");
} else {
    throttle = 0;
    brake = (-output) / 8000.0;               /* 制动：除 8000 */
    if (brake > 1.0) brake = 1.0;
    mode = is_reverse ? "REV_BRAKE" : "BRAKE";
}
/* 倒车：油门取反（flowsim physics 用负油门触发倒车） */
if (is_reverse) throttle = -throttle;
```

三个设计要点，都是从实车跑出来的：

- **归一化系数不对称**（5000 / 8000）：同样的 `output` 幅值，油门开得比刹车轻。这不是笔误，是发动机与制动系统的物理差异。
- **积分限幅不对称**（`[-200, +500]`）：允许积累更多正积分（维持油门），但严格限制负积分（防止长时间刹车后「一松刹车就窜出去」）。
- **倒车镜像**：倒挡时目标速度和当前速度同时取反，PID 全程在正域工作，出口再把油门取反。源码注释把这套机制讲得很直白。

默认增益在 `control_node.cpp:1357`，并通过 `param_registry` 暴露为热重载参数（`:1431-1434`）：

| 参数 | 默认值 | 管道配置（`config/pipeline.json:291`） | RC 车（`config/pipeline_car.json:247`） |
|---|---|---|---|
| `control.pid_kp` | 800.0 | 800 | 300 |
| `control.pid_ki` | 50.0 | 50 | 20 |
| `control.pid_kd` | 100.0 | 100 | 40 |
| `control.cruise_speed` | 12.0 | 20（仅作参考） | 2 |

### 2.3 三重抗积分饱和

这是全章最值得学习的一段工程代码（`control_node.cpp:900-919`）：

```cpp
/* Anti-windup：error 从正翻负时（加速→减速切换），积分饱和是追尾主因。
 * 加速阶段积分可累积到 +500（I=50×+500=+25000），此时减速指令 P=800×(-8)=-6400，
 * 总量 +18600 → 油门全开撞上去。
 * 修复：error 翻负且 |error|>2 时直接清零正积分；正常饱和时慢速泄放。 */
if (pid_error < -2.0 && g.integral > 0) {
    g.integral = 0;  /* 从加速切到减速，残余正积分是催命符，立刻清零 */
} else {
    if (g.integral > 0 && throttle >= 1.0 && pid_error > 0)
        g.integral -= pid_error * 0.05;   /* 油门饱和 → 慢速泄放 */
    if (g.integral > 0 && brake    >= 1.0 && pid_error < 0)
        g.integral += pid_error * 0.05;
}
/* 2026-08-05 掉头卡死修复（负积分对称清除）：Phase 1 刹停到 0 时
 * error<0 使积分转负，目标翻正（驻停/慢转 target>0）后负积分残留把车
 * 钉在原地 —— 掉头返程实测 spd=0 target=0.5 brk=0.13 卡死 30s timeout。 */
if (g.maneuver_mode && g.integral < 0 &&
    pid_error > 0.2 && fabs(g.current_speed) < 1.0) {
    g.integral = 0;
}
```

三层机制，缺一不可：

1. **急清**：误差从正翻负且幅度大（`e < -2`），立刻清零正积分。这是防追尾的第一道闸。
2. **慢泄**：输出饱和但误差方向未变时，按 `e·dt` 反向泄放积分，保住正常的稳态跟踪能力。
3. **负向对称清除**：机动模式下、车已近停而目标转正时清掉负积分，专治「卡在原地 30 秒」这个 2026-08-05 实测到的掉头故障。

### 2.4 换挡必须物理刹停

另一个直接写进代码的实车教训（`control_node.cpp:887-898`）：

```cpp
/* 机动换挡刹停：gear_pending = 带速想换挡（掉头 Phase 2→3 D→R
 * 交界）。巡航 PID 是减速工况标定的，对 2.4m/s 只给 0.23 brake
 * （decel≈1.2m/s²，需 1.9s/2.3m），车还没停就冲过倒车段、最近点
 * 跳到 Phase 4 正向 → gear 翻回 D → 加速冲出路面（2026-08-03
 * 实测 y=8.5）。换挡必须物理刹停，直接给全刹——这就是掉头
 * 三把方向的"停"字。 */
if (g.maneuver_mode && g.gear_pending) {
    throttle = 0.0;
    brake    = 1.0;
    mode     = "SHIFT_STOP";
    g.integral = 0;  /* 全刹时清积分，防换挡后残余积分反向推车 */
}
```

掉头要「左打满 → 倒车 → 再打右」三把方向，每一把之间都必须真的停住。用巡航 PID 的软制动去停一个 2.4 m/s 的车需要近 2 秒，在这段距离里车会一路冲过倒车段的起点。**所以这里直接绕过 PID，全刹。** 这行 `brake = 1.0` 就是「三把方向的停字」的字面实现。

### 2.5 目标速度的来源：1.5 秒近窗最小值

控制层不做任何速度决策——没有巡航速度覆盖、没有超速降档、没有 boost。这是 Apollo 的原则，源码注释写得很明确（`control_node.cpp:718-723`）：

> 纵向控制：Apollo 原则 —— control 是纯轨迹跟随器
> 速度唯一来源是 planning 轨迹末点 v（g.target_speed）。
> control 不做速度决策：没有 cfg_cruise_speed 覆盖、没有 boost、没有 overspeed 降档。

但「取轨迹**末点**的 v」这个做法曾经出过大问题，改成了**近窗最小值**（`control_node.cpp:368-381`）：

```cpp
/* 目标速度：巡航 = 轨迹近窗 1.5s 内最小 v（跟随减速/加速意图）；
 * 机动 = ManeuverTracker 每帧刷新（见 tick）。
 * 旧实现取轨迹末点 v：末点在 80m/4s 外，弯后全速值让 PID 弯中还在
 * 加速（陆家嘴实测 v=10.6 过 R=20 弯 → steer_limit_for_speed 按速
 * 限幅到 0.034 ≪ 所需 0.135 → 转向被锁、车直冲路口 → ROAD_GUARD）。
 * 近窗最小 v 把 ST 剖面的弯前减速提前透传到纵向 PID。 */
if (!mv) {
    double tv = (double)traj.points[n_pts - 1].v;
    for (uint32_t i = 0; i < n_pts; i++) {
        if ((double)traj.points[i].t_rel_us > 1500000.0) break;
        if ((double)traj.points[i].v < tv) tv = (double)traj.points[i].v;
    }
    g.target_speed = tv;
}
```

这是一个**级联延迟**的经典案例，值得单独理解：

- 轨迹末点在 80 m / 4 s 之外，那里是「弯后」的速度；
- 弯中读到弯后的全速值 → PID 不减速；
- 车速不降 → 转向包络 `steer_limit_for_speed` 随 $v^2$ 收紧 → 舵量不够 → 转向被锁死 → 车直冲路口。

**4 秒前的意图，害死了 4 秒后的转向。** 改成取 1.5 s 近窗最小值，等于让纵向提前 2.5 秒感知到即将到来的弯道减速，这 2.5 秒恰好够把车速压下来、把转向权限还给控制器。

---

## 3. 横向：不是 Stanley 的横向级联 PD

### 3.1 先破除公式误区

绝大多数讲 Stanley 的材料给出的公式是：

$$\delta(t) = \psi_e(t) + \arctan\left(\frac{k \cdot e_y(t)}{v(t) + k_{\text{soft}}}\right)$$

**KunAutoDrive 里没有这个公式。** 没有 `k_soft`，没有 `arctan(k·e_y/v)`。实际落地的是一条**三级级联**：

```
        横向偏差 e_lat
              │
              ▼  k_vy=0.35, k_vy_damp=0.6
      期望横向速度 v_y_des  ←──── 横向速度反馈
              │
              ▼  asin(v_y_des / speed_eff)，限幅 ±0.5
        期望航向角 ψ_des
              │
              ▼  atan2(lat_kp·e_lat, speed_eff) − 2.0·Δψ − 0.28·ω
        转向角 δ
```

**三级串联的意义**：横向误差不直接映射成转向角，而是先翻译成「我此刻应该横着走多快」，再翻译成「我此刻车头该朝哪」，最后才翻译成「前轮该打多少度」。多出来的这一层冗余，是**为了打断误差与转向角之间的直接高增益通路**——直接通路 $e \to \delta$ 在实车上必然震荡。

### 3.2 完整实现

代码在 `control_node.cpp:999-1061`，逐段拆开看：

```cpp
double abs_speed     = fabs(g.current_speed);
double speed_eff     = fmax(abs_speed, 3.0);        /* ← 速度软化，地板 3.0 m/s */
double v_lat_actual  = abs_speed *
    sin(g.ego_heading - ref_road_heading);          /* 实际横向速度 */

/* v_y_des = k_vy * e_lat − k_vy_damp * v_lat（参考系左法向，巡航模式） */
double v_y_des = g.k_vy * lat_err_n - g.k_vy_damp * v_lat_actual;

/* 期望航向角：asin(v_y_des / speed_eff)，比例限幅 ±0.5 (±30°) */
double psi_des = ref_road_heading;
{
    double vy_ratio = v_y_des / speed_eff;
    if (vy_ratio >  0.5) vy_ratio =  0.5;
    if (vy_ratio < -0.5) vy_ratio = -0.5;
    psi_des = ref_road_heading + asin(vy_ratio);
}

/* 前馈 1：纯运动学 —— 要产生 v_y_des 这么多横移，前轮该打多少 */
double delta_ff = atan(g.wheelbase * v_y_des / (speed_eff * speed_eff + 1e-6));

/* heading 突变保护：期望航向与当前车头差超过 0.5 rad 就冻结目标 */
double ref_h_eff = psi_des;
{
    double dh = ref_h_eff - g.ego_heading;
    while (dh >  M_PI) dh -= 2.0 * M_PI;
    while (dh < -M_PI) dh += 2.0 * M_PI;
    if (fabs(dh) > 0.5) ref_h_eff = g.ego_heading;
}

/* 横向误差项：atan2(k·e, v_eff)，低速天然饱和，无需额外软化常数 */
double cte_term = atan2(g.lat_kp * lat_err_n, speed_eff);

/* 航向误差项：必须 wrap，否则 ±π 边界抖动会打满舵 */
double heading_term;
{
    double dh_t = g.ego_heading - ref_h_eff;
    while (dh_t >  M_PI) dh_t -= 2.0 * M_PI;
    while (dh_t < -M_PI) dh_t += 2.0 * M_PI;
    heading_term = g.lat_kd_heading * dh_t;
}

/* 横摆角速度阻尼 */
double yaw_damp_term = g.yaw_damping * g.ego_yaw_rate;

/* 前馈 2：曲率前馈，曲率半径 ≤ 60m 时权重 ×1.5 */
double kappa    = ref_kappa;
double ff_weight = 1.0;
if (fabs(kappa) > 1e-9) {
    double R = 1.0 / fabs(kappa);
    if (R <= g.curve_ff_boost_radius_m) ff_weight = g.curve_ff_boost_factor;
}
double ff_term = g.wheelbase * kappa * ff_weight;

steer = cte_term - heading_term - yaw_damp_term + ff_term + delta_ff;
```

五项相加，符号是 `+ - - + +`。逐项的物理含义：

| 项 | 系数 | 默认值 | 物理含义 |
|---|---|---|---|
| `cte_term` | `lat_kp` | 0.5 | 横向误差 → 转向角（唯一的位置纠偏项） |
| `heading_term` | `lat_kd_heading` | 2.0 | 航向误差 → 转向角（主阻尼） |
| `yaw_damp_term` | `yaw_damping` | 0.28 | 横摆角速度阻尼（抑制自激振荡） |
| `ff_term` | — | ×1.0/×1.5 | 曲率前馈（稳态零误差的关键） |
| `delta_ff` | — | — | 运动学前馈（让级联内环自洽） |

**符号是减的**（`steer = cte − heading − yaw_damp + ff + delta_ff`），因为代码里 `steer` 是**右正左负**（消息定义：「正值右转，负值左转」），而 `lat_err_n` 是**左正右负**的左法向投影（`control_node.cpp:844-845`）。两套符号约定不一致，所以误差类项要减。

### 3.3 关键设计：`speed_eff` 的 3.0 m/s 地板

```cpp
double speed_eff = fmax(abs_speed, 3.0);
```

`atan2(lat_kp · e, speed_eff)` 这个式子的分母就是防止低速时转向角发散的那道软化。教科书 Stanley 用 `v + k_soft` 做加性软化，这里用 `max(v, 3.0)` 做**截断式软化**——效果类似但行为不同：加性软化下低速时增益仍会缓慢上升，截断软化下 3.0 m/s 以下增益**完全冻结**。

3.0 m/s 这个地板是标定出来的：低于 10.8 km/h 时横向控制的精度要求可以放松（反正车走得慢，横向偏差有充足时间被消掉），而放大增益只会引来低频漂移。

**注意用 `fabs` 取绝对值**：倒车时 `v < 0`，若直接代入会让 `v_lat_actual` 和 `cte_term` 的符号全部翻转，反馈变成正反馈。源码注释点明了这一点（`:1002-1003`）。

### 3.4 曲率前馈的 1.5 倍增益

```cpp
if (R <= g.curve_ff_boost_radius_m) ff_weight = g.curve_ff_boost_factor;
```

曲率半径小于 60 m 时，前馈权重从 1.0 抬到 1.5。理由是：`ff_term = L·κ` 只是**理想运动学**的稳态前馈（假设 `δ = L·κ` 精确跟随），但真实车辆在弯中还有轮胎侧偏带来的稳态横摆误差，实际需要的前轮转角大于 `L·κ`。在大曲率弯道上这个偏差被放大，不补偿就只能靠 `cte_term` 去纠，而 `cte_term` 的增益只有 0.5，补起来会引入明显滞后。

补偿系数在 $R < 60$ m 时取 1.5，是实车在多个弯道上扫出来的经验值。

### 3.5 限幅：一个 15 行的函数，藏着两次实车教训

```cpp
static double steer_limit_for_speed(double speed_mps, double max_lateral_accel_mps2) {
    double speed = speed_mps;
    if (speed < 2.0) speed = 2.0;                       /* 速度地板 2.0 m/s */
    double limit = atan(max_lateral_accel_mps2 * g.wheelbase / (speed * speed));
    if (limit < 0.016) limit = 0.016;                    /* 下限 0.016 rad ≈ 0.92° */
    if (limit > 0.16) limit  = 0.16;                     /* 上限 0.16 rad ≈ 9.2° */
    return limit;
}
```

这来自运动学自行车模型：稳态转弯时 $a_{lat} = v^2\delta/L$，令 $a_{lat} = a_{max}$ 解出 $\delta = L\,a_{max}/v^2$。

- **$1/v^2$ 的物理意义**：车速翻倍，可用转向角降到四分之一。这条曲线是横向控制的硬边界——**它不是调出来的，是车的物理上限**。
- **$a_{max}$ 是权限，不是能力**：调用点传进来的值是 4.0（巡航）、2.4（大横向误差/ROAD_GUARD）、1.4（数据超时时降级），它表示「本车此刻被允许使用多少横向加速度」，物理上车辆能提供 5.0（第 16 章的 `STG_A_LAT_MAX`），留 20% 余量。

这个函数在控制节点里被调用了**四次，参数各不相同**：

| 调用点 | `a_max` | 场景 |
|---|---|---|
| `control_node.cpp:1056` | 4.0 | 正常巡航 |
| `control_node.cpp:946, 982` | 2.4 / 1.4 | MPC（按 `\|e_y\| > 0.5` 自适应） |
| `control_node.cpp:1133` | 2.4 | ROAD_GUARD 回正 |
| `control_node.cpp:597` | 1.4 | 数据超时降级 |

> [!NOTE]
> **巡航权限 1.4 → 4.0 的调整**（`control_node.cpp:1050-1054`）是本层最重要的一次实车修正：
> ```cpp
> /* 巡航横向加速度权限 1.4→4.0（2026-08-15 陆家嘴实测）：
>  * ST 规划允许 a_lat≤4.25 (5×0.85 安全系数)，控制只准 1.4
>  * → v=7.9 过 R=20 弯时限幅 0.061 ≪ 所需 0.135，转向被锁
>  * 车直冲叉路。4.0 与规划预算对齐；直道上该限幅不约束
>  * 微小修正（所需 a_lat≪1），不影响舒适性。 */
> ```
> 规划层按 `5.0 × 0.85 = 4.25` 放行一条 4.0 m/s² 的弯道轨迹，控制层却只给 1.4 m/s² 的权限——**上限更严的控制层会锁死一个合法的轨迹**，车物理上无法跟随，只能直冲。控制层的限幅必须 ⊇ 规划层的规划预算，否则两层接口就是矛盾的。

### 3.6 输出整形：低通 + 死区

```cpp
steer = STEER_FILTER_NEW * steer + (1.0 - STEER_FILTER_NEW) * g.prev_steer;
if (fabs(steer) < 0.005) steer = 0.0;
g.prev_steer = steer;
```

低通系数 0.5 的来历写在宏定义处（`control_node.cpp:52-55`）：

```cpp
/* 低通滤波新值权重：0.5 = -3dB@1.2Hz (20Hz 采样)。
 * 旧值 0.8 (-3dB@2.8Hz) 高频抑制不足, Stanley 控制器在 cte_term 与 heading_term
 * 互相反向时产生 ~1.6Hz 极限环振荡（左摇右晃）。降到 0.5 增加阻尼, 让 steer
 * 平滑过渡, 牺牲少量相位裕度换取稳定性。yaw_damping 配合抑制高频。 */
#define STEER_FILTER_NEW   0.5
```

**这个极限环是横向控制最经典的病理**：位置误差项要求往左打，航向误差项（阻尼）要求往右打，两个项在轨迹上某处形成等幅反向的拉锯，方向盘就以约 1.6 Hz 持续小幅左右摇摆。不打舵车不跑偏，舵又一直在动——乘客会觉得「方向盘自己在抖」，而系统判定一切正常。

三道防线一起上：
- **低通**（−3 dB @ 1.2 Hz）直接砍掉极限环所在的频段；
- **`yaw_damping`**（0.28）从横摆角速度这个物理量上加阻尼，让自激振荡在源头得不到正反馈；
- **`lat_kd_heading`**（2.0）提供主要的航向阻尼。

死区 0.005 rad ≈ 0.287°：小到这个量级的转向请求对应车轮亚毫米级位移，机构间隙会把它整个吃掉，而执行器为此耗电并让方向盘看起来「有反应但车没动」。

---

## 4. 机动跟踪器：参考线断开时怎么跟

### 4.1 什么算「机动」

掉头、倒车入库、侧方泊车、三点掉头有一个共同点：**参考线不是一条连续单调的参数曲线，而是「前进弧 → 换挡刹停 → 倒车弧」的多段拼接**，而且倒车段的 $s$ 方向与前进段相反。常规的纯轨迹跟随器（对一条单向参数曲线做最近点投影）在这种输入上会直接失效。

KunAutoDrive 的判据极其朴素（`control_node.cpp:344-351`）——**不看行为节点怎么说，只看数据长什么样**：

```cpp
bool has_reverse = false;
double max_kappa = 0.0;
for (const auto& point : traj.points) {
    max_kappa = max(max_kappa, fabs(point.kappa));
    if (point.v < -0.1) has_reverse = true;
}
bool mv = has_reverse || (max_kappa > 0.12);   /* |κ|>0.12 ⟺ R < 8.3 m */
```

- 出现负速度（$v < -0.1$ m/s）→ 机动；
- 或曲率超过 0.12 1/m（转弯半径小于 8.3 m）→ 机动。

> [!IMPORTANT]
> **注意：变道不算「机动」。** 行为节点给出的 `BEH_LEFT_CHANGE` / `BEH_RIGHT_CHANGE` 走的是普通巡航分支，`maneuver_mode` 始终为 false，变道只是打左/右转向灯（`control_node.cpp:1160-1163`）。这和很多读者的直觉相反，但逻辑是对的：变道轨迹仍然是连续正向的，规划器和横向级联 PD 完全能处理，不需要特殊跟踪器。真正需要特殊处理的只有「倒车 + 大曲率」这一类。

### 4.2 弧长推进与发散保护

`ManeuverTracker`（[`modules/adas_nodes/maneuver_tracker.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/maneuver_tracker.h)，header-only，`namespace maneuver`，`kMaxPts = 64`）的核心状态就是一个标量 `s_`——沿轨迹走过的弧长。

**推进**（`advanceS`，`:131-147`）：

```cpp
s_ += |v| * dt;
```

用绝对值，让 s 在正反两个方向都能前进。推进受阻于两个门限：

- **发散保护**（`diverge_guard_rad = 1.2`）：如果车头朝向与执行点朝向的夹角超过 1.2 rad（68.6°），**冻结 s 不动**。理由是：车已经完全跑偏，此时继续推进只会让误差越滚越大，应该先让横向控制把车头拉回来。
- **段边界门限**（`heading_gate_rad = 0.4`）：在前进段 / 倒车段的交界处，除非车头朝向已经对准下一段（夹角 < 0.4 rad），否则不允许跨越。

### 4.3 横向律：前馈 + 反馈，倒挡反馈反号

```cpp
// 横向：kappa 前馈 + 车体系 e_lat/dh 反馈，倒挡反馈反号
TrackPoint tgt = lookahead(exec_i);
bool   rev = (gear_ == -1);
double dh    = normAngle(tgt.heading - ego_heading);
double e_lat = -std::sin(tgt.heading) * (tgt.x - ego_x)
               + std::cos(tgt.heading) * (tgt.y - ego_y);
double ff = std::atan(wheelbase_ * tgt.kappa);
double fb = p_.lat_gain_dh * dh + p_.lat_gain_elat * e_lat;   /* 0.8 * dh + 0.25 * e_lat */
if (rev) fb = -fb;                                          /* ← 倒挡反馈反号 */
double steer = std::max(-p_.max_steer, std::min(p_.max_steer, ff + fb));
```

这就是控制层最难缠的那件事的解法（`maneuver_tracker.h:275-284`）。

> **为什么倒挡必须反号？**
> 前轮转角 δ 产生的前轮侧向力大致指向 $v \times \delta$ 的方向。挂倒挡时 $v < 0$，这个叉积的符号整体翻转——**同一个 δ，在倒挡下产生的是方向相反的横移**。如果照搬前进时的反馈项，车想往左修正却给出一个让它往右的转角，构成正反馈，转角在几个周期内顶到 0.60 rad 的机械极限然后卡死。
> 教科书上的解法是「把参考点从后轴换到前轴，再乘 −1.0」。这里用的是等价但更简洁的写法：**保持参考点在原点（控制点），直接给反馈项乘 −1**，前馈项 $\arctan(L\kappa)$ 不动（它描述的是「前轮该打多少度」，与运动方向无关）。

限幅是 `max_steer = 0.60` rad（34.4°），远大于巡航的 0.16 rad。原因写在 `control_node.cpp:1047-1049`：

```cpp
/* 机动模式放开限幅到满舵 0.60rad：巡航限幅 0.16rad 的转弯
 * 半径 ≥16.7m，无法执行规划的掉头弧（0.45rad，R≈5.6m）。
 * flowsim 物理层看到 |steer|>0.28 会同步 steer_override。*/
```

0.16 rad 对应的最小转弯半径是 $L/\delta = 2.7/0.16 \approx 16.9$ m，而典型掉头弧的半径只有 5.6 m。用巡航限幅跑掉头，物理上就拐不过来。

### 4.4 换挡时机：随刹车距离收缩的前视窗口

掉头的难点全在「什么时候换挡」。`updateGear`（`maneuver_tracker.h:197-223`）里有一段设计得非常好的注释：

```cpp
// 前视窗口按刹车距离收缩（v²/4+0.5，下限 1.0），别用固定 6m：
// 掉头前进弧总长才 ~8-10m，车刚进弧 2m 时固定 6m 前视就能扫到倒车段 →
// gear_pending 过早刹停（实测 h=1.05 就停，远没到 corner），换 R 挡后
// 执行点还卡在前进弧内 → steer 前馈取错方向 → 倒车画圆死锁。低速时窗口
// 缩短到 < 到倒车段的弧长，只有真正能刹住时才请求换挡。
double scan = std::max(1.0, current_speed * current_speed / (2.0 * 2.0) + 0.5);
```

**前视窗口 = 刹车距离 + 余量**。物理上，车辆从当前速度刹停需要走 $v^2/(2a)$ 米，扫到那么远以外的倒车段，就意味着「按当前速度真能停住」。把 6.0 m 的固定窗口换成刹车距离自适应的窗口之后，掉头的换挡点从「刚进弧 2 m 就停」变成「真正接近拐点才停」。

换挡请求还有两个条件（`:219-220`）：

```cpp
if (want != gear_ && std::fabs(current_speed) > p_.gear_pending_speed)
    return true;   // gear_pending：带速想换挡，本帧刹停
```

`gear_pending_speed = 0.8` m/s。低于这个速度就直接换，不请求刹停——低速下原地换挡没什么风险，停下来反而浪费时间。返回 `true` 后，由 2.4 节的 `SHIFT_STOP` 分支接管，**全刹**。

### 4.5 三个真实修掉的死锁

这一节是本章最有工程价值的部分——`maneuver_tracker.h` 里有三段注释描述了 2026-08 的三次实车故障修复，每一个都是「控制逻辑正确但符号/时序错位导致系统锁死」。

#### 死锁一：倒车画圆（`:236-247`）

```cpp
// 修：换挡已落定但执行点仍卡在相反挡段 → 倒车画圆死锁。
// 掉头前视窗口过早扫到倒车段，gear_ 已置 -1，但 exec 点还在前进弧内
// （pts_[exec_i].v > 0）→ 前馈 ff 取前进弧 kappa（+0.6），倒挡下 steer 方向
// 反 → yaw_rate 反向、heading 一路减 → advanceS 的 diverge_guard 永久冻结 s。
// 这里把 s_ 直接跳到下一倒车段边界并重取执行点，让前馈/反馈方向与挡位一致，
// 车头才能正确转升向 π，dh 收敛后 advanceS 恢复推进，后续点自然衔接。
if (gear_ == -1 && (double)pts_[exec_i].v > p_.segment_v_threshold) {
    for (int i = exec_i; i < n_ - 1; i++) {
        if ((double)pts_[i + 1].v < -p_.segment_v_threshold) { s_ = cum_s_[i]; break; }
    }
    exec_pt = exec(&exec_i);   // 重取执行点 → ff 取倒车段 kappa，方向正确
}
```

**级联故障链**是这一节的精髓，值得完整读一遍：

```
换挡窗口过早 → gear_ = -1，但 s_ 还在前进弧里
      ↓
前馈取前进弧的 κ（+0.6，倒车方向下这是错的转角）
      ↓
yaw_rate 反向 → heading 递减
      ↓
heading 与执行点朝向差超过 diverge_guard_rad(1.2)
      ↓
advanceS 永久冻结 s_  ←── 系统彻底锁死
```

**关键洞察**：这个死锁的真正成因是**执行点 $s$ 和挡位 $g$ 失步**。修复方式不是调参，而是强制两者的语义一致性——挡位是 −1 就把执行点直接推到倒车段的起点。

#### 死锁二：换挡边界速度符号模糊（`:249-253`）

```cpp
// 目标速度方向跟随「已落定的 gear」，而非插值 exec v：
// D/R 边界处 exec 在 v=-3 与 v=+3.5 间插值，v 符号在边界模糊（可能跨 0），
// 若用 exec_rev 会与 gear 不一致 → 倒挡期拿到正向 target → REV_BRAKE 卡死
// （2026-08-04 实测掉头倒车后 spd 卡 0 直到 40s timeout）。
bool exec_rev = (gear_ == -1);
```

目标速度的**方向**取自已落定的挡位（离散、可靠），**幅值**才取自执行点（连续、平滑）。用插值出来的 v 判方向，在 D/R 边界会拿到一个符号错误的 target，倒挡期 PID 判成需要制动，车停住不动——又是一个锁死。

#### 死锁三：短机动 min|v| 被地板吃掉（`:262-264`）

```cpp
// 与 updateGear 一致：跳过 v≈0 的换挡刹停/驻停点，否则 min|v| 被
// 压成 floor，车在短前进段里 0.5m/s 爬行，200 tick 到不了 D/R 边界
// 进不了倒挡（parking/parallel CI 失败，2026-08-05 实测）。
if (std::fabs((double)pts_[i + 1].v) > p_.gear_v_threshold)
    mag = std::min(mag, std::fabs((double)pts_[i + 1].v));
```

取目标速度幅值时，会在前视 6 m 内取同挡段 $|v|$ 的最小值作为前瞻减速（对应第 16 章 S-T 图的剖面）。但换挡刹停点的 $v \approx 0$ 也在这 6 m 内——不跳过的话，最小值恒为 0，被地板 `speed_floor_mps = 0.5` 兜住，**车永远以 0.5 m/s 爬行，永远走不到换挡点**。

三个死锁，指向同一条工程铁律：**在多段/换向轨迹上，「状态量」和「几何量」必须成对地同步更新**。任何一处 s 推进、挡位切换、前馈取值的目标点来源不一致，故障就会以「死锁」而非「误差」的形式出现——因为系统不是算错了，而是算对了却在等一个永不到来的条件。

### 4.6 闭环测试：三个机动工况都在 CI 里

`tests/test_maneuver_tracker.cpp`（407 行，注册为 ctest `maneuver_tracker_tests`）用手写的运动学自行车模型（`step_bicycle`，`:72-80`）跑闭环：

| 测试 | 断言 |
|---|---|
| `test_uturn_closed_loop` | 400 tick 内换向 2~10 次，出现过 REVERSE，REVERSE 后回到 D，$\lvert h - \pi \rvert < 0.3$，$y \in [-7, 7]$ |
| `test_parking_closed_loop` | 出现过 REVERSE，换向 $\ge 1$ 次 |
| `test_parallel_parking_closed_loop` | 换向 $\ge 1$ 次（三个机动场景的通用性验证） |
| `test_segment_boundary_gate` | 未满足 `heading_gate_rad` 前，`advanceS` 不越过 D/R 边界 |
| `test_target_speed` | 6 m 内 $\min\lvert v\rvert$、地板、符号 |
| `test_end_stop` | $s \ge$ 轨迹末端时 `target_speed == 0`，`gear == 1` |

测试里有个容易忽略但至关重要的辅助函数——`downsample64()`（`:42-68`）。规划侧下发 101 个轨迹点，跟踪器的 `kMaxPts` 只有 64，必须降采样。**但普通的等间隔抽稀会跨越 v 符号翻转的边界**，把前进段的末点和倒车段的首点凑在一起，产生一段物理上不存在的轨迹。`downsample64` 保证符号翻转点一定是采样点，从而保住 D/R 边界。

---

## 5. LTV-MPC：一个默认关闭的 3 状态仿射 LQR

### 5.1 先说结论：它不是 QP

上一章（[`15_trajectory_planning.md`](file:///home/caixuf/code/FlowEngine/docs/book/15_trajectory_planning.md)）已经澄清了规划节点的真实 QP 调用边界。控制层的 MPC 需要更彻底地澄清：

> [!IMPORTANT]
> **全仓库不存在任何 QP 求解器。** 没有 OSQP、没有内点法、没有 qpOASES、没有 ADMM。`scripts/setup_algo_deps.sh` 里那句 `install_if_missing "osqp" "libosqp-dev"` 和它引用的 `src/plugins/control_osqp.cpp` 都是**空头支票**——那个文件根本不存在。`third_party/tinympc/`（一个完整的 ADMM MPC 实现，支持 box/SOC/linear 约束）确实被 clone 进了仓库，但在 `CMakeLists.txt` 里**一个字都没提**，从未被编译。
>
> `ltv_mpc_solve` 的真实身份：**3 状态、1 控制的离散仿射 LQR**，用后向 Riccati 递推闭式解算，复杂度 $O(N)$，无迭代、无动态内存。论文里管这个叫 MPC，因为它每帧重新线性化并滚动重算；实际上它能做的事，恰好是一个**无约束** LQR 能做的事。

### 5.2 状态定义与线性化

状态和控制（[`include/ltv_mpc.h:11-13`](file:///home/caixuf/code/FlowEngine/include/ltv_mpc.h)）：

$$x = \begin{bmatrix} e_y \\ e_\psi \\ \delta \end{bmatrix}, \qquad u = \begin{bmatrix} \dot\delta \end{bmatrix}$$

**不是四状态**。教科书写 MPC 横向控制的典型 $X = [e_y, \dot e_y, e_\psi, \dot e_\psi]$ 在这里不存在，因为控制量选的是**转向角速率** $\dot\delta$ 而非转角 $\delta$——$\delta$ 被放进了状态向量。

这个选择有两个后果：状态转移矩阵是 $3\times3$ 而不是 $4\times4$；而且 $\delta$ 作为状态天然继承了「转角本身」这层平滑，而 $R$ 罚的是速率，MPC 输出的也是速率，调用点必须积分。

线性化在 `ltv_mpc.c:178-190`：

```c
/* e_y[k+1]   = e_y[k]   + dt*(v*e_psi + 0.5*v*delta) */
/* e_psi[k+1] = e_psi[k] + dt*(0.5*v/L*delta - kappa*v) */
/* delta[k+1]= delta[k]+ dt*ddelta */
A[0][0] = 1.0;  A[0][1] = v_safe * dt;  A[0][2] = 0.5 * v_safe * dt;
A[1][1] = 1.0;  A[1][2] = v_safe / L * dt;   /* 与 plant v/L·tanδ 一致 */
A[2][2] = 1.0;
B[0] = 0.0;  B[1] = 0.0;  B[2] = dt;
c[0] = 0.0;
c[1] = -kk * v_safe * dt;    /* 曲率前馈 */
c[2] = 0.0;
```

三个要点：

- **这是运动学模型，不是动力学模型。** 只有 `dt`、`v`、`L`、`κ` 四个量参与，**没有质量、没有转动惯量 $I_z$、没有侧偏刚度 $C_f/C_r$**。那些参数只存在于 FlowSim 的被控对象里（`modules/adas_nodes/flowsim/physics.cpp`）。所以这个 MPC 预测的是「车会往哪走」，不是「力矩怎么转」。
- **`A[1][2]` 那个 0.5 曾经写错过**。注释留了案底：「旧 0.5 低估 2×横摆 authority」。注意 `A[0][2]` 的 0.5 是对的（一阶泰勒展开的积分项），`A[1][1]` 行的 `A[1][2]` 不该有 0.5——$\dot e_\psi = \frac{v}{L}\delta - \kappa v$ 里 $\delta$ 前面没有 0.5。
- **`v_safe` 是防除零的地板**：`v_safe = (vk < 0.01) ? 0.01 : vk`（`:175`、`:294`）。$v \to 0$ 时 $A$ 退化，控制增益也会跟着退化——低俗地说，这个 MPC 在静止时基本没有控制权。

### 5.3 代价与 Riccati 递推

代价矩阵（`ltv_mpc.c:154-166`）：

$$J = \sum_{k=0}^{N-1}\left(x_k^\top Q x_k + u_k^2 R\right) + x_N^\top Q_f x_N$$

$$Q = \mathrm{diag}(q_y,\ q_\psi,\ q_\delta), \quad R = r_{\dot\delta}, \quad Q_f = \mathrm{diag}(q_{f_y},\ q_{f_\psi},\ 0)$$

默认值（`ltv_mpc_default_config`，`:114-133`）：

| 参数 | 默认 | 标定备注 |
|---|---|---|
| `q_y` | 10.0 | 横向误差 |
| `q_psi` | **80.0** | 航向误差，高权重抑制摇摆 |
| `q_delta` | 1.0 | 转角本身 |
| `r_ddelta` | **1.0** | 速率惩罚，抑制抖动 |
| `qf_y` | 20.0 | 终端（×2） |
| `qf_psi` | **160.0** | 终端（×2） |
| `horizon` | 60 | 60 × 0.025 = **1.5 s** |
| `dt` | 0.025 | 匹配 40 Hz 控制周期 |
| `wheelbase` | 2.7 | |
| `max_steer` | 0.16 | 运行时被逐帧覆盖 |
| `max_dsteer` | 0.5 | rad/s |

注释说明了权重取向：「q_psi/qf_psi 高权重点头抑制航向摇摆，r_ddelta=1.0 抑制速率抖动」（`:116-117`）。

**预测时域是 60 步 = 1.5 秒**，不是常见的 10~20 步。原因和规划层一致：转向动作的物理延迟（转向电机、轮胎松弛、车身侧倾）加起来超过半秒，1.5 秒的时域才能让优化器「看见」转过去之后会怎样。

### 5.4 递推主体：一次修复，两处错误

后向 Riccati 递推（`ltv_mpc.c:194-272`）的形式是标准 affine-LQR：

```c
Quu     = R + B'·P·B;                     /* :197 */
if (fabs(Quu) < 1e-12) return LTV_MPC_ERR_SINGULAR;   /* :198 奇异保护 */
K[k]    = -Quu_inv · B'·P·A;             /* :207-209  (1×3) */
kff[k]  = -Quu_inv · B'·(P·c + p);       /* :217-221  前馈 */
A_cl    = A + B·K;                       /* :224-228 */
P_k     = Q + A_cl'·P_{k+1}·A_cl + K'·R·K;   /* :239-258 */
p_k     = A_cl'·(P_{k+1}·c + p_{k+1});       /* :260-273 */
```

其中 `p` 递推那段留了一段极有价值的注释（`:230-234`）：

```c
/* 在更新 solver->P 之前保存 P_{k+1} / p_{k+1}，下方更新 p_k
 * 时必须用 P_{k+1}（不是更新后的 P_k）。原始实现两处都错：
 *   1. 用 solver->P（已被覆盖为 P_k）当作 P_{k+1}
 *   2. 漏掉 + p_{k+1} 项
 *   3. 多了 K'R·kff（标准 Riccati p 递推没有这一项） */
```

三条都是 Riccati 递推的经典陷阱，值得记下来：

1. **原地覆盖**：`solver->P` 被用作工作区。标准递推要读 $P_{k+1}$ 算 $k_{ff}$ 和 $P_k$、再读 $P_{k+1}$ 算 $p_k$，任何一处顺序写错都会引入难以定位的偏差。
2. **漏掉 affine 项**：曲率前馈 $c_k \ne 0$ 时，标准 LQR 的 $K$ 增益**完全不变**（因为 $c$ 只是常数偏置，不改变状态转移），变的只有前馈 $k_{ff}$。所以第 1 条的错（用 $P_k$ 算 $k_{ff}$）在 $\kappa = 0$ 时**测不出来**。
3. **不该有的项**：$p_k$ 的递推是 $A_{cl}^\top(P_{k+1}c + p_{k+1})$，没有 $K^\top R k_{ff}$。加了它属于引入伪耦合。

> **第 2 条解释了测试为什么必须覆盖 $\kappa \ne 0$。** `modules/adas_nodes/test_ltv_mpc.c` 的**用例 3**（文件里自带一句注释「**暴露 p 递推 bug 的关键场景**」）专门测 $\kappa = 0.005$，用黄金参考值 `KFF0_KAPPA005 = 0.070914650933649` 卡死精度，容差 $10^{-9}$。**如果测试只在 $\kappa = 0$ 的直道上跑，这个 bug 永远不会暴露。**

### 5.5 约束是截断，不是约束

「约束」出现在两个地方（`:282-303`）：

```c
/* rollout: u = K[k]·x + kff[k] */
if (u >  max_dsteer) u =  max_dsteer;      /* 速率饱和 */
if (u < -max_dsteer) u = -max_dsteer;
...
if (delta >  max_steer) delta =  max_steer; /* 转角饱和 */
if (delta < -max_steer) delta = -max_steer;
```

**这是解完之后做 clip，不是在优化里把 $u$ 限制在可行域内。** 差别是实质性的：无约束解算出 $u = 2.0$、上限 0.5，截断后得到 0.5；而真正的约束优化会知道「$u$ 顶到 0.5 了，剩下的横向误差得靠其他代价项重新权衡」，可能选择不同的整条轨迹。

对单输入系统（$u$ 是标量）而言，事后饱和的损失比看起来小——因为每个时刻只有一个决策变量被裁剪。但如果这个 MPC 将来要加第二个输入（比如同时输出驱动/制动的横向-纵向解耦），截断就会变成一个严重的问题。

### 5.6 调用点：默认关闭

```cpp
bool mpc_used = false;
if (g.use_ltv_mpc && !g.maneuver_mode && g.has_planning && g.ref_path.size() > 1) {
```

四个门控条件。第二个尤其重要——**机动模式下 MPC 被整体跳过**，注释解释了原因（`:922-924`）：

```cpp
/* ── LTV MPC 横向控制 ──
 * 机动模式（掉头/倒车）跳过：MPC 线性化假设小转角误差动力学，
 * 满舵弧 + 倒挡在其模型域外，直接走 Stanley + kappa 前馈。 */
```

满舵 0.60 rad 远超 $A$ 矩阵线性化在 $\delta \approx 0$ 处的有效范围，$v < 0$ 时 $\dot e_\psi = \frac{v}{L}\tan\delta$ 的符号关系也和模型假设不符。**在模型的适用域外用一个模型，是控制工程里最典型的翻车方式。**

然后是三个曾经踩过的坑，全都有注释记录：

```cpp
/* - max_steer 按当前车速经 steer_limit_for_speed(1.4) 取巡航
 *   包络（与 Stanley 回退同口径），MPC 才不会误以为有超出
 *   执行器权限的舵量（此前硬编码 0.35，是真实权限的 2~8 倍）。 */
...

/* e_psi 约定 = ego_heading − ref_heading（左为正），与求解器
 * 模型 ė_y=v·e_psi 自洽；旧代码取反（ref−ego）导致航向反馈
 * 变正反馈，MPC 一启用就来回甩（2026-08-15 定位）。 */
double e_psi = heading_error;
...

/* mpc_steer_delta 是转向速率(rad/s)，须按步长积分得到本周期
 * 转角增量（修复：此前漏乘 dt，每帧舵量被放大 ~20×）。 */
steer = g.prev_steer + mpc_steer_delta * g.ltv_mpc_cfg.dt;
```

第二个坑最值得警惕：**符号约定与模型不自洽，等于把负反馈接成了正反馈。** 模型的 $\dot e_y = v\,e_\psi$ 要求 $e_\psi$ 定义为「车头相对参考线的**左偏**角」，而代码原本取了反。误差反馈一旦变成正反馈，MPC 表现得比什么都不做还糟——这大概就是它默认关闭的直接原因之一。

### 5.7 为什么默认关闭：变道场景 3/3 失败

`control_node.cpp:181-184`：

```cpp
/* 2026-08-15 决策：MPC 默认关闭，Stanley 保持默认横向控制器 */
int use_ltv_mpc{0};
```

**没有任何 `config/*.json` 打开过它。** 唯一能打开的入口是 JSON 里的 `ltv_mpc_enable`（`:1413-1414`），而全仓库对这个键的引用只有 `control_node.cpp` 自己和 `docs/HANDOFF_2026-09-21b.md`。`config/pipeline.json:291` 甚至写了 `"mpc_horizon": 0`。

关闭的原因记录在交接文档里：8 个场景的回归中，**`lane_change_traffic` 三次全部失败**——碰撞、横向偏出 6.43 m（阈值 4.5 m）、以及 `comfort_jerk_max = 213 m/s³`（Stanley 方案约 9 m/s³）。

**213 m/s³ 意味着什么**：急刹时加加速度约 9 m/s³，而 213 是它的 23 倍，这个量级的 jerk 在物理上根本不可能被车辆执行——传感器和执行器根本跟不上。真要理解为什么 MPC 的横向输出会这么抖，得回到 5.6 节的 `lat_env` 自适应包络：

```cpp
/* 机动自适应转向包络（2026-08-15 仿真标定）：大横向误差
 * （变道/避障）用 2.4 m/s²（与 ROAD_GUARD 同档），巡航小误差
 * 用 1.4 m/s² 舒适包络。统一 1.4 在 12m/s 只给 0.026rad，
 * 变道最小可行时间贴边 + 增量式控制慢半拍 → 饱和摇摆
 * （control_sim obstacle 场景复现：y 超调 ±2m 甩到对向车道）。 */
const double lat_env = (fabs(e_y) > 0.5) ? 2.4 : 1.4;
```

**MPC 是增量式控制**（输出 $\dot\delta$，调用点积分），每个周期只改一小步。当转角长期顶在包络边界上时，积分器的输出会持续累积直到饱和并来回摇摆——「变道最小可行时间贴边 + 增量式控制慢半拍」说的就是这个。CRC 没把它整死，但也没让它比 Stanley 更好。

这个模块现在的定位很清楚：**它是实验性的、默认关闭的、不在生产路径上的**。`modules/adas_nodes/test_ltv_mpc.c`（468 行，ctest 名 `ltv_mpc_kkt`）用 12 个用例守着它的数值正确性（黄金增益 `K0_REF`、$\kappa\ne0$ 回归、horizon 收敛、奇异/NaN/重复求解稳定性），但**没有一个测试覆盖它接进控制闭环之后的行为**——那才是它挂掉的地方。

---

## 6. 降级与安全覆盖：控制指令发布前的四道闸

控制节点自己也是一道安全防线。发布 `control/raw_cmd` 之前，有四层覆盖会直接改写指令：

### 6.1 数据超时

`control_node.cpp:556-635` 的 `DATA_TIMEOUT` 分支：定位或轨迹超过 1000 ms 未更新，**不发布正常指令，而是发一帧 `brake = 0.25` 的降级指令**，并用 1.4 m/s² 的横向权限做开环回正，然后 `continue`。

降级时不发正常指令这一点很关键——继续用陈旧的目标去控制一个已经不确定位置的车，比什么都不做更危险。

### 6.2 近边限幅

```cpp
/* y_from_target > 4.5m 且非机动 → 限幅到 0.165 rad */
```

横向偏差过大时收紧限幅，防止因为一个错误的大误差指令而把车猛地甩出。

### 6.3 ROAD_GUARD：最后一道不许讲道理的闸门

```cpp
#define ROAD_GUARD_THRESHOLD_M 3.0    /* control_node.cpp:70 */
```

`control_node.cpp:1126-1148`：距目标车道中心超过 3.0 m 时，无视所有控制逻辑，直接强制回正——

- 转向：`steer = ±steer_limit_for_speed(|v|, 2.4)`，**朝道路中心方向打满**；
- 纵向：$|v| < 2.5$ 给 0.18 油门（爬回路面），否则给 $\ge 0.65$ 刹车（先停下来）；
- 模式标记 `"ROAD_GUARD"`，这个字符串会随 `ControlRaw` 发出去，出现在实车日志里。

为什么需要一道「不讲道理」的闸？因为上游的**所有**纠偏逻辑——横向 PD、MPC、机动跟踪器——都是**目标误差的函数**。当自车定位已经严重偏离目标车道时，它们给出的仍然是「朝目标点的最优修正方向」，而目标点本身可能就在另一个街区。让目标退化成一个**方向**（「往右打」），比继续信任一个已经不可信的目标点更安全。

注释也说明了阈值的定位（`:68-70`）：

```cpp
/* ROAD_GUARD 触发阈值: 距道路中心超过此值强制回正。车道判定已移到 planning，
 * 此阈值仅为安全冗余，保持 control 独立于 lane_width/lane_count 配置。 */
```

**这是一层独立的安全冗余**：车道判定归规划层管，但控制层不能假设规划层永远正确。

### 6.4 降级阶梯标记

`control_node.cpp:771-812` 实现了 MRM（最小风险策略）阶梯：

- **L3**（Level 3 降级）：目标速度与 `acc_target` 直接置 0，同时清零 PID 积分，全刹停车；
- **L2**（Level 2 降级）：把 `acc_target` 压到 `l1_speed_limit`（若已设置）或 3.0 m/s 兜底，**爬行，不停**；
- **3 秒后自动恢复**：计时的**前提是车已经停住**（`|v| < 0.5` m/s，源码里叫 `stalled`），停稳满 3 秒才 `degrade_clear()`。逻辑是「停稳了 + 故障条件应该已经消失 → 重新起步」，而不是「故障持续 3 秒就撤销降级」。L2 和 L3 各有一份独立的计时器。

```cpp
} else if (ds->degrade_level >= DEGRADE_L2) {
    double lim = ds->l1_speed_limit > 0.0 ? ds->l1_speed_limit : 3.0;
    if (acc_target > lim) acc_target = lim;   /* 爬行，不停 */
    /* L2 自动恢复：停稳 3s 后清降级。与 L3 同理：
     * 碰撞后 safety_control 设 L2，车已停但降级永不… */
    if (stalled) {
        g.mrm_stall_us += CONTROL_DT_S * 1e6;
        if (g.mrm_stall_us > 3000000.0) {
            degrade_clear();
            g.mrm_stall_us = 0;
```

降级期间还会给模式名加 `MRM`（机动期是 `MRM+MANEUVER`）标记，防止下游安全节点把机动轨迹误判。`control_node.cpp:1079` 还会向 `safety/status` 发 req/reply 握手。**设计原则：控制层可以在降级状态下继续运行，但其权限被显式收窄，并且这个状态是可见的。**

### 6.5 一个附带的输出

顺手提一下 `control/ldw`（车道偏离预警，`control_node.cpp:1259-1277`）：`ldw_threshold = 0.5` m、`ldw_min_speed = 1.0` m/s、`ldw_cooldown = 2.0` s。它是**纯监控**——只报警，不干预，报警后也不让控制逻辑改变。

> [!TIP]
> 报警与干预分离是个好设计。LDW 如果同时改了控制量，那么一次误报就会引发一次不受控的动作；只报警、让人类判断，把控制权干净地留在控制器这边。

---

## 7. 源码与资源对照

| 模块 / 层次 | 源码文件路径 | 核心符号 / API | 架构职责与设计要点 |
|---|---|---|---|
| **控制节点宿主** | [`modules/adas_nodes/control_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/control_node.cpp) | `ControlTask::run`（`:506`）<br>`steer_limit_for_speed`（`:191`）<br>`ControlRaw` 发布（`:1171-1189`） | 40 Hz 消息驱动循环、纵向 PID、横向级联 PD、四层安全覆盖；全部参数经 `param_registry` 每帧热重载 |
| **横向级联** | 同上，`:999-1061` | `cte_term` / `heading_term` / `yaw_damp_term` / `ff_term` / `delta_ff` | 横向速度 PD → ψ_des → δ 三级级联；`speed_eff = max(\|v\|, 3.0)` 截断式软化；曲率前馈 1.5× 增益 |
| **纵向 PID** | 同上，`:854-920` | `pid_error` / `g.integral`<br>`SHIFT_STOP`（`:893`） | 不对称归一化（5000/8000）、三重抗饱和、倒车镜像、换挡全刹 |
| **LTV MPC** | [`include/ltv_mpc.h`](file:///home/caixuf/code/FlowEngine/include/ltv_mpc.h)<br>[`src/algorithms/ltv_mpc.c`](file:///home/caixuf/code/FlowEngine/src/algorithms/ltv_mpc.c) | `ltv_mpc_solve`（`.c:143`）<br>`ltv_mpc_set_state` / `ltv_mpc_set_reference`<br>`ltv_mpc_default_config`（`.c:114`） | 3 状态 1 控制仿射 LQR，后向 Riccati 递推，$O(N)$ 无迭代；**非 QP，约束为事后截断**；**默认关闭** |
| **机动跟踪器** | [`modules/adas_nodes/maneuver_tracker.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/maneuver_tracker.h) | `maneuver::ManeuverTracker::tick`（`:228`）<br>`advanceS`（`:131`）<br>`updateGear`（`:197`） | header-only 弧长跟踪器；发散/段边界双门限；倒挡反馈反号；三个死锁修复 |
| **执行器映射** | [`modules/adas_nodes/pwm_map.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/pwm_map.c)<br>[`modules/adas_nodes/actuator_node.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/actuator_node.c) | `pwm_map.h::PWM_MAX_STEER_RAD`（0.22）<br>`MAX_STEER_RAD`（0.22f） | `steer_norm = steering_rad / 0.22` → 舵机微秒 `1500 ± norm·scale`；`brake > 0.01` 时优先于 `throttle` |
| **消息定义** | [`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) | `ControlRaw`（`:189-202`）<br>`Gear`（`:33-35`） | 转向角全链路为 **rad**，油门/刹车为 **[0,1] 归一化**，全程无角度制转换 |
| **测试** | [`modules/adas_nodes/test_ltv_mpc.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/test_ltv_mpc.c)<br>[`tests/test_maneuver_tracker.cpp`](file:///home/caixuf/code/FlowEngine/tests/test_maneuver_tracker.cpp) | ctest `ltv_mpc_kkt`（12 用例）<br>ctest `maneuver_tracker_tests`（6 用例） | MPC 数值正确性由黄金增益锁死；机动跟踪器跑运动学闭环验证三个泊车场景 |
| **Python 参照** | [`tools/control_sim.py`](file:///home/caixuf/code/FlowEngine/tools/control_sim.py) | `LtvMpcSolver`（`:113`）<br>`stanley_control`（`:306`） | 离线扫参工具；**注意其 MPC 权重与 C 实现不一致，且 p 递推仍是修复前的版本** |

---

## 8. 思考题

1. **控制层限幅与规划层预算的契约形式化**：
   第 3.5 节表明，巡航限幅 1.4 → 4.0 的修复，本质是让 `steer_limit_for_speed` 的 `a_max` 覆盖规划层的 $5.0 \times 0.85 = 4.25\text{ m/s}^2$。但这个对应关系目前是**散落在注释里的口头约定**，没有任何代码在两层之间做校验（`planning_node` 不知道 `control` 的限幅函数，`control` 不知道 `st_graph.h` 的 `STG_A_LAT_MAX`）。请设计一个机制：让规划层在发布轨迹时声明它所依据的横向加速度预算，控制层在订阅时校验 `预算 ≤ 本层限幅`，并在校验失败时主动降级或报警。追问：这个契约应该放进 `ControlRaw` 消息、单独的握手话题，还是构建期（CMake）的静态断言？

2. **速度地板 3.0 m/s 的低速失效**：
   3.3 节的 `speed_eff = fmax(|v|, 3.0)` 在低速时**完全冻结了横向纠偏增益**。这对「宽 4 m 的大曲率路口 + 3 km/h 的蠕行」是一个真实的能力缺口——车偏出半个车身宽度时，控制器无法纠正。
   请评估两种改法：(a) 把地板降到 0.5 m/s，但在 $v \to 0$ 时引入 $v$ 相关的增益调度 $k(v) = k_0 \cdot \max(v, v_{min})$；(b) 保留地板，但增加一个**位置环**——当 $|e_y| > 1.0$ m 时，绕过级联 PD，直接用 `atan2(lat_kp·|e|/v_min, ...)` 之类的低速积分器纠偏。哪一种更不容易引入低速时的转向抖动？为什么？

3. **把事后截断改成真约束**：
   5.5 节指出 `max_dsteer` / `max_steer` 是解完之后 clip 的，并说明单输入系统下损失可控。假设现在把 MPC 升级为**双输入**（横向 $\dot\delta$ + 纵向 $\ddot v$，用于弯道速度预瞄和横向解耦）。
   (a) 推导在 $\lvert u_\delta \rvert \le u_{\max}$、$\lvert u_v \rvert \le u_{\max,v}$ 下的闭式解需要什么样的 Riccati 变体？
   (b) 仓库里已有 `pjqp_smooth_2d` 的五对角 LDLᵀ 求解器（见第 16 章），它能否直接承载这个问题？会遇到什么维度和结构上的困难？
   (c) 或者——是否应该放弃把 MPC 变成通用优化器，转而论证「在运动学自行车模型 + 单输入 + 无约束的场景下，无约束 LQR + 事后饱和**已经**是最优解」？请给出这个论证成立所需的前提条件，以及哪一条前提在变道场景中被破坏了。

4. **三处死锁的共同结构**：
   4.5 节的三个死锁（倒车画圆、换挡边界符号、短机动 min\|v\| 被地板吃掉）在算法上相距甚远，但在**失效模式**上高度同构：状态量与几何量失步 → 反馈符号错误或条件永不满足 → 系统静默锁死（而非发散或报警）。
   请提炼这个失效模式的**通用检测手段**。具体地：假设你在 `ManeuverTracker` 里加一个监控，当 $s$ 连续 $N$ 周期不推进（推进量 < 阈值）时判定为「疑似锁死」，并触发降级。
   (a) 这个检测会不会误报？什么样的合法工况会导致 $s$ 不推进但系统完全健康？
   (b) 触发降级后应该怎么恢复？直接把 $s$ 往前推是危险的（4.5 节的修复就是这么做的，且只在已知条件下安全）；有没有更保守的恢复策略？
   (c) 反过来，有没有**应该**让它继续锁死、不做任何干预的场景——即「锁死反而是正确行为」？
