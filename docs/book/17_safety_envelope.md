# 第 18 章：安全包络与降级 —— 仲裁、TTC、熔断阶梯

> **本章导读**：
> 前 17 章的所有算法都只是「统计意义上的稳」：多项式规划偶尔吐 NaN，EKF 会看走眼，纯学习模型会偶发退化。这些异常一旦原封不动地送到刹车和转向，就是一次事故。
>
> KunAutoDrive 的做法是在控制节点和执行器之间插一道**安全闸门**（`safety_control_node`），让所有指令先过一遍物理合理性审查。本章讲清三个问题：
>
> 1. **谁说了算** —— 规则控制（PID + 几何级联）和学习模型（端到端 MLP）同时在线时，`safety_arbiter_apply` 的仲裁优先级怎么排；
> 2. **什么时候踩刹车** —— TTC 的真实公式、真实阈值，以及为什么它**不是**教科书里那套 3.0/2.0/1.0 秒的分级阶梯；
> 3. **上游卡死了怎么办** —— `degrade_ladder` 的 L0~L3 熔断阶梯、粘滞锁存、以及两套互相独立的心跳监控。
>
> 同样先说破：**上一版这一章里的示例代码，几乎全是凭空写的**——`is_obstacle_in_collision_corridor()` 这个函数在仓库里不存在，教科书式的 TTC 三级阶梯不存在，`safety/cmd` 这个话题不存在，「规划节点 200 ms 心跳丢失」的看门狗也不存在。本章全部按源码重建。

---

## 1. 这道闸门长什么样

### 1.1 位置与数据流

```
  [control_node]  ──control/raw_cmd──┐
   PID + 横向级联                    │
                                     ▼
  [inference_node] ──inference/raw_cmd──► [ safety_control_node ]
                                            │
  fusion/localization ─────────────────────► │  1. 仲裁（规则 vs 模型）
  perception/obstacles ───────────────────► │  2. 指令包络钳位
                                            │  3. TTC / 障碍物守卫
                                            │  4. 降级阶梯执行
                                            ▼
                                        control/cmd
                                     （+ safety/evidence）
                                            │
                                            ▼
                       flowsim / actuator_pwm / actuator_node
```

三路输入里有一个容易看漏的细节（`safety_control_node.cpp:463`）：

```cpp
BusQueueBridge cmd_bridge(bus(), {"control/raw_cmd", "inference/raw_cmd"});
```

**`inference/raw_cmd` 走的是 `BusQueueBridge`，不是 `transport_subscribe`**。而 `s_inputs[]`（`:886-887`）里只声明了 `control/raw_cmd`：

```cpp
const char* s_inputs[] = {"control/raw_cmd", TOPIC_FUSION_LOCALIZATION, TOPIC_PERCEPTION_OBSTACLES, nullptr};
```

`TOPIC_FUSION_LOCALIZATION` 和 `TOPIC_PERCEPTION_OBSTACLES` 走 `transport_subscribe`（`:935-936`），原始控制指令走总线桥。**三路输入，两种传输机制**——第 4 章讲过的架构分层在这里留下了一个痕迹。

### 1.2 轮询节拍 5 ms，输出频率随消息

```cpp
co_await sleep_us(5000);  /* 5ms 轮询节拍（消息驱动 → 固定周期） */   /* :570 */
```

任务循环是 **200 Hz 固定节拍**（`TASK_PRIORITY_NORMAL`，`:962`），但 `control/cmd` 的发布是**消息驱动**的：每从桥里取出一条原始指令，才跑一次完整的仲裁 + 包络 + 发布。所以输出频率上限受限于上游（控制节点 40 Hz），而不是这个 200 Hz 的节拍。

**200 Hz 节拍真正的用途**是第 4 节的数据超时看门狗——它必须独立于消息流持续运行。

### 1.3 在管道里的地位：默认配置下不可绕过

`config/pipeline.json:293-318`：

```json
{
  "name": "safety_control",
  "library_path": "build/lib/libsafety_control_node.so",
  "auto_start": true,
  "subscribe": ["control/raw_cmd", "fusion/localization", "perception/obstacles"],
  "publish": [
    { "topic": "control/cmd", "type": "ControlCmd",
      "qos": { "depth": 8, "policy": "drop_oldest",
               "reliability": "best_effort", "deadline_ms": 50 } },
    { "topic": "safety/evidence" }
  ],
  "params": "{\"max_throttle\":1.0,\"max_steer\":0.22,\"low_speed_steer\":0.18,\"time_headway\":1.3}"
}
```

**默认管道里没有任何节点绕过这道闸门。** `control/cmd` 的唯一消费者是 `flowsim`（`pipeline.json:19-21`）。

> [!IMPORTANT]
> **唯一的例外是手动驾驶模式。** `config/pipeline_manual.json` 里没有 `safety_control` 节点，它的 `allow_hung_subs` 明确写着「manual 模式无 safety_control」。此时 `manual_drive_node.c:252` **直接发布 `control/cmd`**，整道闸门被完全跳过。这是设计意图（人开车时不需要自动驾驶的仲裁），但也意味着**手动模式下一条 NaN 指令可以直达执行器**。
>
> 顺带一提，`pipeline_car.json:253-268`（RC 小车配置）也存在一处不匹配：它只订阅 `fusion/localization` + `perception/obstacles`，**不订阅 `control/raw_cmd`**，也不发布 `safety/evidence`。该配置标记为 `experimental`，所以 `ci/gates/topic_contract_check.py` 的「JSON ⊆ 代码」检查放行了它——但节点在这个配置里实际上是不工作的。

### 1.4 输出是另一个结构体

一个容易忽略但对理解架构很关键的细节：**输出不是 `ControlRaw`，而是完全不同的 `ControlCmd`**。

```
# msg/adas_msgs.msg:204-213
struct ControlCmd {
    uint32   seq            # 指令序号
    float    throttle       # 油门 [0.0, 1.0]
    float    brake          # 制动 [0.0, 1.0]
    float    steering       # 转向角（rad）
    Gear     gear           # 档位
    bool     emergency_stop # 紧急制动标志
    uint8    turn_signal    # 转向灯指令
    bool     hazard         # 双闪灯指令
}
```

对比第 17 章的 `ControlRaw`（`msg/adas_msgs.msg:189-202`，12 字段 / 59 字节）：

| | `ControlRaw`（输入） | `ControlCmd`（输出） |
|---|---|---|
| 字段数 | 12 | **8** |
| 序列化大小 | 59 B | **20 B** |
| type_id | `0xafeb3d23` | `0xed9c7088` |
| 独有字段 | `speed` `target` `error` `cte` `mode` | **`emergency_stop`** |

安全节点**剥掉了所有监控/调试字段，只留执行器真正需要的量**，并且新增了 `emergency_stop`。带宽从 59 B 降到 20 B，200 Hz 也就 4 kB/s——这是刻意的设计：**下游只该有能力做一件事，而不该有能力做别的事**。

> [!WARNING]
> `safety_control_node.cpp:38-39` 里硬编码的类型 ID 是**过期的**：
> ```cpp
> constexpr uint32_t CONTROL_RAW_TYPE_ID = 0x871712d1u;  /* CONTROLRAW_TYPE_ID (adas_msgs_gen.h) */
> constexpr uint32_t CONTROL_CMD_TYPE_ID = 0x2D95C6D2u;  /* CONTROLCMD_TYPE_ID (adas_msgs_gen.h) */
> ```
> 而生成头文件里的实际值是 `0xafeb3d23` 和 `0xed9c7088`。`CONTROL_RAW_TYPE_ID` 只在 `discovery_advertise`（`:954`）里用到；`CONTROL_CMD_TYPE_ID` 用于 `transport_advertise`（`:937`、`:957`）。同一个文件第 40 行的 `VEHICLE_STATE_TYPE_ID` 声明了但从未使用，是死代码。

---

## 2. 仲裁：规则控制 vs 学习模型，谁说了算

这是整章最值得学的部分，也是**旧版这一章完全缺失的**。KunAutoDrive 有一条端到端学习链路（见第 23 章）会发布 `inference/raw_cmd`。同一条车上跑着两套控制源，安全节点必须回答一个问题：**模型的输出，能信多少？**

### 2.1 仲裁接口

```c
/* modules/adas_nodes/safety_arbiter.h:52-56 */
SafetyArbiterCmd safety_arbiter_apply(const SafetyArbiterCmd* rule_cmd,
                                      const SafetyArbiterCmd* model_cmd,
                                      int has_fresh_model,
                                      int is_degraded,
                                      int* out_intervened);
```

注意输入结构体（`safety_arbiter.h:20-27`）只有 6 个字段：`throttle, brake, steer, turn_signal, hazard, gear`——**没有 `speed`/`target`/`mode`**。仲裁器看到的只有「动作」，看不到「理由」。

三个阈值（`safety_arbiter.h:30-34`）：

```c
#define SAFETY_ARBITER_STEER_ENVELOPE_RAD  0.12    /* 转向包络 6.9° */
#define SAFETY_ARBITER_RULE_BRAKE_ACTIVE   0.10    /* 规则"在刹车"的判据 */
#define SAFETY_ARBITER_MODEL_THROTTLE_CAP  0.85    /* 模型油门上限 */
```

### 2.2 优先级链

实现只有 60 行（`safety_arbiter.c:13-60`），但每一层都对应一个明确的安全命题：

