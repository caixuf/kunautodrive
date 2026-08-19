# 卷四 · 第四课：造一个仿真世界

## 本章问题

你的车学会超车了（卷三）。但它在哪超的？一条**无限长的直线**——太无聊了。

真实世界有弯道、有行人、有红绿灯、有对向来车、有路口。你要把这些都造出来，让车在
一个**像样的世界**里练习。这一章就是：**造一个仿真世界，并且保证这个世界没写错。**

## 你自己的答案：直接写死坐标

最简单的做法：在代码里写死一切。

```c
// 一辆车在 (80, -5.25) 朝东开，速度 3 m/s
spawn_npc(80, -5.25, 0, 3.0);
// 一个行人在 (200, 0)，等会儿横穿马路
spawn_pedestrian(200, 0);
```

看，多直接。但运行一天后你会撞上三个问题：

1. **改场景要改代码**：想让车换个起点？改 C 代码，重新编译。想让行人走快一点？
   再编译。**场景和代码绑死，调参像受刑。**
2. **不可复现**：NPC 的行为有点「随机」。你在仿真里看到一个 bug，想重跑一遍确认，
   结果这次车走了另一条路——**bug 时隐时现，没法查。**
3. **不可验证**：你怎么证明这个世界没写错？比如「车会不会飞出路面」——你难道等肉眼
   在 3D 里看到一辆车悬在半空才知道？

## 真正的方案 1：场景 = 数据，不是代码

把场景从代码里拿出来，变成一份 **JSON 数据**（`scenarios/*.json`），代码只负责
「读数据 + 跑起来」。这就是「场景即剧本」：代码是演员，场景是剧本，换剧本不用重编译。

```json
{
  "name": "straight_road",
  "random_seed": 137,
  "ego": {
    "x": 20.0, "y": 1.75, "heading": 3.14159,
    "init_speed": 10.0, "target_speed": 20.0,
    "wheelbase": 2.7, "length": 4.6, "width": 2.0
  },
  "road_network": {
    "edges": [
      {
        "id": 0, "type": "highway", "name": "main_road",
        "length_m": 3007.0, "lanes": 4, "lane_width": 3.5,
        "speed_limit": 22.0,
        "nodes": [[0,0,0], [100,0,0], [200,50,0], ...]
      }
    ],
    "junctions": [...]
  },
  "actors": [
    { "id": 1, "segment_id": 0, "type": "car", "s": 80, "l": -5.25, "vx": 3.0 }
  ],
  "traffic_lights": [...],
  "choreography": { ... }
}
```

**为什么用 JSON 而不是代码？** 因为「场景」是**测试用例**——测试用例应该像数据一样
可批量生成、可对比、可版本控制，而不是嵌在代码里的死值。这也为卷九的「场景矩阵回归」
铺了路：同一套代码，喂不同场景，跑出 PASS/FAIL 矩阵。

### 场景 JSON 的完整结构

一个场景 JSON 由以下顶层字段组成：

| 字段 | 作用 | 必填 |
|------|------|------|
| `name` | 场景名（日志/评估器用） | ✅ |
| `random_seed` | 确定性随机种子 | ✅ |
| `duration_s` | 仿真时长（0=跑完自动停） | 可选 |
| `ego` | 主车初始状态 | ✅ |
| `road_network` | 路网定义（edges + junctions） | ✅ |
| `actors` | NPC 车辆/行人列表 | 可选 |
| `traffic_lights` | 红绿灯定义 | 可选 |
| `choreography` | 编舞脚本（节拍式 NPC 调度） | 可选 |

`ego` 字段里的关键参数：

```json
{
  "x": 20.0,          // 世界 ENU x（东）
  "y": 1.75,          // 世界 ENU y（北），负值=右车道
  "heading": 3.14159, // 朝向弧度，0=东，π/2=北，π=西
  "init_speed": 10.0, // 初始速度 m/s
  "target_speed": 20.0 // 巡航目标速度 m/s
}
```

### road_network 的 edge 与 junction

**edge** 是一段连续道路，每个 edge 有独立的 type、车道数、节点序列：

```json
{
  "id": 0,
  "type": "highway",    // 渲染分支开关！见下表
  "length_m": 3007.0,
  "lanes": 4,
  "lane_width": 3.5,
  "speed_limit": 22.0,
  "nodes": [[x1,y1,z1], [x2,y2,z2], ...]  // 中心线折点
}
```

