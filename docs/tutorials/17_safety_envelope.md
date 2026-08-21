# 第 17 章：安全包络与急停闸门（Safety Envelope & Guardian）

> **本章导读**：
> 在自动驾驶系统中，算法模型（感知神经网络、规划路径多项式）无论经过多么详尽的测试，都存在偶发性“模型退化（Model Failure）”或“输出异常点”的统计概率。如果将规控算法的原始输出直接透传给底层刹车与转向执行器，一旦规划器抛出 NaN 或误判前向无障碍物，将导致灾难性交通事故。
>
> FlowEngine 构建了独立的 **安全包络防护体系（Safety Envelope / Guardian Node）**：它以最高实时优先级运行在最底层，作为车辆物理安全的最后一道硬闸门，实现了 **碰撞时间（TTC）监控、横向干涉豁免、异常指令钳位（Clamp）与硬件直连急停**。

---

## 1. 独立安全底座架构（Guardian Architecture）

```
  [Planning Node 规划输出] ──► [Control Node 控制解算] 
                                      │
                                      ▼
                        [raw_cmd (待审核的控制指令)]
                                      │
                                      ▼
        ┌─────────────────────────────────────────────────────────────┐
        │      Safety Control / Guardian 安全防护包络 (最高优先级)      │
        │                                                             │
        │  ├── 1. 指令有效性检查 (NaN/Inf 过滤、最大转角/加速度限制)    │
        │  ├── 2. TTC 碰撞时间物理闸门 (Time-To-Collision 评估)       │
        │  ├── 3. 行人/易脆弱群体穿越强行制动 (VRU Protection)         │
        │  └── 4. 心跳看门狗超时 (上位机进程死锁则自动触发 AEB)       │
        └──────────────────────────────┬──────────────────────────────┘
                                       │ 仲裁后的安全指令
                                       ▼
                         [actuator/cmd (真车执行器)]
```

---

## 2. 核心数学模型：碰撞时间（TTC）与 AEB 触发曲线

碰撞时间（Time-To-Collision, TTC）表示在假定当前相对速度不变的前提下，两车发生物理接触所剩余的时间：

$$\text{TTC} = \frac{d_{\text{rel}} - d_{\text{safe}}}{v_{\text{ego}} - v_{\text{target}}} \quad (\text{当 } v_{\text{ego}} > v_{\text{target}})$$

```
TTC 分级响应阶梯:
  TTC > 3.0s:      [绿灯] 正常行驶，安全包络不做干预
  2.0s < TTC <= 3.0s: [黄灯 Warning] 仪表盘声光预警，预充液压制动器 (Pre-fill)
  1.0s < TTC <= 2.0s: [橙灯 Partial Braking] 减速 0.3g，协助驾驶员/规划减速
  TTC <= 1.0s:     [红灯 Hard AEB] 立即覆盖并剥夺规划控制权，最大全力制动 (-1.0g)
```

---

## 3. 横向干涉豁免机制（Lateral Interference Exemption）

在复杂的城市路口转弯或绕行静止违停车辆时，前向雷达的视场角（FOV）可能会误扫到路边护栏或相邻车道车辆。如果简单地采用全向 TTC 触发急停，会导致严重的**误刹车（Phantom Braking）**。

FlowEngine 实现了基于车道走廊的**横向横偏投影剔除算法**：

```c
/* modules/adas_nodes/safety_control_node.cpp */
bool is_obstacle_in_collision_corridor(double obs_x, double obs_y, double ego_v, double steer) {
    // 1. 根据当前前轮转角计算车辆预测圆弧轨迹半径 R
    double R = (fabs(steer) > 1e-3) ? (WHEELBASE / tan(steer)) : 1e6;
    
    // 2. 计算障碍物到该圆弧的径向距离
    double dist_to_path = compute_radial_distance_to_arc(obs_x, obs_y, R);
    
    // 3. 动态扩展包络宽度: 基础车宽 1.8m + 随速度增加的动态余量
    double corridor_width = 1.8 + 0.1 * ego_v;
    
    // 若障碍物在走廊外侧，即使纵向距离很近也豁免 AEB 干涉
    if (dist_to_path > corridor_width / 2.0) {
        return false; // 豁免触发
    }
    return true; // 存在真实碰撞危险
}
```

---

## 4. 故障看门狗与降级接管（Fail-Safe Watchdog）

安全包络节点内部维护一组针对上游核心算法的心跳定时器：

```c
/* 周期性执行检查 (100Hz 独立线程) */
uint64_t now_us = clock_now_monotonic_wall_us();

// 检查规划模块心跳（若超过 200ms 未更新规划指令）
if (now_us - last_planning_cmd_time_us > 200000) {
    LOG_FATAL("Guardian", "规划节点心跳丢失 (超过 200ms)，触发安全刹停接管！");
    safety_override_active = true;
    apply_emergency_brake();
}
```

---

## 5. 工业级避坑指南

### 避坑 1：AEB 与底盘执行器超调产生的“点头顿挫”
- **现象**：当 AEB 触发全力刹车时，由于悬架弹簧压缩，雷达俯仰角向下倾斜，导致地面被误识别为障碍物，使 AEB 陷入无法解除的死循环。
- **解决方案**：在 AEB 触发期间，融合轮速计速度与 IMU 纵向加速度；当车速已降至 $0\text{ m/s}$ 时，平滑释放刹车压力并退出急停状态。

### 避坑 2：安全仲裁权重的原子性控制
- 安全包络指令必须在 `MessageBus` 和 `IpcChannel` 上具有最高仲裁优先级。当 `safety_control_node` 发布 `override = true` 时，底盘执行器节点必须在驱动硬件寄存器层直接丢弃常规 `control/cmd`，只响应 `safety/cmd`。

---

*第三卷完结。下一章将进入【第四卷：仿真验证、学习闭环与运维】，深入探讨 FlowSim 多 edge 路网拓扑与 NPC 交互设计。*