```c
/* P0: 没有规则指令 → 什么都不做 */
if (!rule_cmd) {
    if (out_intervened) *out_intervened = 0;
    return zero;
}

/* P1: 门闸——模型不可信时，规则全权接管。
 * 注意 intervened = 0：这不是"干预"，这是"本来就该听规则的"。 */
if (!has_fresh_model || is_degraded || !model_cmd) {
    if (out_intervened) *out_intervened = 0;
    return *rule_cmd;
}

SafetyArbiterCmd out = *rule_cmd;
int intervened = 0;

/* 1. 转向角安全包络：模型偏离规则基线超过 6.9° 判定超限，拒绝模型转向 */
const double delta_steer = fabs(model_cmd->steer - rule_cmd->steer);
if (delta_steer > SAFETY_ARBITER_STEER_ENVELOPE_RAD) {
    out.steer = rule_cmd->steer;
    intervened = 1;
} else {
    out.steer = model_cmd->steer;
}

/* 2. 纵向安全仲裁：规则处于制动态时以规则为主，禁止模型油门冲撞 */
if (rule_cmd->brake > SAFETY_ARBITER_RULE_BRAKE_ACTIVE) {
    out.brake = fmax(rule_cmd->brake, model_cmd->brake);
    out.throttle = 0.0;
    intervened = 1;
} else {
    const double max_thr = fmax(rule_cmd->throttle, SAFETY_ARBITER_MODEL_THROTTLE_CAP);
    out.throttle = fmin(model_cmd->throttle, max_thr);
    out.brake = model_cmd->brake;
}

/* 3. 灯光与档位继承规则安全态 */
out.turn_signal = (rule_cmd->turn_signal != 0) ? rule_cmd->turn_signal
                                               : model_cmd->turn_signal;
out.hazard = (rule_cmd->hazard || model_cmd->hazard) ? 1 : 0;
out.gear = rule_cmd->gear;

if (out_intervened) *out_intervened = intervened;
return out;
```

整理成表：

| 优先级 | 条件 | 行为 | 计入干预 |
|---|---|---|---|
| P0 | `rule_cmd == NULL` | 全零输出 | 否 |
| **P1** | `!has_fresh_model` \|\| `is_degraded` \|\| `!model_cmd` | **规则全权**（逐字段原样返回） | **否** |
| P2 | `\|steer_model − steer_rule\| > 0.12` rad | 转向角回退到规则值 | 是 |
| P3 | `rule.brake > 0.10` | `brake = max(规则, 模型)`，`throttle = 0` | 是 |
| P3′ | 否则 | `throttle = min(模型, max(规则, 0.85))` | 否 |
| P4 | 灯光/档位 | 规则优先，双闪取或 | 否 |

### 2.3 三个精妙的设计决策

**决策一：降级的优先级高于模型（`is_degraded` 在 P1）**

一旦系统进入 L1 及以上，模型输出就被整体屏蔽。理由很直接：**降级状态下系统的行为规范已经变了**（比如禁止变道、限制速度），而模型是按全功能状态训练出来的，它根本不知道现在处于降级——让它继续输出就是在执行一个过时的策略。

**决策二：P1 不计入 `intervened`**

```c
if (!has_fresh_model || is_degraded || !model_cmd) {
    if (out_intervened) *out_intervened = 0;   /* ← 特意置 0 */
    return *rule_cmd;
}
```

这是 `intervened` 标志的**语义定义**：它回答的是「**我是否否决了模型**」，而不是「输出和规则是否一致」。模型没上线、降级生效，这些情况下输出确实等于规则指令，但**没有发生任何否决**。这个区分让 `control/debug` 里 `ARBITER[MODEL]` 和 `ARBITER[OVERRIDE]` 两个标签的语义保持干净（`safety_control_node.cpp:407-436`）：

```cpp
out.mode = intervened ? "ARBITER[OVERRIDE]" : "ARBITER[MODEL]";
```

如果把 P1 也算作干预，那么模型从未上线时日志会满屏 `OVERRIDE`，真正的否决反而被淹没。

**决策三：转向和纵向独立仲裁**

P2 只管 `steer`，P3 只管 `brake`/`throttle`，两者互不影响。这意味着**模型可以在纵向被否决的同时保留转向**（或反之）。对学习型控制这是合理的：横向和纵向的失效模式不同、风险不对称，不应该因为纵向超了就一票否决转向。

而且 P3 用的是**取最大值**（`fmax`）而不是「规则赢」：

```c
out.brake = fmax(rule_cmd->brake, model_cmd->brake);
```

**制动是取或，不是取谁。** 规则和模型都认为要刹车时，取**更狠的那个**。这个方向性选择是对的——纵向控制的错误代价是不对称的：漏刹会撞车，多刹只是不舒服。

**决策四（隐含）：模型可以任意刹车**

```c
} else {
    ...
    out.brake = model_cmd->brake;      /* ← 无下限、无上限 */
}
```

规则不在刹车状态时，模型的 `brake` 是**原样透传**的——没有地板也没有天花板。唯一的约束来自后面的 `apply_safety()` 里 `max_brake = 1.0` 的钳位。

这在直觉上有点怪：油门被限制在 0.85，刹车却完全不限制。但仔细想想是合理的——**油门开大只会让车撞上去，刹车开大最多是顿一下**。而一个「学会了该踩刹车」的学习模型，它的刹车值本来就该被信任；反过来，如果给它刹车设地板（比如 0.5），模型想轻点刹车减速跟车时就做不到了。

### 2.4 一个真实的架构漏洞

仲裁器本身写得没问题，但**调用它的那一行代码有一个 bug**（`safety_control_node.cpp:470-507`）：

```cpp
if (cmd_bridge.try_take_any(&topic, &msg)) {
    bool is_rule = (topic == "control/raw_cmd");
    if (is_rule) {
        arbiter.last_rule_cmd = parse_control_cmd(msg);
        arbiter.last_rule_cmd_us = now_us;
    } else {
        arbiter.last_model_cmd = parse_control_cmd(msg);
        arbiter.last_model_cmd_us = now_us;
        /* 若规则主拍仍在活跃期（100ms 内有 rule cmd），则由规则主拍统一仲裁 */
        if (now_us - arbiter.last_rule_cmd_us < 100000ULL) {
            continue;
        }
    }
    ...
    ControlCmd cmd = is_rule
        ? arbitrate_control(arbiter.last_rule_cmd, arbiter.last_model_cmd,
                            has_fresh_model, is_degraded, &arbiter_intervened)
        : base_cmd;                      /* ←←← 问题在这里 */
```

当桥里取出的是 **`inference/raw_cmd`**（模型指令）时，代码走的是 `: base_cmd` 分支——`base_cmd` 是从那条模型消息直接解析出来的，**`safety_arbiter_apply` 根本没被调用**。

后果：模型指令在「规则指令超过 100 ms 未到达」时，会**绕过整个仲裁器直通 `apply_safety()`**。也就是说 P2 的转向包络（0.12 rad）、P3 的油门上限（0.85）、P1 的降级屏蔽——**在这条路径上全部失效**。

从频率上看这条路径并不罕见：`control_node` 是 40 Hz（25 ms 一条），100 ms 的去重窗口意味着正常运行时模型指令几乎总是被 `continue` 掉。但**一旦控制节点卡住超过 100 ms**，模型指令就会以每条直通，而此时系统恰恰是最需要仲裁的时候。

> 这个 bug 的危险性在于它和故障条件高度相关：**仲裁器在系统正常时工作得很好，在系统出问题时反而被绕过。** 这是一个典型的「保护逻辑的失效模式与它要保护的对象的失效模式相关」的设计缺陷。

---

## 3. 指令包络：NaN、限幅与钳位

### 3.1 NaN 防护的两道防线

第一道在 `clamp`（`safety_control_node.cpp:102-109`）：

```cpp
double clamp(double value, double lo, double hi) {
    /* IEEE-754 下 NaN < x 恒为 false，未加防护时 clamp(NaN, 0.0, max_brake) 会
     * 返回 0.0（不刹车），clamp(NaN, -steer_limit, steer_limit) 会返回 -steer_limit
     * （一侧打死）。NaN/Inf 输入直接返回 lo（"不刹车/不转向"安全侧）；brake 的
     * 安全侧在 publish_cmd 里再做一次显式 isfinite 紧急刹车兜底。 */
    if (!std::isfinite(value)) return lo;
    return std::max(lo, std::min(value, hi));
}
```

**IEEE-754 里 `NaN` 和任何值比较都返回 false**，这导致了一个极其阴险的失效：

```cpp
std::min(NaN, 1.0)      // 编译为 a < b ? a : b → false < true → 返回 NaN 那一侧？
std::max(0.0, NaN)      // 取决于实现
```

实际结果是 `clamp(NaN, 0.0, 1.0)` 返回 **0.0**——**「不刹车」**。一次数值发散（规划层吐了 NaN）会被安全层「温柔地」翻译成「松开刹车」。这是最坏的失效方向：出错的不是刹车，是**刹车的解除**。

对 `steer` 同样危险：`clamp(NaN, -0.22, 0.22)` 会返回 `-0.22`——**打死一侧**。

`if (!std::isfinite(value)) return lo;` 这一行把两侧都拉到了安全侧：刹车 0.0、转向 −limit。但**0.0 对刹车来说仍然不是安全侧**（安全侧应该是 1.0），所以有了第二道：

第二道在 `publish_cmd`（`safety_control_node.cpp:816-830`），在所有检查之后：