| edge.type | 渲染行为 | 使用场景 |
|-----------|---------|---------|
| `highway` | RoadView 平路 ribbon + 车道线 | 普通高速/城市快速路 |
| `urban` | RoadView 平路 + StreetlightView 路灯 + BarrierView 护栏 | 城市道路 |
| `viaduct_highway` | ViaductView 高架分支：抬高 deck(z=7)、桥墩 | 真正的高架/立交 |
| `ramp_curve` | RoadView 弯道 ribbon | 匝道/弯道 |
| `cross_road` | RoadView 十字路口 | 交叉口 |

**junction** 描述 edge 之间的分叉/汇合关系，让 Route::build() 能把 edge 链成一条主路。

### 场景里最容易踩的坑（都真实发生过）

- **`edge.type` 是前端渲染的唯一开关**：写 `viaduct_highway`（高架），前端就会把
  NPC/红绿灯抬高 7 米——车藏进路面下看不见。平路场景**绝对禁用**高架类型。
- **别把会动的 NPC 放在路口内部的 connector 道上**：它会被投影到主路，突然出现在
  你的车前，触发「幽灵变道」。占位用静止车辆。
- **所有 actor 的 `s` 必须 ≥ 0**：负的弧长会走到路的起点，两辆车叠在一起。
- **`l` 值代表横向偏移**：`l=0` 是道路中心线，`l=-1.75` 是左车道中心（3.5m 车道宽时），
  `l=-5.25` 是 ego 车道左侧相邻车道。ego 在 y=-1.75 时，NPC 别放同车道近处。

## 真正的方案 2：NPC 不需要「智能」，需要「像样」

你可能会想：要造一个像样的交通流，是不是要给每辆车装一个「AI」？——不是。NPC 只需
一条很简单的规则，就能让交通看起来「活」起来：**IDM 跟车模型**。

```
安全间距 = 5.0 + 速度 × 1.5
如果前车太近 → 减速到前车速度
如果前面空旷 → 加速到限速
```

就这么简单。它造不出「聪明」的交通（NPC 不会主动超车变道），但造得出**真实**的交通：
车流会排队、会等红灯、会跟着前车走走停停。**对一个仿真器来说，「真实」比「聪明」
重要得多**——你要测的是你的车，不是 NPC 的车。

### IDM 跟车模型的实现细节

IDM（Intelligent Driver Model）的核心代码在 `npc_ai.cpp:121-132`：

```cpp
static double idm_desired_speed(double v, double gap, double target_v,
                                const NpcAiConfig& cfg, double dt) {
    // 安全间距 = base + v × time_headway
    double safe_gap = cfg.idm_safe_gap_base + v * cfg.idm_safe_gap_time;
    double gap_error = gap - safe_gap;
    if (gap_error > 0) {
        // 间距充足：平稳加速到目标速度
        return std::min(v + cfg.accel_rate * dt, target_v);
    }
    // 间距不足：gap_error 越负刹车越猛
    double brake = cfg.follow_decel_factor * std::exp(-gap_error / 2.0);
    return std::max(0.0, v - brake * dt);
}
```

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `idm_safe_gap_base` | 5.0 m | 静止安全间距 |
| `idm_safe_gap_time` | 1.5 s | 跟车时距 |
| `accel_rate` | 1.5 m/s² | 自由流加速度 |
| `follow_decel_factor` | 3.5 m/s² | 跟车减速度基准 |

前车搜索逻辑（`find_lead`）优先用 route_s 比较（沿路弧长），没有 route 时用车头
方向投影——这保证了对向车不会被误判为前车。

### MOBIL 变道模型（已禁用）

项目曾实现完整的 MOBIL（Minimizing Overall Braking Induced by Lane change）变道模型，
但因用户需求「NPC 各守其道不变道」已用 `#if 0` 禁用。完整代码保留在 `npc_ai.cpp` 中，
包含：

- `mobil_gain()`：变道收益函数 `gain = a'_c - a_c + politeness × (a'_n - a_n + a'_o - a_o)`
- `boundary_permissive()`：边界权限门——虚线才可变道，双黄/实线禁跨
- `find_leader_in_lane()` / `find_follower_in_lane()`：目标车道前/后车搜索

重新启用步骤：把 `enable_mobil` 配置置 true，验证 `lane_change_timer` 冷却期间
平滑插值无抖动即可。

### 一个历史教训：NPC 曾经会飞出路面

