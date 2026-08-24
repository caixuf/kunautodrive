# 第 15 章：轨迹与速度规划（Frenet Frame & ST-Graph）

> **本章导读**：
> 自动驾驶车辆在弯曲道路中行驶时，在笛卡尔全局直角坐标系 $(X, Y)$ 下描述车辆运动极为复杂——因为车道本身的几何曲率导致路径约束高度非线性。为了解耦横向控制与纵向控速，工业界普遍采用 **Frenet 曲线坐标系**。
>
> KunAutoDrive 规划算法栈实现了标准的 **笛卡尔-Frenet 相互投影变换**、**五次多项式横向轨迹采样优化** 以及 **基于 ST 图（S-T Graph）的动态规划与凸优化速度剖面生成**。

---

## 1. 笛卡尔坐标系到 Frenet 坐标系的投影映射

Frenet 坐标系以道路中心参考线（Reference Line）为基准：
- **$s$ (Longitudinal)**：沿道路参考线的纵向弧长距离；
- **$d$ (Lateral)**：垂直于参考线切线方向的横向偏移距离（左正右负）。

```
Frenet 坐标变换图解:
                        d (横向偏移)
                        ▲     Ego (s_ego, d_ego)
                        │    /
                        │   /
道路参考线 (Reference Line) ───────┼──/──────────────► s (纵向弧长)
                                (s_proj, 0)
```

### 1.1 投影点搜索（Projection Finding）
给定本车全局坐标 $(x, y)$，通过遍历离散参考线找到距离最近的线段，求垂足 $(x_r, y_r)$ 及对应的弧长 $s$ 与切线航向角 $\theta_r$：

$$d = (x - x_r) \cdot (-\sin\theta_r) + (y - y_r) \cdot \cos\theta_r$$

---

## 2. 横向多项式轨迹采样（Quintic Polynomial Generation）

为了保证横向变道与避障动作平滑，横向轨迹 $d(s)$ 通常采用 **五次多项式（Quintic Polynomial）**，以确保加速度与加加速度（Jerk）连续可导：

$$d(s) = a_0 + a_1 s + a_2 s^2 + a_3 s^3 + a_4 s^4 + a_5 s^5$$

### 2.1 边界条件约束求解
- **起点状态**：$s_0 = 0, \quad d(0) = d_0, \quad d'(0) = d'_0, \quad d''(0) = d''_0$
- **终点目标状态**：$s_1 = S_{\text{end}}, \quad d(S_{\text{end}}) = d_1, \quad d'(S_{\text{end}}) = 0, \quad d''(S_{\text{end}}) = 0$

通过求解 $6 \times 6$ 线性方程组求出系数向量 $[a_0, a_1, a_2, a_3, a_4, a_5]^T$。

### 2.2 轨迹代价函数评估（Cost Evaluation）
采样生成的一组候选轨迹中，通过加权代价函数选取最优曲线：

$$J(d) = w_j \int (d''')^2 dt + w_t T + w_d (d_1 - d_{\text{target}})^2 + w_{\text{coll}} \cdot \text{CollisionCost}$$

---

## 3. 纵向 ST 图与动态规划控速（ST-Graph & DP）

在时间-弧长空间（S-T 空间）中，动态与静态障碍物被投影为多边形禁行区域（Obstacle Polygons）：

```
ST 图速度规划与决策剖面:
  s (距离/m)
  ▲
  │              / [超车轨迹 Over-take: 从上方穿过]
  │             /
  │      ┌────────────┐ (障碍物占据区间: 4s~7s, 50m~60m)
  │      │  Obstacle  │
  │      └────────────┘
  │         /
  │        / [跟车/让行轨迹 Yield: 从下方穿过]
  │       /
  └──────┼────────────────────────► t (时间/s)
         0      2      4      6      8
```

### 3.1 动态规划（Dynamic Programming）离散搜索
将 $S \times T$ 空间划分为网格节点 $(s_i, t_j)$：
1. 状态转移代价：$Cost = C_{\text{speed}} + C_{\text{accel}} + C_{\text{jerk}} + C_{\text{obstacle}}$；
2. 求解贝尔曼方程（Bellman Equation）找到全局无碰撞代价最低的粗解路径。

### 3.2 连续凸优化（Quadratic Programming）平滑
以 DP 粗解为热启动初值，利用二次规划（QP）施加加速度 $|a| \le 3.0\text{ m/s}^2$ 与舒适度 Jerk 上限，输出一条连续平滑的 $s(t)$ 速度剖面。

---

## 4. 工业级避坑指南

### 避坑 1：大曲率道路下的 Frenet 奇异点（Curvature Singularity）
- **现象**：当道路曲率半径 $R < |d|$ 时（例如车辆处于急弯内侧），投影点法线会发生交叉重叠，导致同一个笛卡尔点对应多个不同的 $(s, d)$ 坐标。
- **解决方案**：在参考线生成阶段进行曲率平滑（Curvature Smoothing），并对横向偏移 $|d| > R_{\text{min}}$ 的极端工况回退到笛卡尔直接规划。

### 避坑 2：规划轨迹在时间边界处的端点抖动
- 若每周期重新规划从 $t=0$ 从头计算，由于环境感知噪点，相邻两帧生成的轨迹会发生剧烈跳变。
- **最佳实践**：采用**轨迹拼接（Trajectory Stitching）**机制，以当前时刻向前推 $100\sim 150\text{ ms}$ 的规划轨迹点作为下一周期规划的起点。

---

*下一章预告：第 16 章将进入执行控制层——Stanley 几何横向算法、LTV MPC 与特殊机动跟踪器。*