```cpp
/* NaN/Inf 兜底：clamp 已把 NaN/Inf 收敛到 lo，但 brake 的 lo=0.0 意味着
 * "不刹车"，对制动不安全。发布前再做一次显式 isfinite 复查，任一字段
 * 非有限 → 强制 emergency_stop（brake=1.0, throttle=0.0, steer=0.0）。 */
if (!std::isfinite(cmd.throttle) || !std::isfinite(cmd.brake) || !std::isfinite(cmd.steer)) {
    bin.throttle       = 0.0f;
    bin.brake          = 1.0f;
    bin.steering       = 0.0f;
    bin.emergency_stop = true;
    fprintf(stderr, "[safety] NaN/Inf in control cmd, forcing emergency stop\n");
} else {
    bin.throttle       = (float)cmd.throttle;
    bin.brake          = (float)cmd.brake;
    bin.steering       = (float)cmd.steer;
    bin.emergency_stop = cmd.brake > 0.95;
}
```

**两道防线的分工**：第一道负责转向和油门（把 NaN 收敛到「不动作」），第二道专门兜住刹车（把 NaN 转成「全力刹车」）。代码注释把这条推理链写得很清楚。

顺带注意 `emergency_stop` 是**推导出来的，不是指令**：`bin.emergency_stop = cmd.brake > 0.95`。下游执行器据此判断这是一次紧急制动。

### 3.2 三个包络限幅

包络在 `apply_safety` 里（`safety_control_node.cpp:604-610`）：

```cpp
const double thr_lo = (cmd.gear == GEAR_REVERSE) ? -params_.max_throttle : 0.0;
set_changed(cmd.throttle, clamp(cmd.throttle, thr_lo, params_.max_throttle));
set_changed(cmd.brake,    clamp(cmd.brake, 0.0, params_.max_brake));
double steer_limit = maneuver ? 0.62
                   : (has_state && state.speed < 3.0) ? params_.low_speed_steer
                                                      : params_.max_steer;
set_changed(cmd.steer, clamp(cmd.steer, -steer_limit, steer_limit));
```

| 量 | 巡航限幅 | 条件 | 说明 |
|---|---|---|---|
| `throttle` | `[0, 0.85]` | 倒挡 `[-0.85, 0.85]` | 负油门即倒车 |
| `brake` | `[0, 1.0]` | — | |
| `steer` | `[-0.22, 0.22]` | 低速(<3 m/s) `±0.18`；机动 `±0.62` | |

**低速收紧转向**（0.22 → 0.18）是一条实车经验：低速时转向机构的非线性（齿隙、助力泵滞后）占比大，放大转向权限容易在小角度上产生大误差。

**机动放宽到 0.62 rad** 是为了容纳第 17 章讲的掉头弧（需要 0.60 rad 满舵）。机动判定很朴素（`:599-601`）：

```cpp
const bool maneuver = (cmd.gear == GEAR_REVERSE) ||
                      std::fabs(cmd.steer) > 0.30 ||
                      cmd.mode.find("MANEUVER") != std::string::npos;
```

三个条件任一满足即视为机动：倒挡、转向超 0.30 rad、或模式串里带 `MANEUVER`。

### 3.3 `set_changed` 与 `+SAFE` 标签

每一处修改都走 `set_changed`，它同时做两件事：**写值** + **置 `changed` 标志**。最后统一打标签（`:804-806`）：

```cpp
if (changed && cmd.mode.find("SAFE") == std::string::npos) {
    cmd.mode += "+SAFE";
}
```

所以实车日志里的 `ACCEL+SAFE`、`MRM+MANEUVER+SAFE`、`DATA_TIMEOUT+SAFE` 这样的模式串，**就是「安全层改过这条指令」的证据**。排查异常时的第一件事就是看这个标签。

---

## 4. TTC：真实的公式与真实的阶梯

### 4.1 破除误区：不存在 3.0/2.0/1.0 三级阶梯

旧版这一章写了一套教科书式的分级响应：

| 旧稿声称 | 实际情况 |
|---|---|
| TTC > 3.0s 绿灯 | **不存在** |
| 2.0 < TTC ≤ 3.0s 黄灯 + **预充液压制动器（Pre-fill）** | **不存在**，代码里没有任何预充液逻辑 |
| 1.0 < TTC ≤ 2.0s 橙灯，减速 **0.3g** | **不存在**，brake 是 [0,1] 归一化量，不是 g |
| TTC ≤ 1.0s 红灯，最大全力制动 **−1.0g** | 存在（`brake = 1.0`），但**没有 g 的概念** |

**真实的阈值是 2.5 / 1.5 / 1.0 三个，而且它们触发的不是三个「档位」，而是三件不同的事。**

### 4.2 真实的 TTC 公式

`min_vehicle_ttc`（`safety_control_node.cpp:285-317`）：

```cpp
const double fwd_x = std::cos(state.heading);
const double fwd_y = std::sin(state.heading);
for (int i = 0; i < kMaxObs; ++i) {
    if (!state.obs_valid[i]) continue;
    const double ex = state.obs_x[i] - state.x;
    const double ey = state.obs_y[i] - state.y;
    const double along = ex * fwd_x + ey * fwd_y;             /* 沿车头前方距离 */
    const double lat = std::fabs(-ex * fwd_y + ey * fwd_x);   /* 横向偏移 */
    if (along < 0.0 || along > 35.0 || lat > 2.3) continue;

    const double along_v = state.obs_v[i] * fwd_x + state.obs_vy[i] * fwd_y;
    const double closing = state.speed - along_v;
    if (closing <= 0.4) continue;

    const double clearance = along - 4.8;
    const double ttc = clearance / std::max(0.1, closing);
    if (ttc < best_ttc) { best_ttc = ttc; best_dx = along; best_dy = lat; }
}
```

$$\text{TTC} = \frac{d_{\text{along}} - 4.8}{\max\!\big(0.1,\; v_{\text{ego}} - v_{\text{obs,along}}\big)}$$

四个门限，每一个都有理由：

| 门限 | 值 | 含义 |
|---|---|---|
| `along` 范围 | `[0, 35]` m | 车头前方，35 m 感知上限 |
| `lat` | `≤ 2.3` m | 同车道判定（≈ 车道宽 3.5 m 的 2/3） |
| `closing` | `> 0.4` m/s | 接近速度太小时不报警（否则会除出巨大的 TTC） |
| `clearance` | `along − 4.8` | **4.8 m 是车身长度加余量**，不是「安全距离」 |

**4.8 这个数字值得单独说**：它是把「碰撞」定义为「两车车头接触」，然后往前推一辆车的长度。物理意义是「留给制动系统的最后余量」。

**方向感知是被显式修过的**（`:289-291`）：

```cpp
/* 方向感知（2026-08-04 掉头返程同向防撞失效）：旧实现用世界 dx=obs_x-ego_x，
 * 返程 ego 向西时前车在 -x（dx<0）被 skip → 同向 TTC 完全失效，返程无防撞。
 * 改为沿车头方向投影 ahead + 沿向速度，前进/返程统一。 */
```

**这是一个典型的坐标系陷阱**：用世界系的 `dx` 判断「在不在前方」，在车辆掉头返程（车头朝西）时，「前车在 −x」恰恰意味着**前车在自己正前方**。用世界坐标判断前后，等于把「前方」定义成「东边」——这个定义在车辆朝北时恰好正确，在朝西时就完全错了。而掉头返程正是最容易发生追尾的时刻。

### 4.3 真实的响应阶梯

**不是三级档位，是一个带迟滞的连续制动曲线**（`safety_control_node.cpp:661-711`）：

```cpp
static bool safety_vehicle_brake_latched = false;
double risk_dx = 0.0, risk_dy = 0.0;
double ttc = min_vehicle_ttc(state, &risk_dx, &risk_dy);

/* 停车距离 = v²/(2·a_brake) + τ·v + safety_margin
 * 参数：5 m/s² 满刹（typical dry pavement）+ 0.3s 反应 + 1.5m 余量。 */
constexpr double kBrakeDecel  = 5.0;
constexpr double kBrakeTau    = 0.3;
constexpr double kBrakeMargin = 1.5;
constexpr double kHysteresisGap = 2.0;   /* 释放比触发多这么多 */
const double stop_dist   = state.speed * state.speed / (2.0 * kBrakeDecel)
                         + kBrakeTau * state.speed + kBrakeMargin;
const double engage_dist = std::min(stop_dist + 0.5, 35.0);
const double release_dist = stop_dist + kHysteresisGap;

const bool no_obstacle = (ttc > 1e8);
if (no_obstacle) {
    safety_vehicle_brake_latched = false;
} else {
    const bool in_brake_zone = safety_vehicle_brake_latched
        ? (risk_dx < release_dist)     /* 已锁定：释放门槛更高 */
        : (risk_dx < engage_dist);     /* 未锁定：触发门槛更低 */
    const bool ttc_trigger = (ttc < 2.5);

    if (in_brake_zone || ttc_trigger) {
        safety_vehicle_brake_latched = true;
        set_changed(cmd.throttle, 0.0);
        if (risk_dx < stop_dist) {
            /* 已进入停车距离：必须制动，按超出比例 0.45→1.0 ramp */
            double deficit = clamp((stop_dist - risk_dx) / stop_dist, 0.0, 1.0);
            double brake_floor = 0.45 + deficit * 0.55;
            set_changed(cmd.brake, std::max(cmd.brake, brake_floor));
        } else {
            /* 停车距离够但间隙紧：温和减速（hysteresis 留出的余量） */
            set_changed(cmd.brake, std::max(cmd.brake, 0.30));
        }
        if (ttc < 1.0 || (risk_dx < 6.5 && risk_dy < 1.9)) {
            set_changed(cmd.brake, 1.0);
        }
        if (ttc < 1.5) {
            degrade_set_level(DEGRADE_L2, DEGRADE_REASON_COLLISION);
        }
    } else {
        safety_vehicle_brake_latched = false;
    }
}
```