早期 NPC 的移动逻辑是「世界系直线积分」——不管道路怎么拐弯，NPC 都直着走。结果路
一拐弯，NPC 就冲出路面、飞到几千米外。修复：让 NPC **沿道路的弧长参数**走（Frenet
坐标），而不是在世界坐标里直线推进。**记住这个教训：在自动驾驶里，任何沿「路」运动的
东西，都应该用「路上的坐标系」来描述，而不是世界直线。**

## 真正的方案 3：物理模型——运动学 vs 动力学

仿真世界的核心是物理引擎。项目提供三种自行车模型（`physics.cpp`），从简单到复杂：

### 运动学自行车模型（默认）

```
纵向：accel = (throttle×5000 - brake×8000×sgn(v) - drag×v²) / mass
横向：yaw_rate = (speed / wheelbase) × tan(steer)
      heading += yaw_rate × dt
      x += (v×cos(h) - half_wb×sin(h)×yaw_rate) × dt
      y += (v×sin(h) + half_wb×cos(h)×yaw_rate) × dt
```

**核心假设**：前后轮同平面、无侧滑。适用于常规乘用车仿真（v < 30 m/s）。

注意位置更新用的是**车辆中心**而非后轴——后轴到中心有一个 `half_wb × yaw_rate`
的切向修正项。早期版本漏了这个修正，导致掉头时「车尾横移」。

### 线性轮胎二自由度动力学模型

```
前轴滑移角：α_f = steer - atan2(v_y + a×r, v_x)
后轴滑移角：α_r = -atan2(v_y - b×r, v_x)
侧向力：F_yf = C_αf × α_f,  F_yr = C_αr × α_r
横向加速度：v_y' = (F_yf + F_yr)/m - v_x × r
横摆角加速度：r' = (a×F_yf - b×F_yr) / I_z
```

**关键参数**（`physics.cpp:258-310`）：

| 参数 | 轿车 | SUV | 卡车 |
|------|------|-----|------|
| wheelbase | 2.7 m | 2.85 m | 5.0 m |
| mass | 1500 kg | 1800 kg | 8000 kg |
| tire_stiffness_f/r | 80000 N/rad | 90000 | 180000 |
| yaw_inertia | 2250 kg·m² | 3200 | 25000 |

**低速退化**：v < 5 m/s 时自动退化运动学——前向欧拉对高横向刚度在低速发散，
且低速轮胎滑移可忽略，运动学既够用又更稳。

### Pacejka 魔术公式（最高保真度）

```
F_y = D × sin(C × atan(B×α - E×(B×α - atan(B×α))))
D = μ × F_z（峰值受附着系数限制）
```

Pacejka 不硬饱和滑移角，由魔术公式自身平滑进入饱和，更贴近真实摩擦圆。
μ 可降以模拟湿滑/积雪路面。启用方式：`pipeline.json` 中 flowsim 节点 params 加
`"physics_model":"pacejka"`。

### 执行器模型

所有物理模型共用转向执行器模型（`update_steer`）：

```cpp
// 一阶滞后 (τ≈0.15s EPS) + 速率限幅
double alpha = dt / (e.steer_tau + dt);
double steer_next = e.steer + alpha * (steer_cmd - e.steer);
// 速率限幅
double max_rate = e.steer_rate_max * dt;
```

正常限幅 0.25 rad（≈14°），掉头时放宽到 0.60 rad（≈34°）——窄路掉头要打死方向盘。

## 真正的方案 4：场景事件——红绿灯与 ETC 闸门

场景不只是车和路，还有**事件**：红绿灯变色、ETC 闸门抬杆。事件调度在
`scene_events.cpp` 中实现。

### 红绿灯相位推进

红绿灯基于仿真时间的 `fmod` 计算当前相位：

```
T = green + yellow + red
tp = fmod(sim_time + offset, T)

tp < steady_green      → Green
tp < green             → FlashingGreen（绿灯末尾闪烁）
tp < green + yellow    → Yellow
else                   → Red
```

场景 JSON 里红绿灯的配置：

```json
{
  "traffic_lights": [
    {
      "id": 100, "x": 200.0, "y": 0.0,
      "green_s": 15.0, "yellow_s": 3.0, "red_s": 12.0,
      "offset": 0.0,
      "lane_y": -1.75
    }
  ]
}
```

`phase_state` 用 Entity 的 `throttle/brake/steer` 字段复用存储相位时长——这是历史
设计选择，不是最优方案，但稳定工作。

### NPC 响应红绿灯

