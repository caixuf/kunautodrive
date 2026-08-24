# 第 12 章：点云聚类与卡尔曼目标追踪（LiDAR Tracking）

> **本章导读**：
> 激光雷达（LiDAR）每秒输出成千上万个离散的三维空间点云。如果直接将这些稀疏点输入给上层规划，规划器将因算力崩溃而无法处理。感知前端必须完成两大核心任务：**空间聚类（Clustering）**将离散点聚合成独立的目标实体（Bounding Box），以及**时间序列目标追踪（Tracking）**为不同帧之间的同一个障碍物赋予持久的全局 ID 与精确的速度矢量估计。
>
> KunAutoDrive 感知算法栈采用 **DBSCAN 密度聚类** 与 **基于匈牙利算法匹配的扩展卡尔曼滤波追踪器（Kalman Tracker）**，实现了低延迟、抗遮挡、动静态自识别的多目标追踪引擎。

---

## 1. 感知与追踪数据流架构

```
                     ┌────────────────────────────────────────────────────────┐
                     │              sensor/lidar (原始三维点云帧)             │
                     └──────────────────────────┬─────────────────────────────┘
                                                │
                                                ▼
                     ┌────────────────────────────────────────────────────────┐
                     │      1. 地面点滤除与 ROI 空间截取 (PassThrough Filter)  │
                     └──────────────────────────┬─────────────────────────────┘
                                                │
                                                ▼
                     ┌────────────────────────────────────────────────────────┐
                     │      2. DBSCAN 空间密度聚类与 3D Bounding Box 拟合     │
                     └──────────────────────────┬─────────────────────────────┘
                                                │ 得到当前帧检测列表 (Detections)
                                                ▼
                     ┌────────────────────────────────────────────────────────┐
                     │      3. 目标追踪器 (KalmanTracker & Hungarian Matcher) │
                     │         ├── 状态传播: X(k|k-1) = F * X(k-1)            │
                     │         ├── 距离代价矩阵构建 (Mahalanobis / Euclidean) │
                     │         ├── 匈牙利算法最优二分图匹配 (KM Matcher)      │
                     │         └── 状态更新与协方差收敛                       │
                     └──────────────────────────┬─────────────────────────────┘
                                                │
                                                ▼
                     ┌────────────────────────────────────────────────────────┐
                     │ perception/tracked_objects (全局 TrackID / 动态/静态)  │
                     └────────────────────────────────────────────────────────┘
```

---

## 2. DBSCAN 空间点云密度聚类算法

DBSCAN (Density-Based Spatial Clustering of Applications with Noise) 无需预先指定簇数量 $K$，能有效剔除稀疏噪点并拟合任意几何形状的障碍物。

### 2.1 核心数学参数
- **邻域半径 $\epsilon$ (Epsilon)**：两点之间可视为同一簇的最大欧氏距离（通常取 $0.5 \sim 0.8\text{ m}$）；
- **核心点阈值 $\text{MinPts}$**：半径 $\epsilon$ 范围内至少包含的点数（通常取 $3 \sim 5$）。

### 2.2 伪代码与边界框（Bounding Box）拟合
```c
// 聚类完成后拟合 3D AABB / OBB 边界框
typedef struct {
    double center_x, center_y, center_z;
    double length, width, height;
    double yaw;
    uint32_t point_count;
} BoundingBox;
```

---

## 3. 多目标卡尔曼追踪器（Multi-Object Kalman Tracker）

追踪器的核心任务是在时间序列上维护一组航迹（Tracks），估计目标的绝对位置 $(x, y)$ 与绝对速度 $(v_x, v_y)$。

### 3.1 状态向量与连续运动学模型
采用恒定速度（CV, Constant Velocity）运动学模型：

$$X = \begin{bmatrix} x \\ y \\ v_x \\ v_y \end{bmatrix}, \quad F = \begin{bmatrix} 1 & 0 & \Delta t & 0 \\ 0 & 1 & 0 & \Delta t \\ 0 & 0 & 1 & 0 \\ 0 & 0 & 0 & 1 \end{bmatrix}, \quad H = \begin{bmatrix} 1 & 0 & 0 & 0 \\ 0 & 1 & 0 & 0 \end{bmatrix}$$

### 3.2 预测与更新方程
1. **时间预测（Time Update）**：
   $$\hat{X}_{k|k-1} = F \hat{X}_{k-1|k-1}$$
   $$P_{k|k-1} = F P_{k-1|k-1} F^T + Q$$
2. **测量更新（Measurement Update）**：
   $$K_k = P_{k|k-1} H^T (H P_{k|k-1} H^T + R)^{-1}$$
   $$\hat{X}_{k|k} = \hat{X}_{k|k-1} + K_k (Z_k - H \hat{X}_{k|k-1})$$
   $$P_{k|k} = (I - K_k H) P_{k|k-1}$$

---

## 4. 数据关联：匈牙利算法（Hungarian Association）

在测量更新前，必须将当前帧的 $M$ 个检测目标与内存中的 $N$ 条存量航迹进行一一匹配。

```mermaid
flowchart LR
    A[历史航迹预测位置 Tracks N] --> C[代价矩阵 Cost Matrix N x M]
    B[当前帧检测点 Detections M] --> C
    C --> D[匈牙利算法 Hungarian Algorithm]
    D --> E[匹配成功: 执行 Kalman Update]
    D --> F[未匹配检测: 初始化新 Track (Candidate)]
    D --> G[未匹配航迹: 连续丢帧计数 age++, 达到阈值剔除]
```

---

## 5. 动静态目标分类机制（Static/Dynamic Classification）

上层规划器对静态障碍物（如路桩、路沿、违停车辆）与动态障碍物（如对向来车、变道行人）的避让策略截然不同。

KunAutoDrive 在 `modules/adas_nodes/object_tracker_node.c` 中设计了基于速度积分的时序分类器：
```c
#define STATIC_SPEED_THRESHOLD 0.5f   /* m/s 以下视为低速候选 */
#define STATIC_FRAMES_MIN      20u    /* 连续 20 帧 (1.0s) 低速才判定为真静态 */

if (hypot(track->vx, track->vy) < STATIC_SPEED_THRESHOLD) {
    static_counter[track_id]++;
    if (static_counter[track_id] >= STATIC_FRAMES_MIN) {
        track->is_static = true;
    }
} else {
    static_counter[track_id] = 0;
    track->is_static = false;
}
```

---

## 6. 工业级避坑指南

### 避坑 1：航向角速度补偿（Ego-Motion Compensation）
- **隐患**：本车在高速转弯时，由于本车坐标系自身的旋转，原本绝对静止的障碍物在传感器雷达坐标系中会表现出虚假的横向移动速度。
- **解决方案**：在卡尔曼预测步之前，读取定位模块 `vehicle/state` 的角速度 $\omega_z$，对测量点坐标进行刚体坐标变换与动系速度逆补偿。

### 避坑 2：ID 频繁跳变（Track ID Flipping）
- 当两辆车在十字路口并排行驶发生短暂遮挡时，传统仅基于欧氏距离的匹配容易发生 ID 互换。
- **最佳实践**：代价矩阵采用**马氏距离（Mahalanobis Distance）**结合几何尺寸长宽比（Aspect Ratio）进行多特征融合加权。

---

*下一章预告：第 13 章将深入探讨多传感器融合与 EKF-SLAM 定位框架。*