#### 停车距离模型

$$d_{\text{stop}} = \frac{v^2}{2 a_{\text{brake}}} + \tau v + d_{\text{margin}} = \frac{v^2}{10} + 0.3v + 1.5$$

三项分别是：**物理制动距离**、**反应时间对应的距离**、**固定余量**。以 20 m/s（72 km/h）为例：

$$d_{\text{stop}} = 40 + 6 + 1.5 = 47.5\ \text{m}$$

这个数字告诉我们一件重要的事：**72 km/h 时，从「决定要刹」到「停住」需要 47.5 米。** 任何声称「我在 30 米外看到前车就能避免追尾」的判断都是错的。

#### 三个阈值，三个不同的动作

| 条件 | 动作 | 性质 |
|---|---|---|
| `risk_dx < engage_dist`（未锁定） | 触发制动 | **空间**判据，看的是「够不够刹停」 |
| `ttc < 2.5` | 触发制动 | **时间**判据，看的是「会不会撞上」 |
| `ttc < 1.0` 或 (`dx<6.5` 且 `dy<1.9`) | `brake = 1.0` | **硬 AEB** |
| `ttc < 1.5` | **L2 降级** | 不是刹车，是**系统状态变更** |

**空间与时间双判据的必要性**：低���时 $d_{\text{stop}}$ 很小（v=2 m/s 时只有 2.2 m），`engage_dist` 约 2.7 m，感知要是丢一帧就漏报。所以代码里明写（`:685-686`）：

```cpp
/* TTC 兜底：低速（v<3m/s）时 stop_dist<3m，engage_dist 太小
 * → TTC 2.5s 兜底确保感知丢帧时不漏报 */
```

高速度时反过来，停车距离可能超出 35 m 感知范围，TTC 判据才是主力。两个判据互为补充。

#### 迟滞：防止制动抖动

```cpp
constexpr double kHysteresisGap = 2.0;  /* 释放比触发多这么多 */
const bool in_brake_zone = safety_vehicle_brake_latched
    ? (risk_dx < release_dist)     /* stop_dist + 2.0 */
    : (risk_dx < engage_dist);     /* stop_dist + 0.5 */
```

**触发门槛 0.5 m，释放门槛 2.0 m，中间 1.5 m 是迟滞带。** 如果两者相同，前车停在恰好等于停车距离的位置时，制动力会「踩一下松一下」反复横跳——这既是舒适性灾难，也会让乘客怀疑系统坏了。

#### 制动力的连续 ramp

```cpp
double deficit = clamp((stop_dist - risk_dx) / stop_dist, 0.0, 1.0);
double brake_floor = 0.45 + deficit * 0.55;    /* 0.45 → 1.00 */
```

**`set_changed(cmd.brake, std::max(cmd.brake, brake_floor))`** ——注意是 `max`，安全层只会**加强**刹车，不会削弱上游的刹车指令。制动力从 0.45（刚进入停车距离）线性升到 1.0（已在停车距离内或更近）。

这一条 ramp 设计的物理意义：**制动力应该正比于「刹不住的紧迫程度」**。一上来就全力刹车是把 20 m 的可用距离在 2 秒内烧掉，乘客和后车都受不了；按 deficit 线性给，既能在真正紧急时用满，又在刚进入危险区时保持平顺。

> [!NOTE]
> `safety_vehicle_brake_latched` 是一个**函数内的 `static` 变量**（`:661`），写在 `const` 成员函数里。这意味着它是**跨实例的全局状态**，不随节点实例重建而清零，也不做线程保护——它完全依赖「单协程执行」这个前提。

### 4.4 对向来车：闭合成速度

```cpp
const double closing = state.speed + std::fabs(along_v);
const double clearance = along - 4.0;
const double ttc = clearance / std::max(0.1, closing);
```

对向车的接近速度是**速度之和**而不是差。响应阶梯也不同（`:747-755`）：

```cpp
if (oncoming_ttc < 4.0) {
    set_changed(cmd.throttle, 0.0);
    double brake_floor = clamp((4.0 - oncoming_ttc) / 4.0, 0.5, 1.0);
    if (oncoming_dx < 15.0) brake_floor = std::max(brake_floor, 0.85);
    set_changed(cmd.brake, std::max(cmd.brake, brake_floor));
    if (oncoming_ttc < 1.5 || oncoming_dx < 8.0) {
        set_changed(cmd.brake, 1.0);  /* 紧急制动 */
    }
}
```

**触发阈值 4.0 秒，比同向的 2.5 秒宽得多。** 物理原因很直接：两车以相对速度 $v_1 + v_2$ 接近，制动距离是 $(v_1+v_2)^2$ 量级——20 m/s 对 15 m/s 时相对 35 m/s，停车距离超过 120 米。如果等到 2.5 秒才反应，根本来不及。

### 4.5 横向穿越守卫

这是「别让隔壁车吓出急刹」那一节的**真实实现**（`safety_control_node.cpp:715-740`）：

```cpp
double cross_dx = 0.0, cross_dy_signed = 0.0;
double cross_risk = nearest_vehicle_lateral_cross_risk(state, &cross_dx, &cross_dy_signed);
const bool crossing_intent = std::fabs(cmd.steer) > 0.08 &&
                             cmd.mode.find("ROAD_GUARD") == std::string::npos;
if (crossing_intent && cross_risk < 9.0 && state.speed > 7.0) {
    set_changed(cmd.throttle, 0.0);
    set_changed(cmd.brake, std::max(cmd.brake, 0.65));
    double steer_guard = 0.06;
    const double cross_dy = std::fabs(cross_dy_signed);
    if (std::fabs(cross_dx) < 5.0 && cross_dy < 1.9) {
        set_changed(cmd.brake, 1.0);
        steer_guard = 0.03;
    }

    /* 转向安全约束：只在风险车仍在前方时限制转向方向
     * （防止变道过半后回正方向被错误覆盖——此时风险车已到侧后方，
     * 自然的回正转向看似"朝向风险车"但实为正确的变道收尾动作）。 */
    if (cross_dx > 0.0) {
        if (cross_dy_signed < 0.0) cmd.steer = std::max(cmd.steer, steer_guard);
        else                      cmd.steer = std::min(cmd.steer, -steer_guard);
    }
}
```

> [!IMPORTANT]
> **旧稿这里写的是一个不存在的函数。** 旧版声称安全层用「根据前轮转角计算圆弧轨迹半径 $R = L/\tan\delta$」来构造走廊，宽度是 $1.8 + 0.1v$。**仓库里没有 `is_obstacle_in_collision_corridor`，没有 `compute_radial_distance_to_arc`，也没有任何弧线几何计算。**
> 真实的 `nearest_vehicle_lateral_cross_risk`（`:366-393`）是一个朴素的**车体系矩形门限**：`along ∈ [−5, 12] m`、`|dy| ≤ 2.2` m，风险度量是 $|along| + 2|dy| < 9.0$。

不过**设计意图和旧稿描述的方向是一致的**，而且真实现有几个更细的地方：

1. **`crossing_intent` 由转向意图触发**，不是「只要旁边有车就报警」。`fabs(cmd.steer) > 0.08` 意味着车确实在往旁边打；
2. **豁免 `ROAD_GUARD`**——`mode.find("ROAD_GUARD") != npos` 时不触发。因为 ROAD_GUARD 是在紧急回正，它的转向必然很大，此时压转向会把车锁死在错误方向；
3. **只压「朝风险车方向」的转向**（`cross_dx > 0.0` 才生效），且在 `-0.06` / `-0.03` 附近限幅。这条约束只阻止「继续朝车撞」，不阻止「离开」；
4. **变道过半后豁免**——`cross_dx` 变负（风险车到侧后方）后不再限制转向。注释解释得很清楚：此时自然的回正转向「看似朝向风险车，实为正确的变道收尾动作」。**如果这里不做这个豁免，每次变道都必然被安全层误判。**

### 4.6 机动硬碰撞：为什么必须单独写一条

巡航段的守卫（第 4.2~4.5 节）全部被 `if (has_state && !maneuver)` 挡在外面。机动模式走的是另一套完全不同的逻辑（`safety_control_node.cpp:612-640`）：