`check_npc_scene_events` 检查每个 NPC 前方 60m 内是否有红灯/黄灯：
- 制动距离 = v² / (2×max_brake)
- 若最近红灯 < 制动距离 + 5m → 触发 `NpcEvent::TL_Red`，进入 `StopForTL` 状态
- 灯转绿 → `NpcEvent::TL_Green`，恢复巡航

### ETC 闸门

ETC 闸门根据 ego 距离切换状态（线性插值抬杆进度）：

```
dist > 50m  → closed（关闭）
dist > 10m  → opening（抬杆中，progress += dt×0.5）
dist > 0m   → open（全开）
dist ≤ 0    → closed（通过后关闭）
```

NPC 不响应 ETC 闸门（只有 ego 需要等抬杆）。

### 编舞系统（Choreography）

编舞是场景级别的 NPC 调度机制：按 `loop_period_s` 循环，每个 beat 在指定时刻触发。

```json
{
  "choreography": {
    "enabled": true,
    "loop_period_s": 30.0,
    "beats": [
      { "t": 5.0, "actor": "1", "act": "cutin", "ds": 80, "vx": 15.0 },
      { "t": 10.0, "actor": "tl", "phase": "red" },
      { "t": 20.0, "actor": "2", "act": "brake", "vx": 0.0 }
    ]
  }
}
```

| act 类型 | 行为 |
|----------|------|
| `cutin` | 传送 NPC 到 ego 前方 ds 米 + 横向偏移 dl 米，触发 CutIn 状态机 |
| `overtake` | 传送 + 超车状态 |
| `brake` | 原地设置目标速度（不传送），NPC 自然减速/加速 |
| `cross` | 行人横穿（设置 vy） |

红绿灯 beat（`actor: "tl"`）可直接覆盖所有红绿灯的相位，锁定一段时间。

## 真正的方案 5：怎么证明世界没写错（Digest + Invariant）

现在是最重要的一节。你说「我造了一个世界」——怎么证明它没写错？

项目的方法是：**把空间关系编码成数值，用断言检查。** 分两步：

**第一步，建「摘要」（digest）**：把世界的关键事实抽出来——

- 静态摘要（路网加载后建一次）：每条车道的宽度、每条车道线的虚实、每个红绿灯朝向
  哪条车道、路面范围。
- 动态摘要（每 20 帧）：每辆车的坐标、朝向、速度、在哪个车道、偏离车道中心多远。

**第二步，跑「不变式」（invariant）**：这些事实必须满足的硬规则——

| 不变式 | 抓什么 bug |
|--------|-----------|
| 车道宽度 ∈ [2.5, 4.0] 米 | 车道宽度配置错 |
| 每辆车离路面的高度 < ε | 车浮空或埋地 |
| 每辆车偏离车道中心 ≤ 半路宽 | 车飞出路面 |
| 车头方向与运动方向夹角 < 30° | 车横着/倒着开 |
| 位置变化 ≈ 速度 × 时间 | 车瞬移 |
| 红绿灯朝向与车道方向相反 | 红绿灯背对来车 |
| 两车 bbox 不重叠 | 穿模/重叠 |
| accel ∈ [-8, +4] m/s² | 运动学不可行 |

这些断言在**提交前**跑，而不是等肉眼发现。**「车飞出路面」这类 bug，从此不再是
「某天在 3D 里碰巧看见」，而是「提交时红灯拦下」。** 这是「仿真即验证」理念的基石：
仿真不只用来跑，还用来**自我检查**。

## 一个细节：仿真必须「可复现」

还记得你自己的答案里那个问题吗——「重跑一次，结果不一样」。解决方案：**随机种子**。
场景 JSON 里的 `random_seed` 决定所有「随机」行为（NPC 变道时机、噪声）。同一个
seed，跑一百次结果完全一样；换一个 seed，是「另一个确定的世界」。

**为什么可复现如此重要？** 因为调试的本质是「把 bug 固定住」——如果每次跑都不一样，
你连「bug 在不在」都没法确认。**确定性是调试的前提。**

## 仿真 vs 真实世界的差距

仿真永远是真实的**近似**。项目诚实地列出差距：

