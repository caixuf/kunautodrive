<!-- @archive-banner -->

> **归档说明**：本章是 docs/book v1（2026-09 前后"按源码逐条重建"产物）的版本。
> 内容大量绑定代码行号、文件名、函数符号，与代码强耦合 —— 维护成本高且易过时。
> v2 重写计划：见 `docs/book/README.md` 的写作风格约束 + 真技术书范式。
> 本归档文件保留供历史参考；引用时用 `docs/_archive/book_v1/` 而非 `docs/book/`。

# 第 16 章：Frenet 轨迹规划 —— 网格枚举、S-T 动态规划与扫掠防撞

> **本章导读**：
> 笔直的路上，用 $(X, Y)$ 说清车在哪儿就够了。可路一弯起来，道路自身的曲率会把约束搅成非线性，横向和纵向缠在一起解不开。经典的绕法是把问题搬进 **Frenet 曲线坐标系** $(s, d)$——沿参考线走了多远是一个轴，偏离参考线多少是另一个轴，两个维度当场分开。
>
> 本章讲 KunAutoDrive 怎么落地这件事：[`modules/adas_nodes/planning_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/planning_node.cpp)（3124+ 行，20 Hz）。三块内容：
>
> 1. **参考线投影与车道映射**——把笛卡尔坐标变成 $(s, d)$；
> 2. **Frenet 轨迹生成**——`third_party/frenet_planner` 那套 zhmzhu 算法；
> 3. **S-T 图动态规划**——纵向速度剖面（`st_graph.c`），以及扫掠碰撞检测（`traj_safety.c`）。
>
> 先破除旧稿里几处会误导实现的描述：
>
> - **「解一个 $6\times6$ 线性方程组」**——方向对，但漏了关键：横向是**五次**、纵向是**四次**，而且系数用 **Eigen 的 LU 分解**解，不是手写高斯消元。
> - **「先粗搜一遍、再用 QP 精修」**——**两段都不存在**。整个系统**没有任何 QP 求解器**。横向是暴力网格枚举（约 2079 条候选），纵向是 S-T 图上的动态规划。
> - **「$|a| \le 3.0$ 的 QP 约束」**——S-T DP 里的加速度上限是 `STG_A_MAX = 4.0`。
> - **「$100\sim150$ ms 的轨迹拼接」**——真实阈值是 `dt < 500 ms`、`位置差 < 2.0 m`、`速度差 < 3.0 m/s`。
> - **「曲率平滑 / 退回笛卡尔坐标系」**——都没有。奇异点用的是**参考线回退路径**（`world_to_frenet` 失败时）。

---

## 1. 参考线投影：最近点，不是 Frenet 变换

### 1.1 五个字段

[`modules/adas_nodes/planning_coordinates.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/planning_coordinates.h)（87 行，header-only，`namespace planning_coord`）：

```cpp
10: struct Projection {
11:     double s{0.0};        // 沿参考线的纵向弧长
12:     double d{0.0};        // 垂直参考线的横向偏移（左正右负）
13:     double ref_x{0.0};    // 投影垂足世界坐标
14:     double ref_y{0.0};
15:     double heading{0.0};  // 投影垂足处的切线航向角
16: };
```

### 1.2 `project_to_path` 的真实实现

`:39-72`，逐段最近点投影：

```cpp
39: inline bool project_to_path(double x, double y,
40:                             const double* ref_x, const double* ref_y,
41:                             const double* ref_s, int count,
42:                             Projection& out) {
43:     if (!ref_x || !ref_y || !ref_s || count < 2) return false;
44:
45:     double best_dist2 = 1e300;
46:     bool found = false;
47:     for (int i = 0; i + 1 < count; ++i) {
48:         const double vx = ref_x[i + 1] - ref_x[i];
49:         const double vy = ref_y[i + 1] - ref_y[i];
50:         const double len2 = vx * vx + vy * vy;
51:         if (len2 <= 1e-9) continue;          // 跳过退化段
52:
53:         const double t = std::clamp(
54:             ((x - ref_x[i]) * vx + (y - ref_y[i]) * vy) / len2, 0.0, 1.0);
55:         const double px = ref_x[i] + t * vx;
56:         const double py = ref_y[i] + t * vy;
57:         const double dx = x - px;
58:         const double dy = y - py;
59:         const double dist2 = dx * dx + dy * dy;
60:         if (dist2 >= best_dist2) continue;
61:
62:         const double heading = std::atan2(vy, vx);
63:         out.s = ref_s[i] + t * std::sqrt(len2);
64:         out.d = -dx * std::sin(heading) + dy * std::cos(heading);
65:         out.ref_x = px;
66:         out.ref_y = py;
67:         out.heading = heading;
68:         best_dist2 = dist2;
69:         found = true;
70:     }
71:     return found;
72: }
```

四个公式：

$$t = \text{clamp}\left(\frac{(\mathbf{p} - \mathbf{r}_i)\cdot\mathbf{v}_i}{\lVert\mathbf{v}_i\rVert^2},\ 0,\ 1\right)$$

$$s = s_i + t\cdot\lVert\mathbf{v}_i\rVert, \qquad \theta = \arctan2(v_{iy}, v_{ix})$$

$$d = -\Delta x \sin\theta + \Delta y \cos\theta$$

**`d` 的符号约定**：残差向量在**左法向**上的投影，**正值 = 参考线左侧**。

### 1.3 四个必须知道的性质

**其一：这不是真正的 Frenet 变换，是最近点投影。**

$t$ 被 clamp 到 $[0,1]$——**不外推**。这意味着：

- 点在参考线**端点之外**时，投影被拉到端点上，不会算外推的 $s$；
- **自相交/回折的路径**上，同一个笛卡尔点可能有多个 $(s,d)$，取的是**最先命中的那一段**（`:60` 的 `dist2 >= best_dist2` 让并列时保留靠前的段）。

真正的 Frenet 变换需要沿法线求交点，而最近点投影是它的**简化近似**。在参考线曲率半径 $R$ 大于 $|d|$ 时两者等价；$|d| > R$ 时最近点投影仍然有定义（不会崩），但给不出唯一解。

> [!NOTE]
> **旧稿说「真撞上 $|d| > R_{\min}$ 就退回笛卡尔坐标系直接规划」——代码里没有这条路径。** 实际做法是 `project_to_path` 失败（返回 `false`）时走**车头坐标系兜底投影**（行为节点 `:805-810`、规划节点各有类似分支），那仍然不是笛卡尔规划，只是换了个投影方式。

**其二：弧长用的是弦长，不是 `ref_s` 的间距。**

```cpp
out.s = ref_s[i] + t * std::sqrt(len2);
```

`ref_s[i]` 只作为**每点的偏移锚点**，段内增量用的是欧氏弦长 $|\mathbf{v}_i|$。如果调用方传的 `ref_s` 数组和实际点距不一致，`s` 会**累积漂移**。

实践中两者是一致的——`behavior_planner_node.cpp:596` 和规划节点的 `ref_s` 都是用同样的 `hypot` 累加出来的——但这是个**没有写在任何地方的隐含不变量**。

**其三：并列时取靠前的段。**

`:60` 的 `if (dist2 >= best_dist2) continue;` 是严格小于才更新。距离完全相同时保留先遍历到的那段。这在路口附近（两段参考线几乎重合）会有影响。

**其四：退化段静默跳过。**

`:51` 的 `len2 <= 1e-9` 跳过零长度段。如果**所有**段都退化，`found` 保持 `false`，函数返回 `false`。

### 1.4 车道映射：三个纯函数