```cpp
if (has_state && maneuver) {
    /* 硬碰撞保护：沿运动方向 2m 内有障碍才全刹，其余放行。
     * 不复用 min_vehicle_ttc——它无候选时返回 dx=0，会被误判
     * "0m 处有障碍" → 恒全刹（2026-08-03 掉头两次死于此）。 */
    const bool backing = (cmd.gear == GEAR_REVERSE);
    const double ch = std::cos(state.heading), sh = std::sin(state.heading);
    for (int i = 0; i < kMaxObs; ++i) {
        if (!state.obs_valid[i]) continue;
        /* 施工区是 planning 生成机动轨迹的边界约束，不是动态碰撞体。
         * Phase 0 的语义正是从施工前缘倒车腾挪；若把墙体感知点纳入
         * 倒车硬门，墙在车后 4.8m 内时会永久 brake=1，机动直到 40s
         * TIMEOUT。真实车辆/行人仍保留双向硬碰撞保护。 */
        if (std::strcmp(state.obs_type[i], "construction") == 0) continue;
        const double dx = state.obs_x[i] - state.x;
        const double dy = state.obs_y[i] - state.y;
        /* 车体系投影：掉头转过 90°/180° 后世界系 +x 早已不是"前方" */
        const double lon = dx * ch + dy * sh;
        const double lat = -dx * sh + dy * ch;
        const double ahead = backing ? -lon : lon;
        /* 1.2m 净距 + 3.6m 偏置（半车长 2.4 + 半障碍 1.2，中心距） */
        if (ahead > 0.0 && ahead < 3.6 + 1.2 && std::fabs(lat) < 1.4) {
            set_changed(cmd.throttle, 0.0);
            set_changed(cmd.brake, 1.0);
            break;
        }
    }
}
```

三条设计，每条都对应一次实车/仿真故障：

**第一：不用 TTC，用纯空间门限。** 注释写得很直接——`min_vehicle_ttc` 在没有候选障碍物时返回 `1e9`，但 `out_dx` 保持 `0.0`。而掉头轨迹的 `apply_safety` 里 `risk_dx` 会被当作「0 m 处有障碍」来用，直接导致**恒定全刹**。2026-08-03 掉头测试两次死在这。

**第二：方向随挡位翻转。** `ahead = backing ? -lon : lon`——**倒车时只看车后**。理由是「倒车逃离前方障碍是 Phase 0 腾挪的合法动作，不得拦截」。这个逻辑很重要：施工区腾挪的第一步就是往后倒，如果前方有施工墙体还必须往前顶，机动根本无法起步。

**第三：施工区感知点被豁免。**

```cpp
if (std::strcmp(state.obs_type[i], "construction") == 0) continue;
```

施工区在感知里是**墙**，但对规划器来说它是**生成机动轨迹的边界约束**，不是需要躲避的动态障碍。施工区腾挪的语义恰恰是「从施工前缘倒车出去」——如果把墙纳入倒车硬门，车后 4.8 m 内有墙就永久 `brake = 1`，机动会一直卡到 40 s TIMEOUT。

**这条豁免是个精确但脆弱的划界**：它依赖 `obs_type` 字符串精确等于 `"construction"`。感知侧一旦改了这个类型名（例如改成 `"construction_zone"` 或 `"barrier"`），豁免会静默失效，掉头立刻回到 40 s 超时。而且没有任何测试覆盖它——`apply_safety` 无法单测（第 8.2 节）。

判据本身也很朴素：`ahead ∈ (0, 4.8)` m 且 `|lat| < 1.4` m 就是一次全刹。4.8 = 半车长 2.4 + 半障碍 1.2 + 1.2 净距。

### 4.7 同车道跟车距

```cpp
double gap = nearest_same_lane_gap(state, params_);
double safe_gap = params_.min_gap + state.speed * params_.time_headway;
if (gap < safe_gap && gap < 80.0) {
    double ratio = clamp(gap / safe_gap, 0.0, 1.0);
    double limited_throttle = cmd.throttle * ratio;
    set_changed(cmd.throttle, std::min(cmd.throttle, limited_throttle));
    if (ratio < params_.hard_brake_ratio) {
        set_changed(cmd.brake, std::max(cmd.brake, 1.0 - ratio));
    }
}
```

$$d_{\text{safe}} = d_{\min} + \tau v = 6.0 + 1.8 v$$

这正是第 15 章讲的 **CTG（常量时距）跟车律**，只是少了 `acc_standoff` 项。油门按 `ratio` 线性缩放，低于 `hard_brake_ratio = 0.45` 时开始加刹车，刹车量是 `1 − ratio`。

> [!NOTE]
> **可配置性有个坑**：`SafetyParams` 有 9 个字段，但 `safety_control_node.cpp:906-913` 只解析其中 4 个（`max_throttle`、`max_steer`、`low_speed_steer`、`time_headway`）。`min_gap = 6.0`、`same_lane_tol = 2.0`、`max_brake = 1.0`、`hard_brake_ratio = 0.45` **是硬编码的**。
> 而 `config/pipeline.json:317` 把 `max_throttle` 覆盖成 `1.0`——所以**生产环境里 `max_throttle` 实际上是 1.0，不是默认的 0.85**。这意味着规则控制或模型可以请求满油门，安全层不拦。

---

## 5. 降级阶梯：L0 到 L3

### 5.1 两套心跳，互不相干

这是最容易搞混的地方。**仓库里有两个独立的心跳系统**：

| | `health.c` | `degrade_ladder.c` |
|---|---|---|
| 追踪对象 | `safety_control` 自己 | `planning_node` / `control_node` / `fusion_node` |
| 超时阈值 | **5 s** → `HEALTH_STALE` | 500 ms / 2000 ms |
| 超时后果 | **仅上报**给 monitor 和 HTTP API，**不触发任何动作** | **驱动 L0→L3 递进** |
| 注册接口 | `health_heartbeat(name)`（`health.h:80`） | `degrade_supervisor_record_heartbeat(name, ms)` |

```cpp
/* src/core/health.c:179-190 */
if (e->error_count > 0 && now - e->last_error_time_us < 5000000ULL) {
    s->status = HEALTH_ERROR;
} else if (e->last_heartbeat_us == 0 ||
           now - e->last_heartbeat_us > 5000000ULL) {
    s->status = HEALTH_STALE;
} else if (s->avg_latency_us > 50000ULL) {
    s->status = HEALTH_DEGRADED;
} else {
    s->status = HEALTH_OK;
}
```

**`HEALTH_STALE` 不会让车做任何事。** 它只是一个观测信号。真正会动车的只有 `degrade_ladder`。

> [!TIP]
> `health_record_latency` 在 `safety_control` 里从未被调用，所以 `avg_latency_us` 恒为 0，**`HEALTH_DEGRADED` 对这个节点不可达**。同理 `HEALTH_CAP_SAFETY_CRITICAL` 标志被设置了，但没有任何逻辑读它。

### 5.2 粘滞：只能升不能降

`degrade_set_level_at`（`src/core/degrade_ladder.c:85-108`）：

```c
if (level < DEGRADE_L0 || level > DEGRADE_L3) return;

/* L0 是全功能状态。事故后不允许任意节点把 L2/L3 覆写回较低等级；
 * 恢复只能走 supervisor 的去抖 degrade_clear()。 */
int current = (int)FLOW_ATOMIC_LOAD(&g_degrade.degrade_level);
if (level < current) return;                          /* ← 拒绝降级 */
if (level == current && level != DEGRADE_L0) return;  /* ← 拒绝同级重复 */

FLOW_ATOMIC_STORE(&g_degrade.degrade_level, level);
FLOW_ATOMIC_STORE(&g_degrade.degrade_reason, reason);
FLOW_ATOMIC_STORE(&g_degrade.degrade_timestamp_ms, now_ms);

if (level >= DEGRADE_L1) FLOW_ATOMIC_STORE(&g_degrade.l1_disable_lane_change, 1);
if (level >= DEGRADE_L2) FLOW_ATOMIC_STORE(&g_degrade.l1_speed_limit, 3.0);  /* 3 m/s crawl */
if (level >= DEGRADE_L3) FLOW_ATOMIC_STORE(&g_degrade.l1_speed_limit, 0.0);  /* 立即停 */
```

**三条规则**：

1. **单调不减**——任何节点都不能把等级调低；
2. **同级不刷新**（L0 除外）——避免时间戳被无意义地刷新，失去诊断价值；
3. **只有 `degrade_clear()` 能降级**。

这是**粘滞锁存（latching）** 设计。理由：安全系统的恢复判据必须**集中**。如果每个节点都能自行降级，那么一个卡了 100 ms 又恢复的 `fusion_node` 就能把系统拉回全功能，而 `control_node` 可能还没恢复——**降级会在节点抖动时反复横跳**。

### 5.3 supervisor 的递进与恢复

`degrade_supervisor_tick`（`src/core/degrade_ladder.c:211-290`）：

```c
int timeout_count = 0, timeout_1s_count = 0;
for (int i = 0; i < g_supervisor.node_count; i++) {
    int64_t hb = g_supervisor.nodes[i].last_heartbeat_ms;
    if (hb == 0) continue;              /* 未注册，不视为超时 */

    int64_t age = now_ms - hb;
    if (age < 0) age = 0;

    if (age > 500) {
        if (g_supervisor.nodes[i].timeout_since_ms == 0)
            g_supervisor.nodes[i].timeout_since_ms = now_ms;
    } else {
        g_supervisor.nodes[i].timeout_since_ms = 0;
    }

    /* 超过阈值后再持续 150ms 才确认，去抖不依赖 supervisor tick 频率。 */
    if (g_supervisor.nodes[i].timeout_since_ms != 0 &&
        now_ms - g_supervisor.nodes[i].timeout_since_ms >= 150) timeout_count++;
    if (age > 2000) timeout_1s_count++;
}
```

两个时间常数：**500 ms 触发 + 150 ms 确认**。这个 150 ms 的去抖窗口有个好性质：**去抖效果与 supervisor 的 tick 频率无关**。无论 tick 是 10 Hz 还是 100 Hz，一个节点都必须连续失联 650 ms 才会被确认超时。

递进表：