| 维度 | 仿真 | 真实世界 | 差距影响 |
|------|------|---------|---------|
| 物理模型 | 自行车模型（2-3 DOF） | 轮胎多体动力学（14+ DOF） | 极限工况（漂移/侧翻）不精确 |
| 传感器 | 确定性数据 | 噪声/延迟/遮挡 | 感知算法需额外鲁棒性 |
| NPC 行为 | IDM 跟车（无变道） | 人类驾驶（不可预测） | 场景覆盖率受限 |
| 天气/光照 | 简单参数（能见度/湿度） | 复杂交互 | 恶劣天气算法难以充分测试 |
| 执行器 | 理想延迟+限幅 | 非线性响应+老化 | 控制精度差异 |

**如何弥合？** 项目的策略是：仿真侧重**场景覆盖率**（快速迭代、可复现、可批量），
真实测试侧重**物理保真度**（验证仿真中发现的问题）。两者互补而非替代。

## 性能：如何高效运行 120s 场景

一个 120s 的仿真场景，背后是：

```
120s × 20Hz = 2400 帧
每帧：N 个实体 × (物理积分 + 前车搜索 + 事件检查)
```

几个性能关键点：

1. **物理积分是 O(N)**：每个实体独立积分，不互相依赖（除了前车搜索）。
2. **前车搜索是 O(N²)**：每个 NPC 搜索同车道前车。N < 50 时可接受，N > 100
   需要空间索引（当前未实现）。
3. **Invariant 检查每 20 帧跑一次**：不是每帧都检查，降低开销。
4. **SSE 传输 5-10Hz**：后端不每帧都推数据给前端，前端靠死推算补帧率。

```bash
# 实际 benchmark：straight_road 场景 120s
# 单核耗时：~8s（20Hz × 120s = 2400 帧，~3ms/帧）
# 内存：~50MB（EntityPool + RoadNetwork + Route）
```

## 动手实践

```bash
# 1. 造一个新场景：复制 straight_road.json，改几处
cp scenarios/straight_road.json /tmp/my_scene.json
#    改：加一个 actor、换一个红绿灯位置、改 target_speed

# 2. 跑你的场景
./build/bin/flow_launcher config/pipeline.json --duration 20 \
    --scenario /tmp/my_scene.json   # 或改 pipeline.json 的 scenario_file

# 3. 看 digest/invariant 是否通过（启动后找日志里的 invariant 结果）
#    "INVARIANT OK" = 世界没写错

# 4. 验证确定性：同一个 seed 跑两次，轨迹应该一模一样

# 5. 切换物理模型（在 pipeline.json 的 flowsim 节点 params 里）
#    "physics_model": "kinematic"   ← 默认运动学
#    "physics_model": "dynamic"     ← 线性轮胎动力学
#    "physics_model": "pacejka"     ← 魔术公式动力学
```

## 小结

这一章你造了一个世界，并学会了五件事：

- **场景 = 数据**：JSON 是剧本，代码是演员，换剧本不用重编译——可批量、可对比、可复现。
- **NPC 只需要「像样」**：一条 IDM 跟车规则就能造出真实的交通流，不需要「聪明」；
  沿路运动的实体用「路上的坐标系」，否则会飞出路面。
- **物理模型有层次**：运动学（简单稳定）→ 线性动力学（轮胎侧滑）→ Pacejka（魔术公式），
  低速统一退化运动学，按需选择保真度。
- **场景事件让世界「活」起来**：红绿灯按 fmod 循环变色，NPC 检测前方红灯自动停车，
  编舞系统按节拍精确调度 NPC。
- **世界要能被验证**：digest 把世界抽成数值，invariant 在提交前拦下「飞出路面/浮空/
  瞬移」类 bug；随机种子让世界可复现，调试才有意义。

## 练习（选做）

1. **读场景**：打开 `scenarios/straight_road.json`，找到 `actors` 数组，数一下有几辆车、
   几个行人。它们的 `s`（弧长）和 `l`（横向偏移）分别代表什么？
2. **思考题**：为什么「沿路运动的实体用路上的坐标系」能避免飞出路面？提示：弧长
   + 横向偏移 = 永远在路上的描述方式。
3. **挑战**：物理模型里为什么低速要退化运动学？如果在 v=2 m/s 时用动力学模型，
   会发生什么？（提示：前向欧拉在高刚度低速区的稳定性）
4. **进阶**：读 `npc_ai.cpp` 的 `recycle_npc`，理解 NPC 到达 route 末端后怎么
   回到 ego 附近形成持续车流。为什么回收点要按 id 错开？

---

**下一卷预告**：世界有了，但还缺一样东西——**路本身从哪来**。卷五讲地图引擎：怎么把
一条真实街道（甚至一座城市）变成车能开、能渲染的 `map.json`。