```cpp
18: inline double lane_center_d(int lane_idx, int lane_count, double lane_width) {
19:     return -(lane_idx - (lane_count - 1) * 0.5) * lane_width;
20: }

22: inline int first_legal_lane(int lane_count, bool road_oneway) {
23:     return road_oneway ? 0 : lane_count / 2;
24: }

26: inline int nearest_lane(double d, int lane_count, double lane_width,
27:                         bool own_side_only = true) {
28:     if (lane_count <= 0 || lane_width <= 0.0) return 0;
29:     double raw = (-d) / lane_width + (lane_count - 1) * 0.5;
30:     int lane = static_cast<int>(raw >= 0.0 ? raw + 0.5 : raw - 0.5);
31:     lane = std::max(own_side_only ? lane_count / 2 : 0, lane);
32:     return std::min(lane_count - 1, lane);
33: }
```

**`lane_center_d` 的符号**：$d = -\left(i - \frac{N-1}{2}\right)w$

代入 $N=4, w=3.5$：

| 车道索引 | `lane_center_d` |
|---|---|
| 0 | **+5.25** |
| 1 | +1.75 |
| 2 | −1.75 |
| 3 | −5.25 |

**索引 0 是最左车道**（$d$ 最大）。

**`nearest_lane` 的 `raw` 是 `lane_center_d` 的精确代数反解**：

$$\text{raw} = \frac{-d}{w} + \frac{N-1}{2}$$

`own_side_only = true`（默认）时下界被钳到 $\lfloor N/2\rfloor$——**双向道路时禁止返回对向车道**。这和第 15 章讲的 `first_legal_lane` 是同一个判据。

### 1.5 五次平滑：不是六次方程组

`:74-83`：

```cpp
74: inline bool quintic_lane_change(double start_d, double target_d,
75:                                 double length, double s, double& out_d) {
76:     if (length <= 1e-6) return false;
77:     const double delta = target_d - start_d;
78:     if (std::fabs(delta) <= 0.2 || std::fabs(delta) >= 8.0) return false;
79:     const double u = std::clamp(s / length, 0.0, 1.0);
80:     const double blend = u * u * u * (10.0 + u * (-15.0 + 6.0 * u));
81:     out_d = start_d + delta * blend;
82:     return true;
83: }
```

$$\text{blend}(u) = 10u^3 - 15u^4 + 6u^5, \qquad u = \text{clamp}\!\left(\tfrac{s}{L}, 0, 1\right)$$

**这就是标准的五次 smoothstep**，只不过自变量是**归一化弧长** $u = s/L$ 而非时间。展开成 $s$ 的多项式确实是 5 次，但**系数不通过解方程组得到**——它们是硬编码的 $10, -15, 6$。

两个拒绝条件值得注意：

| 条件 | 理由 |
|---|---|
| $\lvert\Delta d\rvert \le 0.2$ m | 横向位移太小，smoothstep 的加加速度峰值会很小，但整条曲线的形状变化没意义 |
| $\lvert\Delta d\rvert \ge 8.0$ m | 超过两条车道宽，曲线会横跨对向 |

规划节点在变道时用它（`planning_node.cpp:2242-2302`，长度 $L = 50.0$ m，见 `:2268`）。

---

## 2. Frenet 轨迹生成：暴力网格枚举

### 2.1 桥接层只是一层壳

