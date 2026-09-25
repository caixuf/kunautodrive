# 第 17 章：最后一道不许讲道理的闸门

前面几章的算法再稳，也只是统计意义上的稳：神经网络会偶发退化（Model Failure），多项式规划偶尔会吐出 NaN，融合也总有看走眼的时候。只要把这些原始输出不打招呼地塞给刹车和转向，一次异常就够酿成大祸。所以 KunAutoDrive 在最底层单独放了一个安全包络（Safety Envelope / Guardian Node）：它实时优先级最高，替所有算法守住物理底线——TTC 监控、横向干涉豁免、异常指令钳位（Clamp），以及和硬件直连的急停。

## 一道独立的底座

```
  [Planning Node 规划输出] ──► [Control Node 控制解算] 
                                      │
                                      ▼
                        [raw_cmd (待审核的控制指令)]
                                      │
                                      ▼
        ┌─────────────────────────────────────────────────────────────┐
        │      Safety Control / Guardian 安全防护包络 (最高优先级)    │
        │                                                             │
        │  ├── 1. 指令有效性检查 (NaN/Inf 过滤、最大转角/加速度限制)  │
        │  ├── 2. TTC 碰撞时间物理闸门 (Time-To-Collision 评估)       │
        │  ├── 3. 行人/易脆弱群体穿越强行制动 (VRU Protection)        │
        │  └── 4. 心跳看门狗超时 (上位机进程死锁则自动触发 AEB)       │
        └──────────────────────────────┬──────────────────────────────┘
                                       │ 仲裁后的安全指令
                                       ▼
                         [actuator/cmd (真车执行器)]
```

## TTC：还剩多少秒会撞上

碰撞时间（Time-To-Collision, TTC）算的是这么一件事：假定两车相对速度不变，从现在到真正发生物理接触还剩多少秒。

$$\text{TTC} = \frac{d_{\text{rel}} - d_{\text{safe}}}{v_{\text{ego}} - v_{\text{target}}} \quad (\text{当 } v_{\text{ego}} > v_{\text{target}})$$

```
TTC 分级响应阶梯:
  TTC > 3.0s:      [绿灯] 正常行驶，安全包络不做干预
  2.0s < TTC <= 3.0s: [黄灯 Warning] 仪表盘声光预警，预充液压制动器 (Pre-fill)
  1.0s < TTC <= 2.0s: [橙灯 Partial Braking] 减速 0.3g，协助驾驶员/规划减速
  TTC <= 1.0s:     [红灯 Hard AEB] 立即覆盖并剥夺规划控制权，最大全力制动 (-1.0g)
```

## 别让护栏和隔壁车吓出急刹

城市路口转弯、或者绕行路边一辆违停车的时候，前向雷达的视场角（FOV）很容易扫到路边护栏或相邻车道的车。这时候要是还拿全向 TTC 去触发急停，等着的就是一片误刹车（Phantom Braking）。KunAutoDrive 的做法是基于车道走廊，把横向横偏投影剔除掉：

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

## 上游卡死了，谁来踩刹车

安全包络节点里常驻着一组心跳定时器，盯的都是上游那几个核心算法：

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

## 两件在真机上才暴露的事

第一件出在 AEB 全力刹车的时候。刹车一沉，悬架弹簧被压下去，雷达俯仰角跟着向下偏，地面就被误当成了障碍物，AEB 于是陷入既刹不停、也解除不掉的死循环。解法是在 AEB 触发期间把轮速计的速度和 IMU 的纵向加速度融合进来；等车速降到 $0\text{ m/s}$，再平滑地释放刹车压力、退出急停状态。

第二件跟「谁说了算」有关。安全包络的指令必须在 `MessageBus` 和 `IpcChannel` 上拿到最高仲裁优先级：一旦 `safety_control_node` 发出 `override = true`，底盘执行器节点就得在驱动硬件寄存器的那一层直接丢掉常规 `control/cmd`，只认 `safety/cmd`。这条链路只要有一环没做到原子，安全指令就可能和普通控制指令打架。
