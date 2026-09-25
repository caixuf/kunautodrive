# 第 16 章：让车真的贴着那条线走

规划层把轨迹交出来，只是说清楚了「该走哪儿」；车能不能真贴着这条线走，全看控制层。转向角打多深、油门给多少、什么时候踩刹车，这几笔账算不准，再好的规划也是白搭——车会画龙，严重时直接冲出车道。这一章讲 KunAutoDrive 的控制器 `control_node.cpp`：它用几何的 Stanley 算横向，用 LTV MPC 在大侧向加速度下兜底，还专门为掉头、倒车备了一个特殊机动跟踪器。

## 靠几何就能跟线：Stanley

Stanley 是斯坦福大学无人车队提出的经典横向跟踪算法，它把两样东西合到一起：车头朝向跟目标切线差多少（航向误差 Heading Error $\psi_e$），以及前轴离目标轨迹偏了多远（前轴横向横偏误差 Cross-Track Error $e_y$）：

```
Stanley 几何误差模型:
                      前轴中心 (Front Axle)
                            o ──► 车辆朝向 (Heading psi)
                           /|
                          / | e_y (横向偏差)
                         /  |
期望轨迹 (Target Path) ───────x────────────────► 切线航向 psi_target
```

### 一个公式，两项误差

$$\delta(t) = \psi_e(t) + \arctan\left( \frac{k \cdot e_y(t)}{v(t) + k_{\text{soft}}} \right)$$

其中：$\psi_e = \psi_{\text{ego}} - \psi_{\text{target}}$ 是当前车头朝向与目标轨迹切线朝向的夹角；$e_y$ 是前轴中心到目标轨迹最近点的垂直欧氏距离（偏左为正，偏右为负）；$k$ 是横向增益系数，通常取 $0.8 \sim 1.5$；$k_{\text{soft}}$ 是低速软化常数，防止车速接近 0 时分母为 0 把转向角顶到饱和。

## 车速一高，几何法就不够了

车速上去（$> 60\text{ km/h}$）或侧向加速度一大，轮胎的侧偏角（Tire Slip Angle）就冒出来了，纯几何的 Stanley 会留下一份稳态横偏。KunAutoDrive 于是用自行车动力学模型补上，在 `src/core/ltv_mpc.c` 里实现了 LTV MPC：

### 把它写成一个 QP 问题
$$X = \begin{bmatrix} e_y \\ \dot{e}_y \\ e_\psi \\ \dot{e}_\psi \end{bmatrix}, \quad \dot{X} = A X + B u + C \rho$$

在预测时域 $N_p = 10 \sim 20$ 步内，构造 QP 二次规划问题：
$$\min_U \sum_{k=0}^{N_p} \left( X_k^T Q X_k + u_k^T R u_k + \Delta u_k^T R_{\Delta} \Delta u_k \right)$$

通过 OSQP 或内点法在 $10\text{ ms}$ 内求解出首步最优前轮转角 $u_0^*$。

## 掉头和倒车不属于「连续轨迹」

掉头（U-Turn）、平行泊车（Parking）、倒车（Reverse）这几件事有个共同点：参考线是断的，甚至得掉头往回走，普通轨迹规划器没法在这种工况下生成一条连续单向的多项式。KunAutoDrive 的办法是用基于航路点的状态机 `ManeuverTracker`（`include/maneuver_tracker.h`）来管：

```c
typedef enum {
    MANEUVER_TYPE_NONE = 0,
    MANEUVER_TYPE_UTURN,       // 掉头 (左打满 -> 直行 -> 回正)
    MANEUVER_TYPE_PARKING,     // 泊车 (倒车入库)
    MANEUVER_TYPE_3_POINT_TURN // 三点掉头
} ManeuverType;

typedef struct {
    ManeuverType type;
    uint32_t     stage;        // 当前机动阶段 (0=减速, 1=打满转向, 2=对齐车道)
    uint64_t     stage_start_us;
    float        target_heading;
} ManeuverTracker;
```

## 方向盘为什么一直在抖

实车调试时碰过一个现象：控制周期里一点点调度抖动，就能让转向角生出 $\sim 1.6\text{ Hz}$ 的极限环（Limit Cycle），方向盘一直在小幅左摇右晃。KunAutoDrive 在 `control_node.cpp` 里加了一阶滞后低通滤波和横摆阻尼来压住它：
```c
#define STEER_FILTER_NEW   0.5f  /* 新值权重 50% (-3dB @ 1.2Hz) */
#define STEER_FILTER_PREV  0.5f  /* 历史值权重 50% */

// 一阶低通平滑
float raw_steer = stanley_compute(&ego_pose, &target_point);
g_filtered_steer = STEER_FILTER_NEW * raw_steer + STEER_FILTER_PREV * g_filtered_steer;

// 横摆角速度阻尼抑制高频晃动
float yaw_damping = -0.05f * ego_pose.omega_z;
g_final_steer = clamp(g_filtered_steer + yaw_damping, -MAX_STEER, MAX_STEER);
```

## 控制层最难缠的两件事

第一件是倒车。挂上倒挡之后，前轮转向带来的横向运动学效果和前进时正好相反；要是还照搬前进时的 Stanley 公式，正反馈会让转向角迅速发散，最后死死卡在极限位置。改法很直接：倒车时把参考点换成后轴中心，再把误差项乘上 $-1.0$。

第二件藏在执行器里。真车的转向电机和底盘有机械间隙，$0.5^\circ \sim 1.0^\circ$ 的控制死区就出在这里。控制输出得在这个微小误差区间里做死区非线性补偿，还要把最大转角速度硬限住（Slew Rate Limit，比如 $\le 300^\circ/\text{s}$），不然转向电机会因为过热过流保护跳闸。