| 条件 | 升到 | 原因码 |
|---|---|---|
| 单节点 > 500 ms（且持续 150 ms） | **L1** | `reason_for_node` 按名字判定 |
| ≥2 节点 > 500 ms | **L2** | `DEGRADE_REASON_PLANNING_TO` |
| 单节点 > 2000 ms | **L2** | `CONTROL_TO` / `PLANNING_TO` / `FUSION_TO` |
| ≥2 节点 > 2000 ms | **L3** | `DEGRADE_REASON_PLANNING_TO` |

恢复逻辑（`:238-253`）：

```c
/* 自动恢复：全部心跳健康持续 3s → 清降级。
 * 没有这条，supervisor 只升不降——一次瞬时抖动（调度延迟/负载尖峰）
 * 就把系统钉死在 L2/L3 直到重启。恢复必须滞后（3s 去抖）防振荡。 */
static int64_t healthy_since_ms = 0;
if (timeout_count == 0 && timeout_1s_count == 0) {
    if (current > DEGRADE_L0) {
        if (healthy_since_ms == 0) healthy_since_ms = now_ms;
        else if (now_ms - healthy_since_ms > 3000) {
            degrade_clear();
            healthy_since_ms = 0;
            return;
        }
    }
} else {
    healthy_since_ms = 0;
}
```

**「只升不降」必须配一条「持续健康 3 秒才降」的恢复路径**，否则一次调度延迟就足以把车钉死在 L3 直到重启。3 秒这个值和升级的 150 ms 去抖形成鲜明对比——**降级要快，恢复要慢**，这是所有安全监控系统的通用原则。

`degrade_clear()` 顺带清掉所有心跳记录（`:122-124`）：

```c
for (int i = 0; i < g_supervisor.node_count; i++)
    g_supervisor.nodes[i].last_heartbeat_ms = 0;
```

而 supervisor 跳过 `hb == 0` 的节点（`if (hb == 0) continue;`），所以**清除是「免费」的**——清完到下一次心跳之间不会误判超时。

### 5.4 各等级的实际行为

`degrade_layer_action()`（`src/core/degrade_ladder.c:131-175`）把全局状态翻译成一个动作结构体：

```c
switch (level) {
case DEGRADE_L0: break;                                  /* 全功能 */

case DEGRADE_L1:
    act.disable_lane_change = (disable_lc != 0);
    act.speed_limit = speed_limit;                        /* 恒为 0.0 = 不限速 */
    act.safety_margin = (safety_margin > 1.0) ? safety_margin : 1.5;
    break;

case DEGRADE_L2:
    act.disable_lane_change = true;
    act.mrm_stop = true;
    act.speed_limit = (speed_limit > 0.0) ? speed_limit : 3.0;
    act.safety_margin = 2.0;
    break;

case DEGRADE_L3:
    act.disable_lane_change = true;
    act.immediate_stop = true;
    act.mrm_stop  = true;
    act.speed_limit = 0.0;
    act.safety_margin = 3.0;
    break;
}
```

各等级的**真实效果**（这张表是本章最需要记住的）：

| 等级 | safety_control 的动作 | control_node 的动作 |
|---|---|---|
| **L0** | 无 | 正常 |
| **L1** | **什么都不做**（`immediate_stop` 和 `mrm_stop` 都是 false） | 无速度上限 |
| **L2** | `throttle=0`、`brake ≥ 0.70`、`hazard=true` | `acc_target` 压到 3.0 m/s 爬行；模式标 `MRM`；停稳 3 s 自动恢复 |
| **L3** | `throttle=0`、`brake=1.0`、`steer=0`、`hazard=true` | `target_speed=0`、清积分；停稳 3 s 自动恢复 |

> [!WARNING]
> **L1 名不副实。** 它的头文件注释写着「降级：禁变道、限速、加大安全余量」（`degrade_ladder.h:39`），但实际上：
> - **不限速**——`l1_speed_limit` 从来没在 L1 被赋值（`degrade_ladder.c:99-107` 只在 L2 写 3.0、L3 写 0.0），所以 `act.speed_limit` 恒为 `0.0`，而 0.0 的含义是「不限」；
> - **不真的禁变道**——`DegradeAction.disable_lane_change` 被 set 了，但**全仓库没有任何控制逻辑读它**，只被序列化进 `safety/evidence` 的 JSON；
> - **安全余量没被用**——`act.safety_margin`（1.5/2.0/3.0）同样从不被任何控制逻辑读取。
>
> **L1 在整车层面唯一的实际效果，是 `is_degraded` 变真，从而让仲裁器在 P1 屏蔽掉学习模型。** 仅此而已。
>
> 头文件里的示例注释 `degrade_layer_action("control_node")` 也是错的——真实签名**不接收任何参数**（`degrade_ladder.h:114`）。

### 5.5 降级在安全节点的出口执行

阶梯的权威是 `degrade_ladder`，但**执行点在安全节点的出口**（`safety_control_node.cpp:789-802`）：

```cpp
/* degrade_ladder 是全局安全策略的唯一权威。先由本层完成碰撞/TTC
 * 限幅，再在出口处执行 L2/L3，保证上游恢复出新 raw_cmd 时不会绕过
 * 已锁存的最小风险动作。 */
const DegradeAction degrade_action = degrade_layer_action();
if (degrade_action.immediate_stop) {
    set_changed(cmd.throttle, 0.0);
    set_changed(cmd.brake, 1.0);
    set_changed(cmd.steer, 0.0);
    cmd.hazard = true;
} else if (degrade_action.mrm_stop) {
    set_changed(cmd.throttle, 0.0);
    set_changed(cmd.brake, std::max(cmd.brake, 0.70));
    cmd.hazard = true;
}
```

**为什么放在最后**：因为上游可能刚刚恢复（控制节点重新发来正常的 `raw_cmd`）。如果在入口就执行降级，上游一条正常指令就能覆盖掉锁存的 L3 状态。**放在出口意味着降级是不可绕过的最后一道**——注释里那句「保证上游恢复出新 raw_cmd 时不会绕过已锁存的最小风险动作」说的就是这个。

> [!NOTE]
> L3 同时设置了 `immediate_stop` 和 `mrm_stop`（`degrade_ladder.c:167-168`），所以那个 `else if (mrm_stop)` 分支对 L3 是不可达的。

---

## 6. 数据超时看门狗：闸门自己也得有 Plan B

### 6.1 真实判据

```cpp
/* src/core/safety_fault_injection.c:29-35 */
bool safety_raw_command_timeout_expired(uint64_t now_us,
                                        uint64_t last_raw_command_us,
                                        bool vehicle_moving,
                                        uint64_t timeout_us) {
    return vehicle_moving && last_raw_command_us > 0 && now_us >= last_raw_command_us &&
           now_us - last_raw_command_us > timeout_us;
}
```

四个条件同时成立才算超时，其中最关键的是 **`vehicle_moving`**：

```cpp
double cur_speed = 0.0;
bool has_state = false;
pthread_mutex_lock(&g.state_mutex);
cur_speed = g.latest_state.speed;
has_state = g.has_state;
pthread_mutex_unlock(&g.state_mutex);
bool moving = has_state && cur_speed > 0.5;
```

```cpp
/* 车已停稳（speed<=0.5）时 raw_cmd 停发属正常行为：
 * 例如红灯前刹停后 control 不再高频发 cmd。此时心跳缺失
 * 不应判 L3，否则与 degrade_ladder 的自动恢复形成 MRM 拉锯
 * （车停稳却反复 降级→恢复→降级）。仅当车仍在运动而 2s 无
 * cmd 时才视为真实失联 → L3。
 *
 * 超时从 1s 提升到 2s（2026-08-03）：与 flowsim 的
 * CONTROL_STALE_TIMEOUT_US(2s) 保持一致。高负载下消息总线
 * 丢包率高，1s 超时导致 MRM 拉锯循环：
 *   车停→L3清除→起步加速→speed>0.5→raw_cmd 1s stale→L3
 *   →刹车→停稳→3s 恢复→起步→... 无限循环车速恒为 0 */
```

**这是一段极有价值的注释**，它记录了一个真实的系统级振荡：

```
车停 → L3 清除 → 起步加速 → speed > 0.5 → raw_cmd 超时 → L3
    → 刹车 → 停稳 → 3 s 后恢复 → 起步 → …… 无限循环，车速恒为 0
```

**这个循环的成因是「降级 → 刹车 → 停稳 → 降级被清除 → 起步 → 超时 → 降级」构成的正反馈环**。两个修复动作：

1. **`vehicle_moving` 门限**——停稳时不判超时，直接掐断环路的一环；
2. **阈值 1 s → 2 s**——高负载下总线丢包率本来就高，1 s 太紧。

**1 s 提升到 2 s 时并没有写代码注释里的那句「与 flowsim 保持一致」**——两个模块的超时必须一致，否则被控对象和安全监控会互相打架。这是跨模块契约，值得在思考题里展开。

### 6.2 超时后：自己下发制动

```cpp
if (safety_raw_command_timeout_expired(now_us, last_msg_us, moving, 2000000ULL) &&
    !timeout_action_sent) {
    /* 行驶中数据超时 > 2s → L3，并由安全闸门主动下发制动，
     * 不能只等待一个可能永远不会再来的 raw_cmd。 */
    degrade_set_level_at(DEGRADE_L3, DEGRADE_REASON_HEARTBEAT, (int64_t)(now_us / 1000));
    health_record_error("safety_control", "raw command timeout");
    ControlCmd emergency_stop;
    emergency_stop.brake = 1.0;
    emergency_stop.hazard = true;
    emergency_stop.mode = "DATA_TIMEOUT+SAFE";
    publish_cmd(emergency_stop, true);
    publish_fault_evidence(fault_injection.activated, injected_at_us, now_us,
                           now_us - last_msg_us);
    timeout_action_sent = true;
    LOG_ERROR("safety_control",
              "raw_cmd timeout %.0fms while moving: L3 emergency stop published",
              (double)(now_us - last_msg_us) / 1000.0);
}
```

