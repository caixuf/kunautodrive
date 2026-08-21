# 第 16 章：跟踪控制与特殊机动（Control, MPC & Maneuver）

> **本章导读**：
> 控制模块（Control Module）是自动驾驶系统的“四肢”。无论上层的感知、定位与规划多么完美，如果底层的横向转向角（Steering Angle）与纵向油门/刹车（Throttle/Brake）无法精确、平稳、无超调地跟踪期望轨迹，车辆就会发生画龙振荡（Snaking）甚至冲出车道。
>
> FlowEngine 控制器 `control_node.cpp` 实现了 **Stanley 几何前馈反馈横向控制器**、**线性时变模型预测控制（LTV MPC）** 以及专为狭窄掉头和倒车泊车设计的 **特殊机动跟踪器（ManeuverTracker）**。

---

## 1. 车辆横向动力学与 Stanley 几何控制算法

Stanley 算法是由斯坦福大学无人车队提出的经典横向跟踪算法，它结合了**航向误差（Heading Error $\psi_e$）**与**前轴横向横偏误差（Cross-Track Error $e_y$）**：

```
Stanley 几何误差模型:
                      前轴中心 (Front Axle)
                            o ──► 车辆朝向 (Heading psi)
                           /|
                          / | e_y (横向偏差)
                         /  |
期望轨迹 (Target Path) ───────x────────────────► 切线航向 psi_target
```

### 1.1 Stanley 转向角控制律公式

$$\delta(t) = \psi_e(t) + \arctan\left( \frac{k \cdot e_y(t)}{v(t) + k_{\text{soft}}} \right)$$

其中：
- $\psi_e = \psi_{\text{ego}} - \psi_{\text{target}}$：当前车头朝向与目标轨迹切线朝向的夹角；
- $e_y$：前轴中心到目标轨迹最近点的垂直欧氏距离（偏左为正，偏右为负）；
- $k$：横向增益系数（通常取 $0.8 \sim 1.5$）；
- $k_{\text{soft}}$：低速软化常数（防止车速接近 0 时分母为 0 导致转向角饱和）。

---

## 2. 线性时变模型预测控制（LTV MPC）

在高车速（$> 60\text{ km/h}$）或大侧向加速度工况下，由于轮胎存在侧偏角（Tire Slip Angle），纯几何 Stanley 算法会出现稳态横偏。FlowEngine 在 `src/core/ltv_mpc.c` 中实现了基于自行车动力学模型的 LTV MPC：

### 2.1 状态空间方程
$$X = \begin{bmatrix} e_y \\ \dot{e}_y \\ e_\psi \\ \dot{e}_\psi \end{bmatrix}, \quad \dot{X} = A X + B u + C \rho$$

在预测时域 $N_p = 10 \sim 20$ 步内，构造 QP 二次规划问题：
$$\min_U \sum_{k=0}^{N_p} \left( X_k^T Q X_k + u_k^T R u_k + \Delta u_k^T R_{\Delta} \Delta u_k \right)$$

通过 OSQP 或内点法在 $10\text{ ms}$ 内求解出首步最优前轮转角 $u_0^*$。

---

## 3. 特殊机动跟踪器（ManeuverTracker）

在自动驾驶中，**掉头（U-Turn）、平行泊车（Parking）、倒车（Reverse）** 属于非连续参考线的特殊工况。传统轨迹规划器无法直接在此类工况下生成连续单向多项式。

FlowEngine 采用基于航路点状态机的 `ManeuverTracker`（`include/maneuver_tracker.h`）：

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

---

## 4. 转向角低通滤波与极限环抗振荡

在实车调试中，控制周期微小的调度抖动会导致转向角产生 $\sim 1.6\text{ Hz}$ 的极限环（Limit Cycle）左摇右晃。

FlowEngine 在 `control_node.cpp` 中引入了一阶滞后低通滤波与横摆阻尼：
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

---

## 5. 工业级避坑指南

### 避坑 1：倒车工况下的 Stanley 符号翻转
- **陷阱**：当车辆挂倒挡（Reverse）倒车时，前轮转向产生的横向运动学效果与前进时恰好相反。若直接运行前进时的 Stanley 公式，控制器会迅速正反馈发散导致转向角死锁在极端位置。
- **解决方案**：在倒车时，将参考点切换为后轴中心，并将误差项乘以 $-1.0$。

### 避坑 2：执行器死区（Deadband）与饱和限幅
- 真车转向电机和底盘由于机械间隙存在 $0.5^\circ \sim 1.0^\circ$ 的控制死区。控制输出必须在微小误差区间施加死区非线性补偿，并硬限制最大转角速度（Slew Rate Limit，如 $\le 300^\circ/\text{s}$），防止转向电机过热过流保护跳闸。

---

*下一章预告：第 17 章将讲解安全底线——FlowCoro 协程安全包络与碰撞闸门机制。*
