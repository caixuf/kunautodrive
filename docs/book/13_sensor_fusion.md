# 第 13 章：多传感器融合与状态估计（EKF & SLAM）

> **本章导读**：
> 在真实的物理世界中，没有任何单一传感器是完美无缺的：GPS/GNSS 具备绝对全局定位能力，但采样率低（1~10Hz）且在隧道、高架下易受多路径干扰（Multipath Error）丢失信号；IMU 惯导更新频率极高（100~200Hz）且不受外界遮挡影响，但存在严重的积分零偏漂移（Drift）；轮速计（Odometry）容易发生车轮打滑。
>
> KunAutoDrive 构建了基于 **扩展卡尔曼滤波（Extended Kalman Filter, EKF）** 的多源松耦合融合定位系统 `fusion_node` 与 `ekf_slam` 模块，实现了高频、厘米级精度与具备故障自愈能力的车辆位姿估计。

---

## 1. 多传感器融合拓扑架构

```
  [GPS 模块 (10Hz)] ─────► [经纬度投影 WGS84 → ENU 平面坐标] ──┐
                                                               │ 绝对位置量测 Z_gps
  [IMU 模块 (100Hz)] ────► [重力补偿与角速度积分] ────────────┼──► [EKF 融合状态估计器]
                                                               │      ├── 预测步 (100Hz)
  [轮速里程计 (50Hz)] ──► [阿克曼底盘动力学推算] ─────────────┘      └── 更新步 (10Hz / 异步)
                                                                            │
                                                                            ▼
                                                                [fusion/localization]
                                                                (高频平滑位姿 100Hz)
```

---

## 2. 车辆非线性运动学状态模型

### 2.1 状态向量定义
$$X = \begin{bmatrix} p_x \\ p_y \\ \theta \\ v \\ \omega \end{bmatrix} = \begin{bmatrix} \text{全局东向坐标 (m)} \\ \text{全局北向坐标 (m)} \\ \text{航向角 Heading (rad)} \\ \text{纵向车速 (m/s)} \\ \text{横摆角速度 (rad/s)} \end{bmatrix}$$

### 2.2 离散时间非线性转移方程 $f(X, u)$
在时间间隔 $\Delta t$ 内，若以阿克曼转向或角速度积分推进：

$$\begin{cases}
p_{x, k} = p_{x, k-1} + v_{k-1} \cos(\theta_{k-1}) \Delta t \\
p_{y, k} = p_{y, k-1} + v_{k-1} \sin(\theta_{k-1}) \Delta t \\
\theta_k = \theta_{k-1} + \omega_{k-1} \Delta t \\
v_k = v_{k-1} + a_x \Delta t \\
\omega_k = \omega_z
\end{cases}$$

### 2.3 雅可比矩阵（Jacobian Matrix）线性化
由于状态转移函数 $f(X)$ 含有非线性三角函数 $\sin(\theta)$ 与 $\cos(\theta)$，在 EKF 预测协方差时必须计算一阶偏导数雅可比矩阵 $F_J$：

$$F_J = \frac{\partial f}{\partial X} = \begin{bmatrix}
1 & 0 & -v \sin(\theta) \Delta t & \cos(\theta) \Delta t & 0 \\
0 & 1 & v \cos(\theta) \Delta t & \sin(\theta) \Delta t & 0 \\
0 & 0 & 1 & 0 & \Delta t \\
0 & 0 & 0 & 1 & 0 \\
0 & 0 & 0 & 0 & 0
\end{bmatrix}$$

---

## 3. 异步传感器测量更新（Measurement Update）

KunAutoDrive 采用**异步触发更新架构**：
- IMU 以 100Hz 高频到达，持续执行 EKF **预测步（Predict）**，快速更新位姿；
- 当 GPS 数据到达时，执行 GPS 位置更新（$H_{gps}$ 观测矩阵）；
- 当里程计数据到达时，执行速度更新（$H_{odom}$ 观测矩阵）。

```c
/* modules/adas_nodes/ekf_slam.c 核心更新逻辑 */
void ekf_update_gps(EkfSlam* ekf, float gps_x, float gps_y, float gps_heading) {
    // 1. 计算残差 Innovation
    float y[3];
    y[0] = gps_x - ekf->x.x;
    y[1] = gps_y - ekf->x.y;
    y[2] = normalize_angle(gps_heading - ekf->x.heading);

    // 2. 计算卡尔曼增益 K = P * H^T * (H * P * H^T + R)^-1
    // 3. 状态校正 X = X + K * y
    // 4. 协方差收敛 P = (I - K * H) * P
}
```

---

## 4. 协方差发散抑制与数值稳定性

在长达数小时的连续运行中，由于浮点精度舍入误差，协方差矩阵 $P$ 可能失去**对称正定性（Symmetric Positive-Definite）**，引发卡尔曼增益计算出现 `NaN` 导致系统崩溃。

KunAutoDrive 采取两项关键防护：

### 4.1 强制矩阵对称化（Joseph Form Stabilization）
在每次协方差更新后，强制执行对称投影：
$$P = \frac{1}{2} (P + P^T)$$

### 4.2 协方差下界裁剪（Covariance Clamping）
防止对角线方差因过于自信而过度收缩为 0（导致后续测量被完全无视）：
```c
for (int i = 0; i < STATE_DIM; i++) {
    if (P->data[i * STATE_DIM + i] < 1e-6f) {
        P->data[i * STATE_DIM + i] = 1e-6f;
    }
}
```

---

## 5. 工业级避坑指南

### 避坑 1：角度不连续性（Angle Wrap-around ±π 跃变）
- **现象**：当车辆航向角从 $+179^\circ$ 转动到 $-179^\circ$ 时，直接做差得到的角度差为 $358^\circ$，会导致卡尔曼更新产生剧烈反向抖动！
- **铁律**：所有角度残差计算必须通过 `atan2(sin(dy), cos(dy))` 标准化到 $[-\pi, +\pi]$ 区间。

### 避坑 2：GPS 丢星时的死推（Dead Reckoning）与降级告警
- 当进入隧道等卫星信号丢失场景时，必须停止 GPS 量测更新，纯靠 IMU 与轮速计进行航位推算（Dead Reckoning），并在状态中标记 `localization_status = DEGRADED`，通知规控系统降低巡航车速。

---

*下一章预告：第 14 章将探讨决策大脑——8 状态行为决策状态机与 NOA 导航主动变道。*
