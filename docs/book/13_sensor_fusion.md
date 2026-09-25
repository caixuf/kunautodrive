# 第 13 章：当 GPS、IMU 和轮速计各说各话

物理世界里没有哪一路传感器是能信到底的。GPS/GNSS 给的是绝对全局定位，可它采样率只有 1~10Hz，进隧道、上高架还会因为多路径干扰（Multipath Error）直接丢信号；IMU 反过来，100~200Hz 高频更新，不受遮挡影响，代价是积分零偏带来的漂移；轮速计看着便宜好用，车轮一打滑就不准。

所以定位这件事不是挑一个最准的传感器，而是让它们互相补。KunAutoDrive 用扩展卡尔曼滤波（Extended Kalman Filter, EKF）搭了一套多源松耦合的融合定位系统，落在 `fusion_node` 与 `ekf_slam` 两个模块里，输出高频平滑、厘米级的车辆位姿估计，并且在某一路传感器失效时还能自己兜住。

## 三路数据汇到一个滤波器

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

## 状态里放了五个量

$$X = \begin{bmatrix} p_x \\ p_y \\ \theta \\ v \\ \omega \end{bmatrix} = \begin{bmatrix} \text{全局东向坐标 (m)} \\ \text{全局北向坐标 (m)} \\ \text{航向角 Heading (rad)} \\ \text{纵向车速 (m/s)} \\ \text{横摆角速度 (rad/s)} \end{bmatrix}$$

## 一步时间推进怎么写

在时间间隔 $\Delta t$ 内，以阿克曼转向或角速度积分推进，离散时间的非线性转移方程 $f(X, u)$ 是：

$$\begin{cases}
p_{x, k} = p_{x, k-1} + v_{k-1} \cos(\theta_{k-1}) \Delta t \\
p_{y, k} = p_{y, k-1} + v_{k-1} \sin(\theta_{k-1}) \Delta t \\
\theta_k = \theta_{k-1} + \omega_{k-1} \Delta t \\
v_k = v_{k-1} + a_x \Delta t \\
\omega_k = \omega_z
\end{cases}$$

### 非线性要用雅可比矩阵线性化

因为状态转移函数 $f(X)$ 里含有 $\sin(\theta)$ 与 $\cos(\theta)$，EKF 在推进协方差时必须先算一阶偏导数，也就是雅可比矩阵 $F_J$：

$$F_J = \frac{\partial f}{\partial X} = \begin{bmatrix}
1 & 0 & -v \sin(\theta) \Delta t & \cos(\theta) \Delta t & 0 \\
0 & 1 & v \cos(\theta) \Delta t & \sin(\theta) \Delta t & 0 \\
0 & 0 & 1 & 0 & \Delta t \\
0 & 0 & 0 & 1 & 0 \\
0 & 0 & 0 & 0 & 0
\end{bmatrix}$$

## 谁来了就更新谁

这套系统是异步触发更新的：IMU 以 100Hz 高频到达，所以它每次到达都持续跑 EKF 的预测步，位姿一直在往前推；GPS 数据到达时，做一次 GPS 位置更新，用 $H_{gps}$ 观测矩阵；里程计数据到达时，做一次速度更新，用 $H_{odom}$ 观测矩阵。

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

GPS 长时间不来的时候，逻辑要反过来：停止 GPS 量测更新，纯靠 IMU 与轮速计做航位推算（Dead Reckoning），同时把状态里的 `localization_status` 标成 `DEGRADED`，通知规控系统把巡航车速降下来。

## 连续跑几个小时之后，协方差会绷不住

跑得越久，浮点舍入误差积累得越多，协方差矩阵 $P$ 可能失去对称正定性，卡尔曼增益算着算着就冒出 `NaN`，系统跟着崩掉。项目里有两道保险。

### 每次协方差更新后强制对称化

$$P = \frac{1}{2} (P + P^T)$$

### 给对角线方差设个下界

防止对角线方差因为过度自信收缩到 0——真到 0 之后，后面的测量就完全不被采信了：

```c
for (int i = 0; i < STATE_DIM; i++) {
    if (P->data[i * STATE_DIM + i] < 1e-6f) {
        P->data[i * STATE_DIM + i] = 1e-6f;
    }
}
```

## 一个真实故事：航向角从 +179° 跳到 -179°

车在环岛里绕了一圈掉头，航向角从 $+179^\circ$ 转到了 $-179^\circ$。这两个数在物理上只差 $2^\circ$，可要是直接相减，得到的是 $358^\circ$：卡尔曼更新的残差一下子被喂进一个巨大的错误值，输出位姿开始反向剧烈抖动，规划器跟着画出一条先往左猛打、再往右猛打回来的轨迹。

原因很朴素——角度是个圆周量，做差之后必须做标准化。凡是 $\pm\pi$ 附近的计算，残差也好、观测也好、航向差也好，都得先过一遍 `atan2(sin(dy), cos(dy))`，把结果折回 $[-\pi, +\pi]$ 区间，再送进卡尔曼更新。