**关键设计**：闸门不是简单地「发现上游死了就什么都不做」，而是**主动构造一条 `control/cmd` 制动指令发出去**。理由写在注释里——「不能只等待一个可能永远不会再来的 raw_cmd」。

`timeout_action_sent` 保证这是一次性动作（闩锁），不会每个 5 ms 节拍都发一次。

### 6.3 一个刻意的位置决定

```cpp
/* 每个轮询节拍都检查数据超时。故障注入期间 raw_cmd 仍持续抵达
 * 但被故意丢弃，不能把检测藏在"队列为空"的分支里。 */
```

这段注释解释了一个真实的测试失败：**如果把看门狗逻辑写在「队列为空」的 `else` 分支里，故障注入测试就会失效**——因为故障注入模式下 `raw_cmd` 仍在持续抵达（只是被故意丢弃），队列永远不会空，看门狗永远不跑，测试通过但功能是坏的。

**看门狗必须独立于它所监控的数据流。** 这是一条通用的可靠性原则。

---

## 7. 证据链：安全事件的可追溯性

安全节点发布的第二路输出是 `safety/evidence`——一个 JSON 文档，描述「为什么做了这个动作」。Schema 由 `src/core/safety_evidence.c:5-56` 的 `safety_evidence_to_json` 生成：

```
schema_version: 1
evidence_type: "safety_fault"
fault:       { 故障注入信息, injected_at_us, detected_after_us }
degrade:     { level, reason }
action:      { name, immediate_stop, mrm_stop, disable_lane_change,
               speed_limit_mps, safety_margin }
action.command: { throttle, brake, steer }
```

消费者是 `monitor_node.c:2224`。**这是把「安全层的决策」变成可审计记录的设计**——事故复盘时能拿到的不只是「车刹停了」，还有「当时 TTC 是多少、触发了哪一级降级、动作为什么这么选」。

`action` 里同时带 `disable_lane_change` 和 `safety_margin` 这两个**从未被任何控制逻辑读取的字段**——它们只在证据链里躺着。这其实是个有意思的状态：设计者**预留**了这两个字段，准备给下游用，但下游还没接。

---

## 8. 测试与可测性

### 8.1 已覆盖的部分

**`tests/test_adas_nodes_logic.c:445-502`** —— 5 个仲裁测试（ctest 名 `adas_nodes_logic_tests`）：

| 测试 | 断言 |
|---|---|
| `test_arbiter_no_model` | `has_fresh_model=0` ⇒ 规则透传，`intervened=0` |
| `test_arbiter_degraded_fallback` | `is_degraded=1` ⇒ 规则透传，`intervened=0` |
| `test_arbiter_model_within_envelope` | `Δsteer=0.05` ⇒ 模型接受，`intervened=0` |
| `test_arbiter_steer_reject` | `Δsteer=0.18` ⇒ 规则转向，`intervened=1` |
| `test_arbiter_brake_priority` | `rule.brake=0.7` ⇒ brake 0.7、throttle 0、`intervened=1` |

CMake（`CMakeLists.txt:1107-1124`）编译的是**和生产完全相同的** `modules/adas_nodes/safety_arbiter.c`（`:1113`），所以这不是一个复制品测试。

**`tests/test_safety_fault_evidence.c`** —— 降级阶梯与超时（ctest `safety_fault_evidence_tests`）：

- `test_missed_heartbeat_degrades`：心跳 @1000 ms，tick @1501 ms 仍 L0（去抖生效），tick @1652 ms 升 **L1**，原因 `CONTROL_TO`，时间戳 1652，`safety_margin ≥ 1.5`；
- `test_raw_command_timeout_evidence`：3 500 001 µs 超时（2 s 阈值），**停稳时不触发**（`:72`）；`degrade_set_level_at(L3, HEARTBEAT, 3500)` ⇒ `immediate_stop && mrm_stop && speed_limit == 0.0`。

### 8.2 完全没覆盖的部分

这是本章最需要读者注意的**测试空白**：

| 未测对象 | 所在 | 为什么测不了 |
|---|---|---|
| `min_vehicle_ttc` | `:285` | 匿名命名空间里的 static 函数 |
| `min_oncoming_ttc` | `:334` | 同上 |
| `nearest_same_lane_gap` | `:221` | 同上 |
| `pedestrian_collision_gap` / `pedestrian_crossing_hold_gap` | — | 同上 |
| `nearest_vehicle_lateral_cross_risk` | `:366` | 同上 |
| **`apply_safety` 整个函数** | `:576-808` | 同上 |
| `clamp` 的 NaN 分支 | `:102` | 同上 |
| **`publish_cmd` 的 NaN 紧急兜底** | `:819` | 同上 |
| **第 2.4 节的仲裁绕过 bug** | `:504` | 需要跑整个节点循环 |

**本章讨论的绝大多数物理逻辑（TTC 公式、制动 ramp、迟滞、穿越守卫、NaN 兜底）一行测试都没有。**

这不是疏忽，是架构的直接后果：这些函数全部塞在一个 1586+ 行的 `safety_control_node.cpp` 的匿名命名空间里，**无法从外部链接，因此无法单测**。第 17 章的控制节点有同样的问题，但它的 PID、横向 PD 至少还能通过 `tests/test_adas_nodes_logic.c` 间接覆盖一部分。

---

## 9. 死代码与配置现实

### 9.1 死代码清单

| 符号 | 位置 | 状态 |
|---|---|---|
| `degrade_supervisor_summary()` | `degrade_ladder.h:147`、`.c:294` | **全仓库零调用者** |
| `DegradeAction.disable_lane_change` | `degrade_ladder.c:151,158,166` | 从不被任何控制逻辑读，只序列化进 JSON |
| `DegradeAction.safety_margin` | `degrade_ladder.c:142,153,161,170` | 同上 |
| `l1_safety_margin` | `degrade_ladder.h:69` | 只在 init/clear 写 1.0，**从未被设成 1.5/2.0/3.0** |
| `VEHICLE_STATE_TYPE_ID` | `safety_control_node.cpp:40` | 声明未用 |
| `control/cmd/text` | 发布于 `:850` | 不在 `s_outputs[]`，**无任何流水线 JSON 或节点订阅** |
| `health_record_latency`（safety 侧） | `health.h:83` | 从不调用 ⇒ `HEALTH_DEGRADED` 不可达 |
| `HEALTH_CAP_SAFETY_CRITICAL` | `health.h:39` | 被设置，无逻辑读它 |
| `CONTROL_RAW_TYPE_ID` 值 | `safety_control_node.cpp:38` | 硬编码过期值 `0x871712d1` |
| `CONTROL_CMD_TYPE_ID` 值 | `safety_control_node.cpp:39` | 硬编码过期值 `0x2D95C6D2`（实际 `0xed9c7088`） |

`l1_safety_margin` 这一条尤其能说明问题：头文件里定义了它，`degrade_layer_action` 里三档都返回了 `safety_margin` 的值，但**没有任何地方把 `l1_safety_margin` 设成非 1.0**，所以 `safety_margin > 1.0 ? safety_margin : 1.5` 这个三元表达式**永远走 1.5 分支**——L2 的 2.0 和 L3 的 3.0 只在 L2/L3 的 `case` 里硬编码返回，跟这个字段无关。

### 9.2 硬编码的物理常数

安全路径上**只有 4 个参数可配**（`:906-913`），其余全是源码里的字面量：

| 值 | 含义 | 位置 |
|---|---|---|
| `4.6` / `2.0` | 跟车距偏移 / 同车道横向容差 | `:235` / `:79` |
| `4.8` / `4.0` | 同向 / 对向 TTC 净空 | `:306` / `:355` |
| `2.3` | 同车道横向门限 | `:300`、`:333` |
| `35.0` / `60.0` | 同向 / 对向感知范围 | `:300` / `:347` |
| `0.4` / `−2.0` | 最小接近速度 / 对向速度门限 | `:304` / `:352` |
| `5.0` / `0.3` / `1.5` | 制动减速度 / 反应时间 / 余量 | `:668-670` |
| `2.0` / `0.5` | 迟滞带 / 触发余量 | `:671` / `:674` |
| `2.5` / `1.5` / `1.0` | TTC 触发 / 降级 / 硬刹 | `:687` / `:705` / `:701` |
| `0.45` / `0.55` / `0.30` | 制动 ramp 地板/斜率/温和减速 | `:695` / `:699` |
| `6.5` / `1.9` | 硬 AEB 的空间门限 | `:701` |
| `9.0` / `0.08` / `7.0` | 穿越守卫风险/转向意图/速度 | `:719-720` |
| `0.06` / `0.03` | 穿越守卫转向限幅 | `:722-727` |
| `4.0` / `0.85` / `15.0` / `8.0` | 对向制动阶梯 | `:747-754` |
| `2000000ULL` | raw_cmd 超时 | `:550` |
| `500` / `150` / `2000` / `3000` ms | supervisor 触发/去抖/严重/恢复 | `degrade_ladder.c:222-245` |