[`src/algorithms/frenet_bridge.h`](file:///home/caixuf/code/FlowEngine/src/algorithms/frenet_bridge.h)（88 行）+ [`.cpp`](file:///home/caixuf/code/FlowEngine/src/algorithms/frenet_bridge.cpp) 提供 6 个 C 接口：

| 函数 | 声明 | 实现 | 生产调用点 |
|---|---|---|---|
| `frenet_create` | `:28` | `.cpp:34-62` | `planning_node.cpp:3005` |
| `frenet_set_reference_path` | `:35` | `.cpp:64-69` | `planning_node.cpp:389, 422, 435` |
| `frenet_set_obstacles` | `:43-45` | `.cpp:71-80` | **零调用者（死代码）** |
| `frenet_set_obstacles_v` | `:58-61` | `.cpp:82-98` | `planning_node.cpp:2000` |
| `frenet_plan` | `:75-79` | `.cpp:100-174` | `planning_node.cpp:2206` |
| `frenet_destroy` | `:82` | `.cpp:176-178` | `planning_node.cpp:3100` |

**`frenet_plan` 本身不做任何优化**，它只是一层 marshaller：

1. 填 `FrenetInitialConditions`（`:108-123`）
2. 算障碍物 AABB 角点（`:126-153`）
3. 调 `run_fot(&ic, &fh->hp, &rv)`（`:159`）
4. 提取结果（`:163-173`），遇到 `isnan(rv.x_path[i])` 就停
5. `if (!rv.success) return 0`（`:161`）

真正干活的是 `third_party/frenet_planner/src/FrenetOptimalTrajectory/`。

### 2.2 2 秒速度外推

`.cpp:126-153`：

```cpp
134: const double pred_horizon_s = 2.0;  /* 预测时域 (s),与 d_t_s 对齐 */
...
139: if (fh->has_velocity && (int)fh->ovx.size() > i) {
140:     px += fh->ovx[i] * pred_horizon_s;
141:     py += fh->ovy[i] * pred_horizon_s;
142: }
143: llx[i] = px - fh->ol[i] * 0.5;
144: lly[i] = py - fh->ow[i] * 0.5;
145: urx[i] = px + fh->ol[i] * 0.5;
146: ury[i] = py + fh->ow[i] * 0.5;
```

**障碍物中心被按自身速度外推 2 秒**，然后才生成 AABB。

理由是：`frenet_plan` 的预测时域 `maxt = 10.0` s，如果把障碍物按**当前位置**当静止障碍，规划到第 5 秒时它实际已经在别处了。2 秒是一个折中——覆盖最关键的近期决策，又不至于把远处的障碍推到离谱的位置。

> **`frenet_set_obstacles`（不带速度）是死代码。** 头文件 `:37-42` 自称「static version, backward-compatible」，但全仓库零调用。生产只用带速度的 `frenet_set_obstacles_v`。

### 2.3 一个静默的容量截断

```cpp
// frenet_bridge.cpp:129-130
static double llx[32], lly[32], urx[32], ury[32];
int actual = (no > 32) ? 32 : no;
```

**函数内的 `static` 数组，硬编码 32 个障碍物上限。**

而 `planning_node.cpp:1929` 分配的是 `ox[128]`——**它会老老实实传最多 128 个进去，然后第 33 个之后被静默丢弃。**

症状是「密集车流里前车没被规划器看见」，而且没有任何日志。

### 2.4 真正的算法：三级网格枚举

`third_party/frenet_planner/src/FrenetOptimalTrajectory/FrenetOptimalTrajectory.cpp:57-169`。**没有梯度、没有 QP、没有迭代。**

**横向：五次多项式**（`:75-77`）

```cpp
75: QuinticPolynomial lat_qp = QuinticPolynomial(
76:     fot_ic->c_d, fot_ic->c_d_d, fot_ic->c_d_dd, di, 0.0, 0.0, ti
77: );
```

六个边界条件：

| | 起点 $s=0$ | 终点 $s=T$ |
|---|---|---|
| $d$ | `c_d` | `di` |
| $d'$ | `c_d_d` | 0 |
| $d''$ | `c_d_dd` | 0 |

**系数用 Eigen 的 LU 分解解**（`QuinticPolynomial.cpp:3` `#include <Eigen/LU>`）。所以旧稿说「解 $6\times6$ 方程组」在**数学上是对的**，只是实现上依赖 Eigen。

**横向采样网格**（`:63-65`）：`di` 从 $-\text{max\_road\_width\_l}$ 到 $+\text{max\_road\_width\_r}$，步长 `d_road_w`。

**纵向：四次多项式**（`:107-109`）

```cpp
107: QuarticPolynomial lon_qp = QuarticPolynomial(
108:     fot_ic->s0, fot_ic->c_speed, 0.0, tv, 0.0, ti
109: );
```

五个边界条件：$s(0)=s_0$、$s'(0)=c_{\text{speed}}$、$s''(0)=0$、$s(T)=t_v$、$s'(T)=0$。同样用 Eigen LU（`QuarticPolynomial.cpp:3`）。

**为什么纵向是四次而横向是五次**：纵向只有 5 个约束（起点位置/速度/加速度 + 终点位置/速度），所以最低次数是 4；横向要保证加加速度连续，5 个约束不够，需要 6 个。

### 2.5 候选数量

`frenet_bridge.cpp:34-62` 的 `frenet_create` 设了 21 个超参数，其中决定网格规模的有：

| 参数 | 值 | 行 |
|---|---|---|
| `max_road_width_l` / `_r` | **6.0 / 6.0** m | `:43-44` |
| `d_road_w` | **1.5** m | `:45` |
| `mint` / `maxt` | **2.0 / 10.0** s | `:47-48` |
| `dt` | **0.25** s | `:46` |
| `d_t_s` | **2.0** m/s | `:49` |
| `n_s_sample` | **3.0** | `:50` |

算出来：

- **横向目标 $d_i$**：$\{-6, -4.5, -3, -1.5, 0, 1.5, 3, 4.5, 6\}$ = **9 个**
- **横向时域 $t_i$**：$2.0$ 到 $10.0$ 步长 $0.25$ = **33 个**
- **目标末速 $t_v$**：$\pm 3 \times 2.0 = \pm 6$ m/s 共 **7 个**

$$9 \times 33 \times 7 = \mathbf{2079} \text{ 条候选路径}$$

每条最多 41 个采样点（$10.0 / 0.25 + 1$）。

> **这是暴力枚举，不是优化器。** 每拍生成 2079 条五次+四次多项式曲线，逐条采样、逐条检查可行性、逐条算代价。20 Hz 下就是每秒 4 万条曲线的构造与求值。
>
> 之所以可行，是因为状态空间被离散得很粗（横向 1.5 m、时域 0.25 s、末速 2 m/s），而乘积恰好落在可算的范围内。

### 2.6 代价函数

`FrenetOptimalTrajectory.cpp:131-158`：

```cpp
135: tfp->c_lateral = fot_hp->kd * tfp->c_lateral_deviation +
136:                  fot_hp->kv * tfp->c_lateral_velocity +
137:                  fot_hp->ka * tfp->c_lateral_acceleration +
138:                  fot_hp->kj * tfp->c_lateral_jerk;
...
143: tfp->c_end_speed_deviation = abs(fot_ic->target_speed - tfp->s_d.back());
144: tfp->c_time_taken = ti;
146: tfp->c_longitudinal = fot_hp->ka * tfp->c_longitudinal_acceleration +
147:                       fot_hp->kj * tfp->c_longitudinal_jerk +
148:                       fot_hp->kt * tfp->c_time_taken +
149:                       fot_hp->kd * tfp->c_end_speed_deviation;
...
152: tfp->c_inv_dist_to_obstacles = tfp->inverse_distance_to_obstacles(obstacles);
...
156: tfp->cf = fot_hp->klat * tfp->c_lateral +
157:           fot_hp->klon * tfp->c_longitudinal +
158:           fot_hp->ko * tfp->c_inv_dist_to_obstacles;
```

$$J = k_{lat} J_{lat} + k_{lon} J_{lon} + k_o J_{obs}$$

| 权重 | 值 | 行 |
|---|---|---|
| `kd`（偏差） | **1.0** | `:52` |
| `kv`（横向速度） | **2.0** | `:53` |
| `ka`（加速度） | **0.3** | `:54` |
| `kj`（jerk） | **0.1** | `:55` |
| `kt`（耗时） | **0.3** | `:56` |
| `ko`（障碍距离） | **1.5** | `:57` |
| `klat` | **0.8** | `:58` |
| `klon` | **0.5** | `:59` |

> **注意 `c_longitudinal` 复用了 `kd`、`ka`、`kj`**——这是 zhmzhu 上游代码的原样，`J_{lon}$ 里用 `kd` 罚末速偏差、`ka` 罚纵向加速度、`kj` 罚纵向 jerk，**不是**专门的纵向权重。这是上游的设计，不是笔误。
>
> 另外 `c_end_speed_deviation` 是**线性** $\lvert\Delta\rvert$，不是平方。

`inverse_distance_to_obstacles`（`FrenetPath.cpp:162-184`）对**每个（障碍物，采样点）对**求 $1/\text{closest}$ 求和，`closest` 取到 AABB 四角的最小距离。**离障碍物越近，代价越大**，但注意是**倒数**——所以它对「几乎撞上」和「离得很近」的区分度不高。

### 2.7 可行性过滤

`FrenetPath.cpp:82-104` 逐采样点检查四类约束：

| 约束 | 判据 |
|---|---|
| 速度 | $\lvert\dot s\rvert > \text{max\_speed}$ 拒绝 |
| 加速度 | $\lvert\ddot s\rvert > \text{max\_accel}$ 拒绝 |
| 曲率 | $\lvert c\rvert > \text{max\_curvature}$（**0.3**）拒绝 |
| 碰撞 | `is_collision(obstacles)` |

**碰撞检查用车身轮廓 + 线段对 AABB 边的求交**（`Obstacle.cpp:64-87`），但**只在采样点上**——这正是第 6 节要量化的那个「点测」局限。

### 2.8 Eigen 是硬依赖

`CMakeLists.txt:621` 用 `if(FLOWENGINE_HAVE_EIGEN)` 包住整块；`:646` 和 `:659` 分别 `target_link_libraries(frenet_planner PUBLIC Eigen3::Eigen m)` 和 `frenet_bridge`。

**没有 Eigen 时**（`:662-668`）只建 `add_custom_target(frenet_planner)` / `frenet_bridge` 两个**空目标**——构建「成功」但不产出任何库。`modules/adas_nodes/CMakeLists.txt:491` 只在三个条件全满足时才定义 `HAVE_FRENET`，否则 `planning_node` 走降级路径（`:2228-2239`），退化成恒速车道保持，并每 2.5 秒打一次警告（`:2878-2883`）。

> **顺带一提**：`third_party/frenet-optimal-trajectory-planner/` 是同一套算法的 **Python 参考副本**（自带 `.git/`），**不在任何 CMakeLists.txt 里**，是死重量。

---

## 3. 规划节点的 37 步主循环

`planning_node.cpp:1564-2891`。20 Hz，**事件驱动**（`BusQueueBridge` 挂 4 个话题，50 ms 超时）+ 50 ms 最小间隔门限：

```cpp
1577: (void)co_await plan_bridge.recv_any_for(50000);
1582: if (_plan_now - g.last_plan_us < 50000) continue;
```

> **注意 `discovery_advertise` 报的是 10 Hz（`:3049-3050`），实际跑的是 20 Hz。**

抽掉主干，剩下的关键步骤：

| # | 步骤 | 行 |
|---|---|---|
| 8 | **目标速度解析**：`route_target_speed` > `behavior.target_speed` > `g.target_speed` | `:1657-1664` |
| 9 | 安全钳位：`cfg_max_speed` / `road_speed_limit` / `environment_max_speed` | `:1682-1699` |
| 12 | **TTC 跟车兜底** $v \le (\text{gap}-5)/4$，两轮「先提示再执行」 | `:1751-1859` |
| 13 | 对向让行（×0.4）+ 窄路减速 | `:1861-1921` |
| 14 | **障碍物注入 Frenet** + **红绿灯虚拟墙**（8.0 × 0.5 m） | `:1923-2013` |
| 15 | 消费 `planning/behavior` → `target_lane_offset` | `:2015-2061` |
| 17 | **掉头分支**（`overtake_state == 3`）：缓存命中直接发布，否则生成 512 点 → 降采样到 64 → κ 钳位 | `:2084-2196` |
| 18 | **`frenet_plan(..., 50)`** | `:2198-2240` |
| 19 | 变道 `d_out` 覆盖（quintic，$L=50.0$） | `:2242-2302` |
| 23 | 构建 64 个 `TrajectoryPoint`，`t_rel_us = i × 100000`（**10 Hz 采样**） | `:2356-2393` |
| 28 | **`st_graph_plan` + 速度回填 + $0.9\sqrt{5/\kappa}$ 钳位** | `:2441-2603` |
| 29 | 失败兜底：ego 位置单点，$v=0$ | `:2604-2615` |
| 31 | **W3 扫掠检查（仅诊断）** | `:2637-2668` |
| 32 | **可行性复核**：$\kappa>0.25$、$a_{lat}>5.0$、$v\in(-0.5, 1.1 v_{max})$ | `:2670-2723` |
| 36 | **参考线发布**（101 点，`pjqp_smooth_2d`，$w_{raw}=10.0$） | `:2800-2866` |

> **输出轨迹是 10 Hz 采样**（`t_rel_us = i × 100000`），不是 20 Hz。而第 18 章的安全层 `TtcObsView` 和第 17 章的控制节点都按 50 ms（20 Hz）假设时间轴。这个不一致在跨越控制/规划边界时会产生半拍延迟——值得在思考题里展开。

### 3.1 轨迹拼接一致性检查

`:2337-2354`：

| 判据 | 阈值 | 行 |
|---|---|---|
| 上一拍到这一拍的时间 | `< 500 ms` | — |
| 位置差 | `< 2.0` m | — |
| 速度差 | `< 3.0` m/s | — |

**旧稿说「往前推 100~150 ms 的轨迹点当下一周期起点」——代码里没有这个机制。** 真实做法是**事后校验**：把新轨迹的起点和上一拍的终点比，超差就记警告。

---

## 4. S-T 图动态规划：真正的纵向优化

**这是整条链路上唯一的优化器**——虽然它是动态规划，不是数值优化。

### 4.1 九个常量

[`modules/adas_nodes/st_graph.h:36-54`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/st_graph.h)：

| 宏 | 值 | 行 | 含义 |
|---|---|---|---|
| `STG_S_HORIZON` | **89.0** | `:36` | 默认视界（m） |
| `STG_S_RES` | **1.0** | `:41` | $s$ 方向分辨率（m） |
| `STG_A_MAX` | **4.0** | `:42` | 最大减速度（m/s²） |
| `STG_A_LAT_MAX` | **5.0** | `:43` | 最大侧向加速度（m/s²） |
| `STG_V_MAX` | **20.0** | `:44` | 速度上限（m/s） |
| `STG_V_CAND_STEP` | **0.2** | `:45` | DP 候选速度步长（m/s） |
| `STG_W1` | **1.0** | `:46` | 速度偏差代价权重 |
| `STG_W2` | **2.0** | `:47` | 加速度代价权重 |
| `STG_CURVE_SAFETY` | **0.85** | `:48` | 曲率安全系数 |

> [!WARNING]
> **头文件注释和代码不符。** `st_graph.h:19` 和 `st_graph.c:113` 的注释都写「视界动态扩展：max(50, 停点+5)」，**但 `STG_S_HORIZON` 实际是 89.0**。`planning_node.cpp:2509` 也硬编码了配套的 `stop_d < 89.0` 门限。
>
> 任何照着注释写「50 m 视界」的文档都是错的。

**还有三个只在 `.c` 里的常量**（`st_graph.c`）：

| 值 | 位置 | 说明 |
|---|---|---|
| `STG_MAX_CAND 101` | `:68` | 候选速度上限 |
| 墙半宽 **0.3** | `:28` | **内联字面量，不是宏** |
| 静止点 dt 下限 **0.05** | `:194` | **内联** |
| 视界扩展 **+5.0** | `:116` | **内联** |

### 4.2 网格

**$s$ 轴**：等距 1.0 m，$n = \lfloor 89/1 \rfloor + 1 = 90$ 格，覆盖 $0 \sim 89$ m。

**$v$ 轴（候选）**：等距 0.2 m/s，**上限随 $v_{lim}(s)$ 收缩**（`:131-139`）：

```cpp
131: for (int i = 0; i < n; i++) {
132:     double lim = v_lim[i];
133:     if (lim < 0.0) lim = 0.0;
134:     if (lim > STG_V_MAX) lim = STG_V_MAX;
135:     int m = (int)(lim / STG_V_CAND_STEP) + 1;
136:     if (m > STG_MAX_CAND) m = STG_MAX_CAND;
137:     s_cand_n[i] = m;
138:     for (int k = 0; k < m; k++) s_cand[i][k] = (double)k * STG_V_CAND_STEP;
139: }
```

**候选集是均匀网格，不是速度比例的。** 所以在低速段（$v_{lim}$ 小）候选数少、高速段多。$v_{lim} = 20$ 时是 101 个，$v_{lim} = 5$ 时只有 26 个。

**$t$ 轴是隐式的**——由 $s$ 和 $v$ 反推（见 4.4）。

### 4.3 静态限速：三个约束取最小

`build_v_lim`（`:43-64`）：

```cpp
48:     double v = STG_V_MAX;
49:     if (in->kappa_fn) {
50:         double vc = stg_curve_v_limit(in->kappa_fn(s, in->kappa_user));
51:         if (vc < v) v = vc;
52:     }
53:     if (in->stop_s >= 0.0) {
54:         double d = in->stop_s - s;
55:         if (d > 0.0) {
56:             double vb = sqrt(2.0 * STG_A_MAX * d);
57:             if (vb < v) v = vb;
58:         } else {
59:             v = 0.0;
60:         }
61:     }
62:     v_lim[i] = v;
```

$$v_{lim}(s) = \min\!\left(20,\ 0.85\sqrt{\frac{5}{\lvert\kappa(s)\rvert}},\ \sqrt{2\cdot 4\cdot(s_{stop}-s)}\right)$$

`stg_curve_v_limit`（`:13-17`）：

```cpp
15:     if (kappa < 1e-9 && kappa > -1e-9) return STG_V_MAX;
16:     return STG_CURVE_SAFETY * sqrt(STG_A_LAT_MAX / fabs(kappa));
```

**三个约束的物理含义**：

| 约束 | 公式 | 含义 |
|---|---|---|
| 直道限速 | $20$ m/s | 72 km/h |
| **曲率限速** | $0.85\sqrt{5/\kappa}$ | 向心力上限，**留 15% 冗余** |
| 制动限速 | $\sqrt{8(s_{stop}-s)}$ | 能在停止点前刹停 |

超过停止点时 $v_{lim}$ 直接为 0（`:59`）——**那之后整列候选速度都是 0**，DP 只能输出静止解。

> **0.85 这个系数的来历**：`STG_A_LAT_MAX = 5.0` m/s²，$0.85 \times 5.0 = 4.25$ m/s² 是实际下发给控制层的预算。而第 17 章的 `steer_limit_for_speed` 巡航权限是 **4.0** m/s²（第 17 章 3.5 节记录了从 1.4 调到 4.0 的那次修复），控制层还有 `0.16` rad 的硬限幅。
>
> **所以链路的权限是递减的：规划 4.25 > 控制 4.0 > 物理 5.0。** 这个递减顺序是必需的——任何一层大于它下游，都会锁死一条合法轨迹。第 19 章那个 `max_steer` 0.35 vs 0.22 的问题就是反例。

### 4.4 障碍物占据：线性外推 + 虚拟墙

`occupied_at`（`:21-39`）：

```cpp
24:     for (int i = 0; i < in->n_walls; i++) {
25:         const StgRedWall* w = &in->walls[i];
26:         if (w->t_red >= 0.0 && t >= w->t_red) continue;   /* 变绿后墙消失 */
27:         double ws = w->stopline_s - w->wall_margin;
28:         if (fabs(s - ws) <= 0.3) return 1;                  /* 半宽 0.3 m */
29:     }
...
33:     for (int i = 0; i < in->n_obstacles; i++) {
34:         const StgObstacle* o = &in->obstacles[i];
35:         double t_rel = t - in->t0;
36:         if (fabs(s - (o->s0 + o->v * t_rel)) <= o->half_len) return 1;
37:     }
```

**红绿灯墙**是 $s$ 轴上的一个固定点（`stopline_s - wall_margin`），半宽 0.3 m。`wall_margin` 默认 **5.0**（`planning_node.cpp:2484`）——**停止线前 5 米就开始禁行**。

`t_red` 在当前实现里恒为 `-1.0`（`planning_node.cpp:2480`），意味着 `if (w->t_red >= 0.0 && ...)` 永不成立——**墙永远存在，不随灯色消失**。这是第一阶段的简化。

**动态障碍物**在 $s$ 轴上以恒定速度线性外推：

$$s_{obs}(t) = s_0 + v\cdot(t - t_0)$$

`half_len` 默认 **2.5**（`planning_node.cpp:2539`，注释写「车长/2+0.25」）。

> **所有障碍物的外推模型都是「沿参考线匀速直线」**——不做任何行为预测。所以第 15 章讲的决策、第 21 章讲的世界模型「动作条件化」，在这个 DP 里完全没有体现。一个会减速的前车和一个会加速的前车，在这里是**完全一样**的。

### 4.5 动态规划：闭式前驱窗口

`st_graph_plan`（`:88-251`）的核心是一个**精确的闭式前驱预筛**（`:97-111`）：

```cpp
 99:     const double accel_delta_v2 = 2.0 * STG_S_RES * STG_A_MAX;   /* = 8.0 */
100:     for (int k = 0; k < STG_MAX_CAND; k++) {
101:         const double vk = (double)k * STG_V_CAND_STEP;
102:         const double lower_v2 = vk * vk > accel_delta_v2 ? vk*vk - accel_delta_v2 : 0.0;
103:         const double upper_v2 = vk * vk + accel_delta_v2;
105:         int first = (int)floor(sqrt(lower_v2) / STG_V_CAND_STEP) - 1;
106:         int last  = (int)ceil (sqrt(upper_v2) / STG_V_CAND_STEP) + 1;
```

推导：1 m 网格上的加速度约束 $\lvert a\rvert \le a_{max}$ 等价于

$$\left\lvert v_k^2 - v_j^2 \right| \le 2 a_{max}\Delta s = 2\times4\times1 = 8$$

$$v_j \in \left[\sqrt{\max(0, v_k^2 - 8)},\ \sqrt{v_k^2 + 8}\right]$$

**这是精确解，不是近似**——它把 101 个候选里的绝大多数一次排除。`±1` 格是给浮点误差留的余量。

### 4.6 递推式

`:190-202`：

```cpp
190:                 double a = accel_between(vj, vk);
191:                 if (fabs(a) > STG_A_MAX + 1e-9) continue;
192:                 double dt;
193:                 if (vj + vk > 1e-6) dt = 2.0 * STG_S_RES / (vj + vk);
194:                 else dt = 0.05;  /* 静止点时间下限 */
195:                 double t = p->t + dt;
198:                 if (occupied_at(in, s_list[i], t)) continue;
199:                 double cost = p->cost
200:                             + STG_W1 * (vk - in->v_target) * (vk - in->v_target)
201:                             + STG_W2 * a * a;
202:                 if (cost < best_cost) { best_cost = cost; best_j = j; best_t = t; }
```

$$C(i,k) = \min_{j}\Big\{C(i{-}1,j) + \underbrace{(v_k - v_{target})^2}_{W_1} + \underbrace{a_{jk}^2}_{W_2}\Big\}$$

$$a_{jk} = \frac{v_k^2 - v_j^2}{2\Delta s}, \qquad \Delta t = \frac{2\Delta s}{v_j + v_k}$$

**$\Delta t = 2\Delta s/(v_j+v_k)$ 来自匀变速运动**：$2\Delta s = \bar{v}\cdot\Delta t$，$\bar v = (v_j+v_k)/2$。

> **没有终端代价、没有时间代价、没有 DP 内部的曲率代价。** 曲率只通过 $v_{lim}(s)$ 这个**预筛**影响 DP，不在代价函数里。

### 4.7 一个真实的不对称

初始列（`:158-167`）：

```cpp
160:             if (occupied_at(in, s_list[0], in->t0) && v > 0.0) s_dp[0][k].cost = STG_INF;
162:             else s_dp[0][k].cost = fabs(v - in->v_target) * STG_W1;
...
167:         s_dp[0][k0].cost = fabs(s_cand[0][k0] - in->v_target) * STG_W1;
```

**第 0 列的代价是 $\lvert v - v_{target}\rvert \cdot W_1$（线性），后续列是 $(v - v_{target})^2 \cdot W_1$（平方）。**

> **这是代码里的实际不对称，不是笔误。** 后果：起点附近的代价曲线是折线，后续是抛物线。影响很小（只有一列），但任何写「DP 代价 = $\omega_1(v-v_t)^2 + \omega_2 a^2$」的描述在 $i=0$ 处不成立。

**死列兜底**（`:218-234`，注释「理论上不会发生」）：整列全是 `STG_INF` 时，填一个状态 `cost = prev_min + W1·v²`。**注意这里用的是 $v^2$ 而不是 $(v-v_{target})^2$**——又一个公式不一致。

### 4.8 终止与回溯

`:237-249`：

```cpp
238:     int last = 0;
239:     for (int k = 1; k < s_cand_n[n - 1]; k++)
240:         if (s_dp[n - 1][k].cost < s_dp[n - 1][last].cost) last = k;
...
244:     for (int i = n - 1; i >= 0; i--) {
245:         if (last < 0 || last >= s_cand_n[i]) break;
246:         out->v_out[i] = s_dp[i][last].v;
247:         out->t_out[i] = s_dp[i][last].t;
248:         last = s_dp[i][last].prev_k;
249:     }
```

**终止状态是最后一列（$s = 89$ m）的最小代价格点**——贪心，无前瞻。并列取**较小索引**（严格小于才更新）。

**回溯遇越界就 `break`**，而循环是**从高 $i$ 向低 $i$**，所以一旦断链，**轨迹的近端（$s$ 接近 0）会留成 0.0 m/s**。实践中 `prev_k \le s_{cand\_n}[i{-}1] - 1$ 恒成立，所以不该触发。

> **如果最后一列全不可行**（全是 `STG_INF`），`last` 停在 0，输出就是垃圾值或全零——**而 `st_graph_plan` 仍然返回 0（成功）**。没有任何错误码。

### 4.9 内存：约 327 KB 的静态表

```cpp
// st_graph.c:71-81
typedef struct {
    double cost;
    int    prev_k;   /* -1 = 起点 */
    double t;
    double v;
} StgDpState;

static StgDpState s_dp[STG_MAX_GRID][STG_MAX_CAND];
static double     s_cand[STG_MAX_GRID][STG_MAX_CAND];
static int        s_cand_n[STG_MAX_GRID];
```

$90 \times 101 \times (8 + 4 + 8 + 8) \approx$ **327 KB**

**文件级 `static`，不可重入、不线程安全。** 注释 `:6` 给的理由是「节点单线程」。

> **头文件注释说 142 KB——过时了**，实际约 327 KB（按 8 字节 double + 4 字节 int 算，结构体会被对齐到 32 字节，是 291 KB；不管怎么算都不是 142 KB）。

### 4.10 一个额外的曲率钳位

`planning_node.cpp:2589-2595` 在 DP 之后又加了一道：

```cpp
v <= 0.9 * sqrt(5.0 / kappa)
```

**`0.9` 和字面量 `5.0` 是重复的魔法数字**，和 `STG_CURVE_SAFETY = 0.85`、`STG_A_LAT_MAX = 5.0` 都不一致。掉头分支里还有第三个数 `0.95`（`:2184`）。

> **同一个物理约束（弯道侧向加速度）在这条链路上有四个不同的安全系数：0.85（`build_v_lim`）、0.9（`:2592`）、0.95（`:2184` 掉头）、以及第 17 章的 1.0（`steer_limit_for_speed` 传入的 `a_max` 是 4.0，物理上限 5.0 即 0.8）。**
>
> 这正是第 3 章讲的那个模式：**同一个知识在多层各存一份，谁也不问谁。**

---

## 5. 关于 QP：澄清调用边界

`include/piecewise_jerk_qp.h` + `src/algorithms/piecewise_jerk_qp.c` 提供了 6 个接口。全仓库的调用情况：

| 符号 | 声明 | 定义 | **调用点** |
|---|---|---|---|
| `pjqp_smooth_2d` | `.h:73` | `.c:161-167` | **`planning_node.cpp:2819`** —— **1 个** |
| `pjqp_smooth_1d` | — | `.c` | 2 个，**都在 `pjqp_smooth_2d` 内部**（`.c:164-165`） |
| `pjqp_path_solve` | `.h:167` | `.c:194-245` | **零** |
| `pjqp_speed_solve` | `.h:199` | `.c:251` | **零** |

> [!IMPORTANT]
> **`pjqp_path_solve` 和 `pjqp_speed_solve` 从来没被调用过——而前者连 QP 都不是。**
>
> `pjqp_path_solve` 的整个实现（`.c:218-234`）是：
> ```c
> double w_total = cfg->w_ref + cfg->w_l;
> double scale = (w_total > 1e-10) ? cfg->w_ref / w_total : 1.0;
> for (int i = 0; i < N; i++) {
>     l_out[i] = scale * l_ref[i];
>     dl_out[i] = 0.0;
>     ddl_out[i] = 0.0;
> }
> ```
> **它不构造任何矩阵，不调用 `pjqp_banded_solve`**，只是把参考值按权重比例缩放再钳位。`:237-242` 那段 `ddl_max` 钳位作用在**刚被清零的** `dl`/`ddl` 上，是彻底的 no-op。
>
> 它的注释（`:215`）自己都承认：「暂简化为：无约束 LQ 解 + 投影到边界」。

### 5.1 真正被调用的那个：`pjqp_smooth_2d`

它的用途只有一个：**参考线几何平滑**。

`planning_node.cpp:2800-2866` 建 101 个点，然后：

```cpp
2818: const double w_raw = 10.0;
2819: pjqp_smooth_2d(sm_x, sm_y, raw_x, raw_y, w_raw, n_ref);
```

`pjqp_smooth_1d`（`.c`）解的是

$$\min \sum_{i=1}^{N-2}(x_{i-1} - 2x_i + x_{i+1})^2 + w_{raw}\sum(x_i - x_{raw,i})^2$$

Hessian 是**五对角带状矩阵**，用 LDLᵀ 分解（`pjqp_banded_solve`）。`pjqp_smooth_2d` 对 x、y 各调一次。

**`w_raw = 10.0` 是平滑强度**：越大越贴合原始折线，越小越平滑。这里 101 个点，权重 10 意味着对原始几何的忠实度优先于平滑。

> **所以「带状稀疏 QP」在这个仓库里唯一的用途，是把地图折线磨圆。** 横向轨迹是网格枚举，纵向速度是动态规划——**两处都不用 QP**。

---

## 6. 扫掠碰撞检测：点测的对照组

### 6.1 为什么需要它

`traj_safety.h:12-16` 的注释说得很清楚：**这是诊断工具，不改变规划行为。**

原因在 `planning_node.cpp:2671`——**扫掠检查从不设 `traj.valid = 0`**：

```cpp
2671: traj.valid = 1;    /* 无条件 */
```

而 Frenet 内部的碰撞检查（`FrenetPath.cpp:107-159`）**只在采样点上做**（第 2.7 节）。

### 6.2 离散点测的漏检

采样点是 `t_rel_us = i × 100000`，即 10 Hz。20 m/s 时相邻点间距 **2 米**。

而红绿灯虚拟墙的半宽是 **0.3 m**（`st_graph.c:28`）——**总厚 0.6 m**。

```
点测方式（现状）:
    轨迹采样点 P1                    轨迹采样点 P2
         ●─────────────────────────────────────●
                    ┌──┐
                    │墙│  (厚 0.6 m)
                    └──┘
         两个采样点都落在墙的两侧 → 全部绿灯放行

扫掠方式（traj_swept_check）:
    轨迹采样点 P1 ═══════════[ 命中求交 ]═══════════ 轨迹采样点 P2
                    ┌──┐
                    │墙│  (厚 0.6 m)
                    └──┘
         线段穿透 → 100% 捕获
```

**一道 0.6 米厚的墙，在 2 米的采样间隙里可以完全「隐身」。**

> 这不是理论风险——FlowSim 里的施工区围栏厚度被特意设成 **4.0 m**（`flowsim_node.cpp:1020`），注释写「覆盖薄墙漏检」。**仿真侧已经承认了这个问题并做了补偿，规划侧只是还没有。**

### 6.3 `seg_aabb_hit`：真的 Slab 法

`traj_safety.c:10-39`：

```cpp
10: static int seg_aabb_hit(double x0, double y0, double x1, double y1,
11:                         double cx, double cy, double hx, double hy) {
12:     const double dx = x1 - x0;
13:     const double dy = y1 - y0;
14:     double tmin = 0.0, tmax = 1.0;
15:
16:     if (fabs(dx) < 1e-12) {
17:         if (x0 < cx - hx || x0 > cx + hx) return 0;   /* 平行且在 slab 外 */
18:     } else {
19:         double t1 = (cx - hx - x0) / dx;
20:         double t2 = (cx + hx - x0) / dx;
21:         if (t1 > t2) { double t = t1; t1 = t2; t2 = t; }
22:         if (t1 > tmin) tmin = t1;
23:         if (t2 < tmax) tmax = t2;
24:         if (tmin > tmax) return 0;
25:     }
26:
27:     if (fabs(dy) < 1e-12) {
28:         if (y0 < cy - hy || y0 > cy + hy) return 0;
29:     } else {
30:         double t1 = (cy - hy - y0) / dy;
31:         double t2 = (cy + hy - y0) / dy;
32:         if (t1 > t2) { double t = t1; t1 = t2; t2 = t; }
33:         if (t1 > tmin) tmin = t1;
34:         if (t2 < tmax) tmax = t2;
35:         if (tmin > tmax) return 0;
36:     }
37:
38:     return 1;
39: }
```

**这是标准的 Liang–Barsky slab clipping，实现正确。** 四个要点：

1. `tmin = 0, tmax = 1` 把参数钳在**线段自身**，不是无限长直线；
2. 平行分支（`|d| < 1e-12`）**直接判包含而不做除法**——这是标准的退化处理，不是偷懒；
3. `t1 > t2` 交换处理负方向；
4. 每一轴后判 `tmin > tmax` 提前退出。

**触摸边界算命中**（`tmin == tmax` 通过）。测试 `test_swept_ignores_side_obstacle`（`tests/test_adas_nodes_logic.c:526-540`）验证了：$y=1.0$ 且 `ow=2.0`（半宽 1，下边缘正好在 $y=0$）**判命中**，注释写「压线应命中」。

### 6.4 `traj_swept_check` 的语义

`:50-68`：

```cpp
53:     TrajSweptResult r = {0, -1, -1};
54:     if (!xs || !ys || n_pts < 2 || !ox || !oy || !ow || !ol || n_obs <= 0) return r;
56:     for (int s = 0; s + 1 < n_pts; s++) {
57:         for (int i = 0; i < n_obs; i++) {
58:             if (!obs_valid(ox, oy, ow, ol, i)) continue;
59:             if (seg_aabb_hit(..., ol[i]*0.5, ow[i]*0.5)) {
60:                 if (r.hits == 0) { r.seg_idx = s; r.obs_idx = i; }
63:                 r.hits++;
```

**计数的是每个（段，障碍物）对**——一段穿一个盒子计 1 次，穿 4 个盒子计 4 次。`seg_idx`/`obs_idx` 只记**首次**命中。

复杂度 $O((n_{pts}-1)\cdot n_{obs})$，**无空间索引、无 broad-phase、无提前退出**（因为它必须数全）。64 点 × 32 障碍物 = 2016 次求交，20 Hz 下每秒 4 万次——对现代 CPU 无关痛痒。

`obs_valid`（`:41-48`）跳过非有限值和**退化盒**（$h_x \le 0$ 或 $h_y \le 0$）——**跳过而非钳位**。

### 6.5 四个真实测试

`tests/test_adas_nodes_logic.c:509-565`，编入生产同一份 `traj_safety.c`（`CMakeLists.txt:1114`）：

| 测试 | 断言 |
|---|---|
| `test_swept_catches_thin_obstacle_between_samples` | 0.5 m 墙在 x=2，采样点 0/4/8 ⇒ **点测 0 命中，扫掠 1 命中** |
| `test_swept_ignores_side_obstacle` | y=3 ⇒ 0；y=1.5 ⇒ 0；y=1.0（压线）⇒ ≥1 |
| `test_swept_degenerate_and_empty` | `n_pts=1` ⇒ 0；零尺寸盒 ⇒ 0；`xs=NULL` ⇒ 0 |
| `test_swept_counts_all_segments` | 2 段 × 4 盒各穿一次 ⇒ `hits == 4`，`seg_idx == 0` |

**第一个测试就是这个模块存在的理由**——它把「点测漏检、扫掠捕获」这个性质固化成了可执行的断言。

---

## 7. 测试现状与死代码

### 7.1 测试覆盖

| 模块 | 覆盖 |
|---|---|
| `planning_coordinates.h` | ✅ 1 个测试（`modules/adas_nodes/test_planning_coordinates.cpp`，36 行，`ctest planning_coordinates`），覆盖全部 5 个内联函数。**但它注册在 `modules/adas_nodes/CMakeLists.txt:483` 的 `if(FRENET_BRIDGE AND FRENET_PLANNER AND _EIGEN_INC)` 条件块内部**——Eigen 缺失时这个测试根本不构建，`ctest -N` 里也看不到 |
| `traj_safety.c` | ✅ 4 个测试（见 6.5） |
| **`st_graph.c`** | ❌ **零测试** |
| **`frenet_bridge` / FOT** | ❌ **零测试** |

`docs/HANDOFF_2026-09-21.md:202` 明确记录「`st_graph.c`（DP 速度规划）仍零单测」。

**唯一的验证手段是树外的 Python 交叉检查**：`tools/speed_planner_sim.py`（`--run-all`，`st_graph.h:5` 声称 11/11 PASS）。**它没有注册进 ctest**，所以 CI 不会因为它失败而红。

### 7.2 死代码

| 符号 | 位置 | 状态 |
|---|---|---|
| `frenet_set_obstacles` | `frenet_bridge.h:43` | **零调用者**（不带速度的静态版本） |
| `pjqp_path_solve` | `piecewise_jerk_qp.h:167` | 零调用者，**且不是 QP** |
| `pjqp_speed_solve` | `piecewise_jerk_qp.h:199` | 零调用者 |
| `third_party/frenet-optimal-trajectory-planner/` | 目录 | Python 参考副本，**不在任何 CMakeLists** |

### 7.3 硬编码的魔法数字

| 值 | 位置 | 说明 |
|---|---|---|
| **32** | `frenet_bridge.cpp:129-130` | 障碍物数组上限，**静默截断规划节点的 128** |
| 50.0 | `planning_node.cpp:2268` | 变道长度 $L$（m） |
| 2.5 | `planning_node.cpp:2539` | ST 障碍物 `half_len` |
| 10.0 | `planning_node.cpp:2818` | QP 平滑权重 $w_{raw}$ |
| 0.9 / 5.0 | `planning_node.cpp:2592` | 曲率二次钳位，**与 `STG_CURVE_SAFETY=0.85` 重复且不一致** |
| 0.95 | `planning_node.cpp:2184` | 掉头分支的第三个曲率系数 |
| 0.3 | `st_graph.c:28` | 墙半宽，**内联不是宏** |
| 0.05 | `st_graph.c:194` | 静止点 dt 下限 |
| +5.0 | `st_graph.c:116` | 视界扩展 |
| 0.25 / 5.0 | `planning_node.cpp:2675, 2681` | κ 上限 / a_lat 上限，可行性复核 |

### 7.4 文档腐化

- `st_graph.h:19`、`st_graph.c:113` 注释写「视界 max(50, stop+5)」，**实际是 89.0**
- `st_graph.c:6` 注释说 DP 表 142 KB，**实际约 327 KB**
- `docs/CODE_WIKI.md:393` 说 `frenet_create` 设 `max_road_width=7.0`、`maxt=6.0`，**实际是 6.0 和 10.0**（`docs/HANDOFF_2026-09-21b.md:693` 记录了改动）

---

## 8. 源码与资源对照

| 模块 | 源码文件 | 核心符号 | 职责与要点 |
|---|---|---|---|
| **参考线投影** | [`modules/adas_nodes/planning_coordinates.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/planning_coordinates.h) | `Projection`（`:10-16`）<br>`lane_center_d`（`:18-20`）<br>`first_legal_lane`（`:22-24`）<br>`nearest_lane`（`:26-37`）<br>`project_to_path`（`:39-72`）<br>`quintic_lane_change`（`:74-83`） | 87 行 header-only。**最近点投影，非真 Frenet 变换**（$t$ 钳位在 $[0,1]$，不外推）。`s` 用弦长累加，`ref_s` 仅作锚点——隐含不变量未文档化 |
| **Frenet 桥接** | [`src/algorithms/frenet_bridge.h`](file:///home/caixuf/code/FlowEngine/src/algorithms/frenet_bridge.h)<br>[`.cpp`](file:///home/caixuf/code/FlowEngine/src/algorithms/frenet_bridge.cpp) | `frenet_create`（`.cpp:34-62`）<br>`frenet_set_obstacles_v`（`.cpp:82-98`）<br>`frenet_plan`（`.cpp:100-174`） | **纯 marshaller，不做优化**。2 秒速度外推。**`static` 数组硬编码 32 障碍物，静默截断** |
| **FOT 算法** | `third_party/frenet_planner/src/FrenetOptimalTrajectory/` | `FrenetOptimalTrajectory.cpp:57-169`<br>`FrenetPath.cpp:82-184`<br>`QuinticPolynomial` / `QuarticPolynomial` | **三级网格枚举 2079 条候选**。横向五次 + 纵向四次，系数用 Eigen LU。依赖 Eigen3（`CMakeLists.txt:621`） |
| **S-T 速度规划** | [`modules/adas_nodes/st_graph.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/st_graph.h)<br>[`st_graph.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/st_graph.c) | `st_graph_plan`（`.c:88-251`）<br>`build_v_lim`（`.c:43-64`）<br>`stg_curve_v_limit`（`.c:13-17`）<br>`occupied_at`（`.c:21-39`） | **唯一的优化器**。90×101 网格，$O(N)$ 闭式前驱预筛。`static` 表 ~327 KB 不可重入。**零单测** |
| **扫掠安全** | [`modules/adas_nodes/traj_safety.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/traj_safety.h)<br>[`traj_safety.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/traj_safety.c) | `seg_aabb_hit`（`.c:10-39`）<br>`traj_swept_check`（`.c:50-68`）<br>`traj_point_test_hits`（`.c:70-88`） | **Liang–Barsky slab clipping，实现正确**。**仅诊断，从不设 `traj.valid = 0`** |
| **带状 QP** | [`include/piecewise_jerk_qp.h`](file:///home/caixuf/code/FlowEngine/include/piecewise_jerk_qp.h)<br>[`src/algorithms/piecewise_jerk_qp.c`](file:///home/caixuf/code/FlowEngine/src/algorithms/piecewise_jerk_qp.c) | `pjqp_smooth_2d`（`.c:161-167`）<br>`pjqp_path_solve`（`.c:194-245`，**死代码且非 QP**）<br>`pjqp_speed_solve`（`.c:251`，**死代码**） | 唯一活的是 `pjqp_smooth_2d`，用途只有参考线几何平滑（`planning_node.cpp:2819`，$w_{raw}=10.0$） |
| **规划节点** | [`modules/adas_nodes/planning_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/planning_node.cpp) | `run()`（`:1564-2891`）<br>`frenet_plan` 调用（`:2206`）<br>`st_graph_plan` 调用（`:2574`）<br>扫掠检查（`:2643-2668`） | 3124+ 行，20 Hz（但 `discovery_advertise` 报 10 Hz）。37 步主循环。**输出轨迹 10 Hz 采样**（`t_rel_us = i×100000`） |
| **测试** | [`modules/adas_nodes/test_planning_coordinates.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/test_planning_coordinates.cpp)<br>[`tests/test_adas_nodes_logic.c`](file:///home/caixuf/code/FlowEngine/tests/test_adas_nodes_logic.c) | ctest `planning_coordinates`（**条件注册**）<br>ctest `adas_nodes_logic_tests`（4 个 traj_safety 用例，`:509-565`，无条件） | **`st_graph.c` 和 `frenet_bridge` 零测试**；只有树外 `tools/speed_planner_sim.py` 交叉检查，未注册进 ctest。`planning_coordinates` 的注册在 `modules/adas_nodes/CMakeLists.txt:483` 的 `if(FRENET_BRIDGE AND FRENET_PLANNER AND _EIGEN_INC)` 块内，Eigen 缺失时不构建 |

---

## 9. 思考题

1. **四个曲率安全系数必须统一吗？**：
   第 4.10 节指出同一个物理约束（弯道侧向加速度）在这条链路上有**四个不同的系数**：`STG_CURVE_SAFETY = 0.85`、`planning_node.cpp:2592` 的 `0.9`、`planning_node.cpp:2184` 掉头分支的 `0.95`、以及第 17 章 `steer_limit_for_speed` 传入的 `4.0`（相对物理上限 5.0 即 0.8）。
   (a) 这四个系数的**方向**说明什么？规划层 0.85 比控制层 0.8 宽松——意味着规划会输出一条控制层**无法完全执行**的轨迹。请论证：这种「上游比下游宽松」的配置，在什么条件下会导致实际轨迹偏离规划轨迹？从第 17 章 3.5 节的 `$1/v^2$` 限幅推导一下。
   (b) 设计一个统一方案：系数应该定义在哪一层？注意 `st_graph.h` 是 `modules/adas_nodes/` 下的头文件，而 `safety_control_node.cpp` 和 `control_node.cpp` 是不同节点、可能不同 `.so`——它们能共享一个头文件吗？（对比第 3 章 10 节讲的 `param_registry` 跨进程可见性问题。）
   (c) 如果统一成单一系数，那些依赖不同系数的历史行为（掉头 0.95、DP 二次钳位 0.9）会发生什么变化？请评估：改完之后，现有的 8 个回归场景（`lane_change_traffic` 等）里，哪几个最可能失败，为什么？

2. **给 `st_graph.c` 写单元测试**：
   第 7.1 节说这个文件零单测，唯一的验证是树外的 Python 脚本且没进 ctest。
   (a) 设计测试方案。注意三个障碍：(i) `s_dp`/`s_cand` 是**文件级 `static`**，不可重入——测试必须串行，且要提供一个 reset；(ii) `kappa_fn` 是函数指针，回避 `tools/speed_planner_sim.py` 的做法；(iii) `st_graph_plan` 的 `StgInput` 里 `walls`/`obstacles` 是**调用方拥有的数组**。
   (b) 至少写 8 个用例。特别地，锁住这几个性质：(i) **单调性**——无障碍时，输出速度剖面在满足 `a ≤ 0` 的段上不递增；(ii) **制动可行性**——`stop_s = 50` 时，`s = 49` 处的速度必须 $\le \sqrt{2\times4\times1} = 2.83$ m/s；(iii) **曲率约束**——`|κ| = 0.1`（$R=10$ m）时任何点的速度 $\le 0.85\sqrt{50} = 6.01$ m/s；(iv) **第 0 列的线性代价**（4.7 节的 $i=0$ 不对称）——设计一个能**区分**线性与平方的用例。
   (c) 更根本的问题：`st_graph.c` 的算法本质是「均匀网格 DP」，可替换性很高（换成连续优化、换成 A*、换成 MPC 都行）。**在不改变行为的前提下，怎么把它改造成可测试的？** 提示：`s_dp`/`s_cand` 改成调用方传入的 workspace，`kappa_fn` 换成显式的曲率数组。然后论证：这样改之后，原来那个 Python 交叉检查脚本还有存在价值吗？

3. **10 Hz 轨迹 vs 20 Hz 控制**：
   第 3 节末尾提到：规划输出 `t_rel_us = i × 100000`（10 Hz），而第 17 章控制节点跑 40 Hz、第 18 章安全层 5 ms 轮询。
   (a) 量化这个不一致的代价。控制节点在两次轨迹更新之间**只能重复用同一条轨迹的最近点**（第 17 章讲过它按 `t_rel_us` 查最近点）。请算出：在 20 m/s 巡航下，10 Hz 的轨迹点间距是多少？控制节点用它做横向误差反馈，等效的反馈采样周期是多少？
   (b) 这个不一致会造成可观测的问题吗？设计一个诊断：控制节点在「本拍的轨迹点索引没有前进」时打一条日志，统计频率。你预期在什么工况下会看到大量这种日志？
   (c) 修法有两种：(i) 规划输出改成 20 Hz 采样（128 点而不是 64），代价是消息翻倍（`Trajectory` 从 ~3 KB 涨到 ~6 KB）；(ii) 控制节点做轨迹插值。请论证 (ii) 的正确性要求——**插值出来的点，横向误差和 heading 怎么算？** 直接对笛卡尔坐标做线性插值再算 heading 会产生什么误差（提示：看第 20 章 4.2 节 `frenet_to_world` 为什么故意用弦差分重算 heading）？

4. **32 这个数字**：
   第 2.3 节发现 `frenet_bridge.cpp:129-130` 的 `static` 数组硬编码 32，而 `planning_node.cpp:1929` 传的是 128——**第 33 个之后的障碍物被静默丢弃**。
   (a) 先设计一个复现：构造一个 40 个障碍物的场景，其中第 35 号正好在自车正前方 10 m。预期现象是什么？（提示：Frenet 会「看不见」它，规划出一条直接撞上去的轨迹；但第 6 节的扫掠检查用的是完整的障碍物列表，所以 `traj_swept_hits` 会非零——这个组合现象本身就是诊断线索。）
   (b) 修法有三种：把数组改成动态分配并传 `no`；加一个 `#define` 常量并在超限时打 WARN；或者在 `frenet_set_obstacles_v` 里按距离排序截断。评估每种：(i) 性能影响（20 Hz 下的分配开销）；(ii) 行为影响（排序截断会改变哪些障碍物被看见）；(iii) 可观测性（哪种能让你在日志里发现这个问题）。
   (c) 追问一个更普适的问题：**这个仓库里还有多少处「固定容量的静态数组 + 静默截断」？** 本书已经记录了 `PARAM_MAX_ENTRIES(128)`、`FLOW_REGISTRY_MAX_IO(8)`、`flow_registry_list_params(64)`、`MAX_TOPICS_PER_NODE(16)`、`SCENARIO_MAX_ACTORS(64)`、`STG_MAX_OBS(8)`……请设计一个 CI 检查：扫描所有 `static` 数组声明和 `calloc`/`new T[n]`，报告「容量 vs 生产调用方传入的上限」不一致的地方。这个检查最难处理的情况是什么？