`degrade_ladder` 里的 `DEGRADE_MAX_NODES 16` 和 `DEGRADE_NODE_NAME_LEN 32` 同样不可配——**节点名表满 16 个后新注册会静默失败**（`:196` 直接 `return`），名字被截断到 31 字符。

**这些常数全部硬编码，意味着换一辆车（轴距、质量、制动能力不同）就要重新改源码。** 对一个想上车的系统来说，这是相当硬的约束。

---

## 10. 源码与资源对照

| 模块 / 层次 | 源码文件路径 | 核心符号 / API | 架构职责与设计要点 |
|---|---|---|---|
| **安全闸门节点** | [`modules/adas_nodes/safety_control_node.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/safety_control_node.cpp) | `SafetyControlTask::run`（`:448`）<br>`apply_safety`（`:576-808`）<br>`publish_cmd`（`:810`）<br>`clamp`（`:102`） | 5 ms 节拍；三路输入两套传输；包络钳位 → 碰撞守卫 → 降级执行，**降级在出口不可绕过** |
| **规则/模型仲裁** | [`modules/adas_nodes/safety_arbiter.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/safety_arbiter.h)<br>[`modules/adas_nodes/safety_arbiter.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/safety_arbiter.c) | `safety_arbiter_apply`（`.c:13`）<br>`SAFETY_ARBITER_STEER_ENVELOPE_RAD`（0.12）<br>`SAFETY_ARBITER_RULE_BRAKE_ACTIVE`（0.10） | 降级 > 转向包络 > 规则制动 > 模型油门上限；制动取 max；P1 不计入干预 |
| **降级阶梯** | [`include/degrade_ladder.h`](file:///home/caixuf/code/FlowEngine/include/degrade_ladder.h)<br>[`src/core/degrade_ladder.c`](file:///home/caixuf/code/FlowEngine/src/core/degrade_ladder.c) | `degrade_set_level_at`（`.c:85`）<br>`degrade_layer_action`（`.c:131`）<br>`degrade_supervisor_tick`（`.c:211`）<br>`degrade_clear`（`.c:114`） | 粘滞锁存（单调不减）；500/150/2000/3000 ms 四时间常数；只有 `degrade_clear` 能降级 |
| **健康监控** | [`include/health.h`](file:///home/caixuf/code/FlowEngine/include/health.h)<br>[`src/core/health.c`](file:///home/caixuf/code/FlowEngine/src/core/health.c) | `health_heartbeat`（`.c:100`）<br>状态派生（`.c:179-190`） | 5 s `HEALTH_STALE`；**仅上报不动作**，与 degrade_ladder 无关 |
| **看门狗判据** | [`include/safety_fault_injection.h`](file:///home/caixuf/code/FlowEngine/include/safety_fault_injection.h)<br>[`src/core/safety_fault_injection.c`](file:///home/caixuf/code/FlowEngine/src/core/safety_fault_injection.c) | `safety_raw_command_timeout_expired`（`.c:29`） | `moving && last>0 && Δt>2s`；`moving` 门限掐断 MRM 拉锯环 |
| **证据链** | [`src/core/safety_evidence.c`](file:///home/caixuf/code/FlowEngine/src/core/safety_evidence.c) | `safety_evidence_to_json`（`:5-56`） | `safety/evidence` JSON：故障 + 降级 + 动作 + 动作后的指令；monitor 消费 |
| **消息定义** | [`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) | `ControlRaw`（`:189-202`，12 字段/59 B）<br>**`ControlCmd`（`:204-213`，8 字段/20 B）** | 输出剥掉全部监控字段，只留执行所需；新增 `emergency_stop` |
| **管道配置** | [`config/pipeline.json`](file:///home/caixuf/code/FlowEngine/config/pipeline.json) | safety_control 段（`:293-318`）<br>flowsim 订阅（`:19-21`） | `control/cmd` 唯一消费者是 flowsim；**手动模式无此节点，闸门被绕过** |
| **测试** | [`tests/test_adas_nodes_logic.c`](file:///home/caixuf/code/FlowEngine/tests/test_adas_nodes_logic.c)<br>[`tests/test_safety_fault_evidence.c`](file:///home/caixuf/code/FlowEngine/tests/test_safety_fault_evidence.c) | ctest `adas_nodes_logic_tests`（5 仲裁用例）<br>ctest `safety_fault_evidence_tests`（降级 + 超时） | 编译**生产同一份** `safety_arbiter.c`；**TTC / apply_safety / NaN 兜底全部无测试** |

---

## 11. 思考题

1. **仲裁绕过漏洞的修复**：
   第 2.4 节发现：模型指令在规则指令超过 100 ms 未到达时，会走 `: base_cmd` 分支**完全绕过 `safety_arbiter_apply`**，使 P1/P2/P3 三层保护全部失效。这个窗口恰好在系统最需要保护时被打开。
   请设计修复方案，并回答：
   (a) 最小改动是把 `: base_cmd` 改成也走 `arbitrate_control()`，但此时 `last_rule_cmd` 可能是陈旧的（几秒前的）指令——用陈旧规则指令去否决新鲜模型指令，合适吗？还是应该直接判定为「规则失联」并抑制模型？
   (b) 如果选择抑制模型，那么 `last_rule_cmd_us` 超时后应该走哪条路径？沿用 `degrade_set_level(DEGRADE_L2, …)` 是不是过度反应（一次 100 ms 的控制节点抖动就降级）？
   (c) 这个 bug 为什么能活到现在？现有 5 个仲裁测试都是直接调 `safety_arbiter_apply`，**没有一个覆盖调用点**。请设计一个能捕获这类「被测函数正确但调用方式错误」的测试——mock 消息总线，让 `inference/raw_cmd` 在 `control/raw_cmd` 静默 150 ms 后到达，断言最终 `control/cmd` 的油门不超过 0.85。

2. **把物理常数外置**：
   第 9.2 节列出 30+ 个硬编码的物理常数（`4.8` 净空、`5.0` 制动减速度、`2.3` 同车道容差、`kBrakeTau` 反应时间……）。它们全部写死在 `.cpp` 里，换车就要改源码。
   (a) 请设计一个车辆参数结构（可以复用第 17 章的 `LtvMpcConfig` 或 FlowSim 的 `Entity` 模式），列出应该外置的完整字段集，并说明每个字段的**物理来源**（是整车标定、法规要求、还是经验值？）。
   (b) 这些参数是否应该热重载？注意 `SafetyParams` 现在只解析 9 个字段中的 4 个，而 `min_gap`/`same_lane_tol`/`hard_brake_ratio` 是硬编码的。如果开放热重载，一个误操作会不会把 `max_brake` 改成 0.0 从而让整个安全层失效？
   (c) 哪些参数**必须**在构建期固定（编译进二进制）以防止运行时被篡改？安全关键参数和性能参数的策略应该不同——请论证分界线在哪里。

3. **给安全逻辑造一条测试替身**：
   第 8.2 节指出，TTC 公式、制动 ramp、迟滞、穿越守卫、NaN 兜底**全部无法单测**，因为它们在 `.cpp` 的匿名命名空间里。
   (a) 请设计一个提取方案：哪些函数应该被抽到独立的头文件（像 `safety_arbiter.c` 那样）？抽取时如何处理它对 `VehicleState` 的依赖——把这个结构体也移出去，还是传原始数组？
   (b) 抽出之后，为 `min_vehicle_ttc` 设计**至少 8 个边界用例**：包括 `closing` 恰好等于 0.4、`along` 恰好等于 35、`lat` 恰好等于 2.3、`along` 小于 4.8（净空为负 → TTC 为负）、以及 2026-08-04 那个掉头返程回归（ego 朝西、障碍物在世界 −x 方向）。每个用例说明它锁住的是什么性质。
   (c) 更激进的问题：既然 `apply_safety` 是唯一的编排入口，是否应该把整个函数改造成**纯函数**——输入 `(ControlCmd in, VehicleState state, DegradeAction action, SafetyParams params)`，输出 `(ControlCmd out, bool changed)`？这样它就可以被穷举测试。请评估这个重构的代价，特别是 `safety_vehicle_brake_latched` 这个跨调用 `static` 状态该如何显式化。

4. **L1 该不该做点什么**：
   第 5.4 节揭示：L1 名义上要「禁变道、限速、加大安全余量」，实际上一件都没做，全系统唯一效果是屏蔽学习模型。
   (a) 假设你要让 L1 名副其实。请分析：在**当前架构**下，「禁变道」应该由谁执行？`control_node` 有 `maneuver_mode` 和 `BEH_LEFT_CHANGE` 状态的转移逻辑（第 15 章），`planning_node` 会生成变道轨迹。谁最适合拦这道？把 `disable_lane_change` 读进来的**最小改动点**在哪一行？
   (b) 「限速」需要一个「当前限速值」的来源。`DegradeState.l1_speed_limit` 是 `double`，0.0 表示不限。L1 限速时应该写多少？如果写一个非零值，`control_node.cpp:791-793` 的 L2 分支逻辑会怎样误用它？
   (c) 最根本的问题是：**L1 应该存在吗？** 一个既不限制速度、也不禁止变道、也不加大余量的降级等级，在概念上是不是多余的——把「模型可用性」这个正交的状态从降级等级里拆出来（比如单独一个 `model_enabled` 标志）会不会更清晰？这样降级等级就只剩下真正有物理后果的 L2/L3。请论证这两种设计的取舍。
