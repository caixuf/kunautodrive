# 第 19 章：执行器 —— PWM 与 SocketCAN 的最后一公里

> **本章导读**：
> 前面 18 章的所有输出，到本章为止都还只是一串浮点数。这一章是它们变成**电气信号**的地方：要么是 I2C 总线上写进 PCA9685 寄存器的 5 个字节，要么是 SocketCAN 套接字里发出的 8 字节报文。
>
> 这是整条链路上最短、也最容易出致命事故的一层。控制节点算出 0.3 rad 的转角、执行器把它变成 1500 + (0.3/0.22)×500 = 2181 μs——一个超出舵机量程 9% 的指令。如果钳位写错一个字符，舵机就会打到机械限位。
>
> 但在读这一章之前，先破除旧稿里几个根深蒂固的误解：
>
> 1. **仓库里没有 `send_can_frame()`，也没有 `set_servo_pulse()`。** 旧稿所有 C 代码片段都是凭空编造的。
> 2. **SocketCAN 后端（`actuator_node.c`）从未被任何流水线配置加载过。** 它被编译、被安装、被 CI 登记，但**零引用**。真正在跑的只有 PWM 那一个。
> 3. **执行器不碰串口。** `serial_write()` 全仓库零调用者。旧稿暗示 RC 小车走串口，这不对。
> 4. **默认配置里存在一个真实的、当前生效的参数不匹配**：安全层授权 ±0.35 rad，执行器在 ±0.22 rad 饱和，中间 37% 的授权被静默丢弃。第 8 节详述。

---

## 1. 两个后端，一个活着

```
  [safety_control_node] ──control/cmd (ControlCmd, 20B)──┐
                                                        │
        ┌───────────────────────────────────────────────┴──────────────┐
        │                                                              │
   活的分支（所有 config）                                         死的分支（零 config）
        │                                                              │
   actuator_pwm_node.c                                          actuator_node.c
   libactuator_pwm_node.so                                      libactuator_node.so
   571 行                                                        523 行
        │                                                              │
   I2C → PCA9685 寄存器                                          SocketCAN → can0
        │                                                              │
   ESC 电调 + 转向舵机                                           线控底盘 EPS + 刹车
```

| | `actuator_pwm_node.c` | `actuator_node.c` |
|---|---|---|
| 物理接口 | I2C → PCA9685 | SocketCAN |
| 行数 | 571 | 523 |
| 被 config 引用 | **是**（`pipeline_car.json:273`） | **否（零引用）** |
| 测试覆盖 | 10 个用例（映射层） | **0** |
| 文件自身状态 | 生产在用 | 文件头写明「无 pipeline 引用、无测试覆盖」 |

`actuator_node.c:16-18` 自己就交代了：

```c
 * 部署状态：真车 CAN 部署预留实现。当前 config/pipeline*.json 默认用
 * actuator_pwm_node（RC 小车形态），本节点无 pipeline 引用、无测试覆盖，
 * 真车高速场景切换部署时启用（见 config/pipeline_car.json 头部 _comment）。
```

**本节的正确读法**：CAN 后端不是「备用的第二条路」，而是**一段为未来保留、从未验证过的代码**。本章会以它为主线之一，因为真实的乘用车线控底盘就是这样接的；但读者必须清楚它当前不在生产路径上。

---

## 2. 订阅的那一条话题

两个节点都只订阅 `control/cmd`，都不发布任何东西：

```c
/* modules/adas_nodes/actuator_pwm_node.c:363-364 */
static const char* s_inputs[]  = { "control/cmd", NULL };
static const char* s_outputs[] = { NULL };

/* modules/adas_nodes/actuator_node.c:354-355 */
static const char* s_inputs[]  = { "control/cmd", NULL };
static char* /*static*/ s_outputs[] = { NULL };
```

**执行器是整条流水线的终点。** 它的输出不是消息，是物理量——没有下游可以再检查它，没有安全层可以再否决它。

> [!WARNING]
> `actuator_node.c` 里有**五处注释**声称它订阅 `control/raw_cmd`（`:4`、`:8`、`:122`、`:418`、`:512`，外加 `CMakeLists.txt:623`），而 `:434` 的实际代码是：
> ```c
> transport_subscribe(transport, "control/cmd", on_control_cmd, NULL);
> ```
> `control/raw_cmd` 是第 17 章控制节点的**内部输出**，是给安全层审的草稿；`control/cmd` 才是审完之后、真正能动车的指令。**这个区别很关键**：把执行器接到 `raw_cmd` 上，等于绕过整道安全闸门。
>
> 代码是对的，注释是错的。这类「代码正确但注释说谎」的情况在本仓库不罕见——读者应以代码为准。

---

## 3. PWM 映射：33 行，被精心抽成了纯函数

### 3.1 为什么单独抽出一个 .c 文件

`pwm_map.c` 整个文件只有 33 行，其中 25 行是 `pwm_map_control_cmd`。它被刻意从 571 行的节点里抽出来，理由写在 `pwm_map.h` 和节点的注释里（`actuator_pwm_node.c:275-276`）：

```c
/* 映射逻辑在 pwm_map.c（纯函数，tests/test_adas_nodes_logic.c 直接编同一份，
 * 真车改曲线时单测跟着变）。这里只负责取缩放参数 + 落到硬件。 */
```

**「直接编同一份」** 是这句话的重点：CMake（`CMakeLists.txt:1111`）把 `modules/adas_nodes/pwm_map.c` 加进测试目标，所以测试跑的**就是生产代码**，不是复制品。真车改了映射曲线，单测会跟着变。

这与第 18 章形成鲜明对比——那里 `apply_safety` 塞在 `.cpp` 匿名命名空间里，TTC 公式一行测试都没有。**同样是纯函数逻辑，抽出来与否决定了它能不能被验证。**

### 3.2 四个常量，一个都不过量

```c
/* modules/adas_nodes/pwm_map.h:17-21 */
#define PWM_CENTER_US      1500   /**< 中位脉宽 μs */
#define PWM_MIN_US         1000   /**< 安全下限 μs */
#define PWM_MAX_US         2000   /**< 安全上限 μs */
/** 舵机满量程对应的前轮转角（rad）。steering 输入按此归一化到 [-1, 1]。 */
#define PWM_MAX_STEER_RAD  0.22
```

还有一个在节点里而不是头文件里（`actuator_pwm_node.c:119`）：

```c
#define PWM_RANGE_US         500           /* ±500μs 对应 ±1.0 控制 */
```

**`PWM_RANGE_US` 放在节点里是个小遗憾**——它是 `throttle_scale` 和 `steering_scale` 的默认值，本该和 `PWM_CENTER_US` 放在一起。

### 3.3 完整实现

```c
/* modules/adas_nodes/pwm_map.c:7-33 —— 整个文件 */
static int clamp_pulse(int us) {
    if (us < PWM_MIN_US) return PWM_MIN_US;
    if (us > PWM_MAX_US) return PWM_MAX_US;
    return us;
}

void pwm_map_control_cmd(double throttle, double brake, double steering_rad,
                         int e_stop,
                         double throttle_scale, double steering_scale,
                         int* esc_us, int* steer_us) {
    int esc;
    if (e_stop) {
        esc = PWM_CENTER_US;                       /* 紧急停转：中位 */
    } else if (brake > 0.01) {
        esc = (int)(PWM_CENTER_US - brake * throttle_scale);
    } else {
        esc = (int)(PWM_CENTER_US + throttle * throttle_scale);
    }

    double steer_norm = steering_rad / PWM_MAX_STEER_RAD;
    if (steer_norm > 1.0) steer_norm = 1.0;
    if (steer_norm < -1.0) steer_norm = -1.0;
    int steer = (int)(PWM_CENTER_US + steer_norm * steering_scale);

    if (esc_us)   *esc_us   = clamp_pulse(esc);
    if (steer_us) *steer_us = clamp_pulse(steer);
}
```

纵向是一条三分支的优先级链：

```
e_stop ≠ 0  ──────────────►  esc = 1500              （ESC 停转）
brake > 0.01 ─────────────►  esc = 1500 − brake·S   （反打，反向制动）
     否则 ────────────────►  esc = 1500 + throttle·S（正打，前进）
```

三个要点：

**刹车映射是「往中位以下走」。** `1500 − brake·S`，`brake = 1.0` 且 `S = 500` 时得到 1000 μs，和「倒车」是同一个脉宽。**对 RC 电调来说，倒车和刹车本来就是同一个方向**——这与第 17 章控制节点「倒挡用负油门触发倒车」的思路一致（`control_node.cpp:885`）。

**刹车优先于油门。** `brake > 0.01` 就走刹车分支，`throttle` 被完全丢弃。测试 `test_pwm_brake_overrides_throttle` 锁死了这个行为：输入 `(throttle=1, brake=0.5)`，输出是 1250 μs 而不是 2000 μs。

**`e_stop` 只管 ESC，不管舵机。** 这是本章最需要警惕的一条，第 7.1 节详述。

### 3.4 转向：归一化后再换算

```c
double steer_norm = steering_rad / PWM_MAX_STEER_RAD;   /* 0.22 rad → 1.0 */
if (steer_norm > 1.0) steer_norm = 1.0;
if (steer_norm < -1.0) steer_norm = -1.0;
int steer = (int)(PWM_CENTER_US + steer_norm * steering_scale);
```

`PWM_MAX_STEER_RAD = 0.22` rad 是**满量程对应的前轮转角**。这个数字来自 `msg/adas_msgs.msg:208` 的契约：「转向角（rad）：正值右转，负值左转，范围约 ±0.22 rad，**actuator 负责归一化**」。

也就是说 **0.22 rad 这个数在整条链路上出现了四次**（第 8 节专门处理这个重复）。

### 3.5 截断而非四舍五入

```c
int esc   = (int)(...);      /* 截断 */
int steer = (int)(...);      /* 截断 */
```

`(int)` 是**向零截断**，不是四舍五入。`throttle = 0.999` 时 `1500 + 499.5 = 1999.5` → `1999`，而不是 2000。

这是一个 0.1% 的量化误差，在舵机上完全无害。但它意味着**任何测试如果用精确值断言，都必须知道这一点**——现有测试里 `test_pwm_custom_scale` 用 scale=300、`throttle=1.0`，得到精确的 1800，避开了这个边界。

---

## 4. 从微秒到 PCA9685 寄存器

### 4.1 PCA9685 的分频公式

```c
/* modules/adas_nodes/actuator_pwm_node.c:115-117 */
#define PWM_FREQ_HZ_DEFAULT  50
#define PCA9685_OSC_HZ       25000000ULL   /* PCA9685 内部振荡器 25MHz */
#define PCA9685_RESOLUTION   4096          /* 12-bit */
```

```c
/* pca9685_set_freq, :186-189 */
float prescaleval = (float)PCA9685_OSC_HZ / (float)(PCA9685_RESOLUTION * freq_hz) - 1.0f;
int prescale = (int)(prescaleval + 0.5f);
if (prescale < 3) prescale = 3;
if (prescale > 255) prescale = 255;
```

$$\text{prescale} = \frac{f_{\text{osc}}}{4096 \cdot f_{\text{pwm}}} - 1$$

代入 50 Hz：

$$\frac{25\times 10^6}{4096 \times 50} - 1 = 122.07 - 1 = 121.07 \;\to\; \textbf{121}$$

**这个 12 位分频器的可调范围是 f_osc / 4096 / 256 ≈ 23.8 Hz 到 24.4 kHz**——50 Hz 远在中间，是个安全的取值。

### 4.2 微秒 → tick

```c
/* pca9685_set_pulse_us, :206-223 */
static int pca9685_set_pulse_us(int fd, int addr, int channel, int pulse_us) {
    if (channel < 0 || channel > 15) return -1;
    /* tick = pulse_us / period_us * 4096 */
    int period_us = 1000000 / g.pwm_freq_hz;
    int tick = (int)((float)pulse_us / (float)period_us * (float)PCA9685_RESOLUTION + 0.5f);
    if (tick < 0) tick = 0;
    if (tick >= PCA9685_RESOLUTION) tick = PCA9685_RESOLUTION - 1;

    if (ioctl(fd, I2C_SLAVE, addr) < 0) return -1;
    uint8_t buf[5];
    buf[0] = PCA9685_LED_OFF_L(channel);
    buf[1] = 0;            /* ON_L */
    buf[2] = 0;            /* ON_H */
    buf[3] = tick & 0xFF;  /* OFF_L */
    buf[4] = (tick >> 8) & 0x0F;  /* OFF_H */
    if (write(fd, buf, 5) != 5) return -1;
    return 0;
}
```

$$t_{\text{tick}} = \frac{t_{\mu s}}{t_{\text{period}}} \times 4096, \qquad t_{\text{period}} = \frac{10^6}{f_{\text{pwm}}} = 20000\ \mu s$$

注意这里用了 `+ 0.5f` **四舍五入**，和 `pwm_map.c` 的截断不同——但这是正确的，因为 PCA9685 的 tick 直接决定脉宽，四舍五入能减小占空比误差。

三路 I2C 写入的参数（`scale = 500`）：

| 脉宽 | tick | 物理含义 |
|---|---|---|
| 1000 μs | `4096 × 1000/20000 = 204.8` → **205** | 满舵左 / 满刹车 |
| 1500 μs | `4096 × 1500/20000 = 307.2` → **307** | 中位 |
| 2000 μs | `4096 × 2000/20000 = 409.6` → **410** | 满舵右 / 满油门 |

`period_us` 从 `g.pwm_freq_hz` 算，**不是硬编码的 20000**——旧稿把它写死成字面量，掩盖了频率是运行时参数这一点。

**每次写都重新 `ioctl(fd, I2C_SLAVE, addr)`。** 在 40 Hz 的指令流下这是每秒 80 次系统调用，可以工作但并不优雅。正确做法是 `ioctl(I2C_SLAVE_FORCE)` 或在多设备场景下拼一次事务。

### 4.3 双重钳位

脉宽被钳了**两次**，分属两个文件：

```c
/* pwm_map.c:31-32 —— 用命名常量 */
if (esc_us)   *esc_us   = clamp_pulse(esc);
if (steer_us) *steer_us = clamp_pulse(steer);

/* actuator_pwm_node.c:252-253 —— 用字面量 */
if (pulse_us < 1000) pulse_us = 1000;
if (pulse_us > 2000) pulse_us = 2000;
```

第二处应该直接用 `PWM_MIN_US` / `PWM_MAX_US`。用字面量的后果是：**有人改了 `PWM_MAX_US` 宏，映射层会放行新范围，但硬件层仍然按旧的 2000 截断**——而且不会有任何编译警告。

> 这不是理论风险。第 8 节会讲到一个**当前就已经生效的、性质完全类似的截断**。

### 4.4 写失败是静默的

```c
/* actuator_pwm_node.c:284-290 */
if (g.backend == BACKEND_PCA9685) {
    pwm_set_pulse(g.esc_channel,   g.gpio_esc_pin,   esc_us);
    pwm_set_pulse(g.steer_channel, g.gpio_steer_pin, steer_us);
} else if (g.backend == BACKEND_GPIO) {
    pwm_set_pulse(g.esc_channel,   g.gpio_esc_pin,   esc_us);
    pwm_set_pulse(g.steer_channel, g.gpio_steer_pin, steer_us);
}
```

三个问题叠在一处：

1. **两个分支逐字节相同**——`backend` 在这里是个纯粹的空 dispatch，合并成一句无条件调用行为完全一致；
2. **`pwm_set_pulse` 的返回值被丢弃**——I2C 写失败（舵机掉线、I2C 总线被占、PCA9685 复位）不会有任何日志。安全层的 `+SAFE` 标签、control/debug 的一切诊断，在硬件层失效时统统看不见；
3. **这段代码位于 `on_control_cmd` 回调里**（`:273-303`），而 `pwm_set_pulse` 是一次阻塞式 I2C 写。如果 I2C 从设备卡住不回 ACK，**安全层的发布线程会被阻塞**——这个反馈路径会一路顶到第 18 节的发布循环。

### 4.5 上电先回中

```c
/* actuator_pwm_node.c:459-462 */
/* 初始中位输出（防止舵机上电跳到极限位置） */
pca9685_set_pulse_us(g.i2c_fd, g.i2c_addr, g.esc_channel,   PWM_CENTER_US);
pca9685_set_pulse_us(g.i2c_fd, g.i2c_addr, g.steer_channel, PWM_CENTER_US);
g.pca9685_inited = 1;
```

**这是一条真实的、有价值的实践**：PCA9685 上电后所有通道输出是 0（全开），如果不先写中位，舵机会先抖到极限再回来。清理时（`:527-536`）同样强制回中。

不过 `g.pca9685_inited` 这个标志**写了从不读**——典型的死变量。

---

## 5. SocketCAN 后端：真实的报文布局

CAN 后端不在生产路径上，但它是理解真车线控底盘接法的唯一材料。本节按真实代码展开。

### 5.1 三个报文 ID

```c
/* actuator_node.c:371-373 */
g.can_throttle_id  = 0x100;
g.can_steering_id  = 0x101;
g.can_status_id    = 0x102;
```

| ID | 含义 | DLC | 频率 | 负载布局 |
|---|---|---|---|---|
| **0x100** | 油门/刹车 | 8 | 每条 `control/cmd` | `[0-1]` throttle u16 LE<br>`[2-3]` brake u16 LE<br>`[4]` gear i8<br>`[5]` e_stop u8<br>`[6-7]` 保留（0） |
| **0x101** | 转向 | 4 | 每条 `control/cmd` | `[0-1]` steering i16 LE<br>`[2-3]` seq u16 LE |
| **0x102** | 状态心跳 | 8 | 10 Hz | `[0-3]` frames_sent u32 LE<br>`[4-7]` cmds_received u32 LE |

**旧稿声称油门和转向打包在同一个 8 字节帧的 `[0-3]`**——这与系统里任何一个 ID 都不匹配。真实实现是**两个独立报文**，因为真实的线控底盘协议就是这样分帧的（一个给动力总成，一个给转向）。

### 5.2 定点编码

```c
/* encode_throttle_frame, actuator_node.c:216-230 */
static void encode_throttle_frame(float throttle, float brake,
                                  int gear, int e_stop, uint8_t* out) {
    memset(out, 0, 8);
    /* 钳位到 [0,1] */
    if (throttle < 0) throttle = 0;
    if (throttle > 1) throttle = 1;
    if (brake    < 0) brake    = 0;
    if (brake    > 1) brake    = 1;
    uint16_t t_q = (uint16_t)(throttle * g.throttle_scale);
    uint16_t b_q = (uint16_t)(brake    * g.throttle_scale);
    out[0] = t_q & 0xFF; out[1] = (t_q >> 8) & 0xFF;
    out[2] = b_q & 0xFF; out[3] = (b_q >> 8) & 0xFF;
    out[4] = (uint8_t)gear;
    out[5] = e_stop ? 1 : 0;
}
```

$$q_{\text{throttle}} = \left\lfloor \text{throttle} \times 1000 \right\rfloor, \qquad q_{\text{brake}} = \left\lfloor \text{brake} \times 1000 \right\rfloor$$

`throttle_scale` / `steering_scale` 默认 **1000.0**（`:374-375`），即满量程对应整数 1000。这是 0~1000 的定点约定，10 位精度足够，接收方按 0.1% 解析即可。

**注意与 `pwm_map.c` 的一个不对称**：

| | `pwm_map.c` | `encode_throttle_frame` |
|---|---|---|
| 输入钳位 | **不钳位** throttle/brake | **钳位**到 [0,1] |
| 超范围行为 | `throttle=5.0` → 4000 → 输出钳到 2000 | `throttle=5.0` → 1.0 → 1000 |
| 符号 | 允许负（倒车） | 不允许负 |

CAN 路径**更严谨**——它先钳位再编码。PWM 路径依赖输出侧的 `clamp_pulse` 兜底。两条路径对同一份输入的处理方式不同，这是一个值得统一的分歧。

### 5.3 转向编码与那个重复的常量

```c
/* encode_steering_frame, actuator_node.c:232-242 */
static void encode_steering_frame(float steering, uint32_t seq, uint8_t* out) {
    /* ControlCmd.steering 是弧度（control_node 产出，限幅 ±0.22 rad），
     * actuator 硬件需要归一化 [-1,1]，故先除以最大转向角再量化。 */
    const float MAX_STEER_RAD = 0.22f;
    float steer_norm = steering / MAX_STEER_RAD;
    if (steer_norm < -1) steer_norm = -1;
    if (steer_norm >  1) steer_norm =  1;
    int16_t s_q = (int16_t)(steer_norm * g.steering_scale);
    out[0] = s_q & 0xFF; out[1] = (s_q >> 8) & 0xFF;
    out[2] = seq & 0xFF; out[3] = (seq >> 8) & 0xFF;
}
```

> [!IMPORTANT]
> **0.22 这个物理常数在两个互不相干的文件里各写了一遍，没有任何东西让它们保持同步。**
> - `modules/adas_nodes/pwm_map.h:21`：`#define PWM_MAX_STEER_RAD 0.22`（double）
> - `modules/adas_nodes/actuator_node.c:235`：`const float MAX_STEER_RAD = 0.22f;`（函数内局部变量）
>
> 换一辆转向比不同的车，要改两个地方，漏掉任何一个就会出现「PWM 满舵和 CAN 满舵对应不同转角」的不对称——**而其中一个后端不在生产路径上，所以它连一次实车验证的机会都没有。**

### 5.4 发送与 dry-run

```c
/* can_send, actuator_node.c:173-200 */
static int can_send(uint32_t can_id, const uint8_t* data, uint8_t dlc) {
    if (dlc > 8) dlc = 8;
    if (g.dry_run || g.can_sock < 0) {
        /* 降级模式：只打印，不发真实帧 */
        char hex[32] = {0};
        for (int i = 0; i < dlc; i++) snprintf(hex + i*3, 4, "%02x ", data[i]);
        LOG_INFO("actuator", "[dry-run] CAN 0x%03X [%d] %s", can_id, dlc, hex);
        return 0;
    }
    ...
```

**dry-run 下 `return 0`**（成功），而 `g.frames_sent` 的自增在 `:194`，**位于 dry-run 分支之后**。后果是：

- `frames_sent` 永远停在 0；
- 0x102 心跳帧的 `[0-3]` 字段永远是 0；
- `actuator_health`（`:501-506`）只比较 `frames_failed > frames_sent && frames_failed > 100`，在 dry-run 下 `frames_failed` 也是 0，**所以永远返回「健康」**。

**一个从不发帧的节点会一直宣称自己健康。**

### 5.5 dry-run 的四种触发条件

```c
/* actuator_node.c:423-431 —— 降级只在这里发生 */
if (!g.dry_run) {
    g.can_sock = can_open(g.can_interface);
    if (g.can_sock < 0) {
        LOG_WARN("actuator", "CAN open failed on '%s', falling back to dry-run "
                 "(真实硬件部署时检查: ip link set %s up)",
                 g.can_interface, g.can_interface);
        g.dry_run = 1;
    }
}
```

`can_open`（`:138-170`）的失败路径：socket 创建失败 / `SIOCGIFINDEX` 失败（网卡不存在）/ `bind` 失败 / 非 Linux 平台。

**加上显式 `dry_run=1` 参数，完整的触发条件是 5 种。**

> [!WARNING]
> **降级是单向的，永不重试。** 如果 `can0` 在节点启动 10 秒后才被 `ip link set can0 up` 拉起来，节点会**永久停留在 dry-run**，直到重启。真车部署时这是一个很容易踩的坑——尤其是开机自启的系统，网卡 up 的时刻和节点启动的时刻没有保证。
>
> `can_send:175` 里还有一次 `g.can_sock < 0` 的冗余检查，但那是防御性的，**不构成恢复路径**。

---

## 6. 看门狗：两个后端，两个实现

这是本章最有工程价值的一节，也是旧稿完全没写的一节（旧稿只讨论了「你该去买什么硬件」，没写仓库里真实存在的软件看门狗）。

### 6.1 PWM 后端

```c
/* actuator_pwm_node.c:330-352 */
static int actuator_pwm_execute(TaskBase* task) {
    pthread_setname_np(pthread_self(), "act_pwm_wd");
    while (!task->should_stop) {
        sleep(1);
        if (task->should_stop) break;
        time_t now = time(NULL);
        if (g.last_cmd_time > 0 && (now - g.last_cmd_time) > g.watchdog_timeout_s) {
            /* 超时：ESC 强制中位 */
            int esc_us = PWM_CENTER_US;
            int steer_us = PWM_CENTER_US;
            ...
            LOG_WARN("actuator_pwm", "WATCHDOG: %ds 无 cmd，强制 ESC 中位", g.watchdog_timeout_s);
            g.last_cmd_time = now;  /* 避免每秒重复告警 */
        }
    }
    return 0;
}
```

配置：`watchdog_timeout_s` 默认 **3**（`:390`），`pipeline_car.json:278` 显式设成 3。

四个需要指出的地方：

1. **日志说「强制 ESC 中位」，但 `:339` 同时把舵机也回中了。** 日志描述得比代码做的事少——这对排障是误导的（看到日志会以为转向没被动）。
2. **通过篡改时钟来重新武装**：`g.last_cmd_time = now`。看门狗因此只在每个 3 秒窗口触发**一次**，而不是持续输出中位帧。
3. **`g.last_cmd_time` 被两个线程共享且无锁**：`on_control_cmd:319` 写它，看门狗 `:348` 也写它。PWM 节点的命令路径是**事件驱动**的（transport 回调线程），看门狗是**独立的 1 Hz 任务线程**——这是一个真实的数据竞争。
4. **同样有那段逐字节相同的 `if/else` 分支**（`:340-346`）。

### 6.2 CAN 后端：实现得更好

```c
/* actuator_node.c:289-325 */
static int actuator_execute(TaskBase* task) {
    pthread_setname_np(pthread_self(), "act_hb");
    long period_us = 1000000L / (g.heartbeat_hz > 0 ? g.heartbeat_hz : 10);
    time_t last_wd_warn = 0;  /* 看门狗告警去抖：每 timeout 周期最多打一次 */
    while (!task->should_stop) {
        usleep((unsigned long)period_us);
        if (task->should_stop || !g.enabled) break;
        /* status 帧: [0-3] frames_sent, [4-7] cmds_received */
        uint8_t buf[8];
        memcpy(buf,     &g.frames_sent,    4);
        memcpy(buf + 4, &g.cmds_received,  4);
        can_send(g.can_status_id, buf, 8);

        /* ── 看门狗：超时无 cmd → 强制 ESC 中位 ── */
        if (g.last_cmd_time > 0 && g.watchdog_timeout_s > 0) {
            time_t now = time(NULL);
            if ((now - g.last_cmd_time) > g.watchdog_timeout_s) {
                uint8_t tbuf[8] = {0};
                uint8_t sbuf[4] = {0};
                encode_throttle_frame(0.0f, 0.0f, 0, 0, tbuf);
                encode_steering_frame(0.0f, 0, sbuf);
                can_send(g.can_throttle_id, tbuf, 8);
                can_send(g.can_steering_id, sbuf, 4);
                if (now - last_wd_warn >= g.watchdog_timeout_s) {
                    LOG_WARN("actuator", "WATCHDOG: %ds 无 control/cmd，强制 ESC 中位 "
                             "(thr=0/brk=0/steer=0)，检查 control_node/safety_control_node",
                             g.watchdog_timeout_s);
                    last_wd_warn = now;
                }
                g.last_cmd_time = now;
            }
        }
    }
    return 0;
}
```

**这个实现比 PWM 那个好两处**：

| | PWM 节点 | CAN 节点 |
|---|---|---|
| 告警去抖 | 复用 `g.last_cmd_time`（与命令路径共享） | 独立的 `last_wd_warn` 局部变量 |
| 超时期间 | 每 3 s 输出一次中位帧 | **每 100 ms（10 Hz 心跳）持续输出中位帧** |

**持续输出比输出一次重要得多。** 真实的线控底盘通常有接收超时——它收到一帧就重置自己的计时器。只发一次中位帧，如果这帧因为总线错误丢了，接收方的计时器会继续走，最终自己超时（可能触发更粗暴的动作）。**持续发送中位帧把决定权留在了软件的精细控制之下。**

但它仍有一个**启动漏洞**：

```c
if (g.last_cmd_time > 0 && g.watchdog_timeout_s > 0)
```

CAN 节点的 `g.last_cmd_time` **只在 `on_control_cmd:260` 里赋值**，节点启动时从不初始化。所以**在第一条 `control/cmd` 到达之前，看门狗完全惰性**——`0 > 0` 为假，它永远不会触发。

> **如果 `control/cmd` 从未到达，CAN 节点会永远只发 0x102 心跳帧，一条中位帧都不发。** 车保持的是「最后一次被写入的物理状态」——如果上一次运行留下的占空比是满油门，车就会保持满油门。
>
> 对比 PWM 节点：`actuator_pwm_start:502` 在启动时设了 `g.last_cmd_time = time(NULL)`，看门狗从第 3 秒起就是活的。**这个不对称的安全性差异值得注意。**

### 6.3 三个时间常数互不同步

| 看门狗 | 位置 | 阈值 | 时钟 |
|---|---|---|---|
| PWM 执行器 | `actuator_pwm_node.c:336` | `watchdog_timeout_s`，默认 **3 s** | `time(NULL)`，**1 s 粒度** |
| CAN 执行器 | `actuator_node.c:305` | **硬编码 3 s**（不可配） | `time(NULL)`，1 s 粒度 |
| PWM 健康检查 | `actuator_pwm_node.c:552` | **硬编码 5 s** | `time(NULL)` |
| 安全层 raw_cmd 看门狗 | `safety_control_node.cpp:549` | **2 s** | `clock_now_us()`，微秒 |
| FlowSim 指令过期 | `flowsim_node.cpp:75` | **2 s** | 微秒 |

三个问题：

1. **`time(NULL)` 是 1 秒粒度**——标称 3 s 的看门狗，实际触发点在 3~4 s 之间；
2. **CAN 节点的 `watchdog_timeout_s` 无法配置**——PWM 节点在 `:419-420` 解析它，CAN 节点只写死不读；
3. **PWM 健康检查的 5 s 与看门狗的 3 s 是两个独立的硬编码字面量**，没有任何注释解释为什么一个 3 一个 5。

> **模块间的超时契约**：安全层 2 s → 执行器 3 s → 硬件超时（若存在）。这个 2 < 3 的顺序是对的——**上游先于下游放弃**，让下游有机会执行更温和的减速，而不是两边同时踩死。旧稿建议的「硬件 100 ms 超时」在数量级上完全不是一回事。

---

## 7. 安全语义：e_stop、brake 和一个危险的区间

### 7.1 e_stop 不回正转向

```c
/* pwm_map.c:18-19 */
if (e_stop) {
    esc = PWM_CENTER_US;                       /* 紧急停转：中位 */
}
```

**这个分支只覆盖 `esc`。转向走的是完全独立的第 26-29 行**：

```c
double steer_norm = steering_rad / PWM_MAX_STEER_RAD;
...
int steer = (int)(PWM_CENTER_US + steer_norm * steering_scale);
```

**所以 `emergency_stop = true` 时，ESC 会回中，但舵机会保持在最后一次指令的转角上。**

而 `emergency_stop` 的来源是第 18 章的 `safety_control_node.cpp:829`：

```cpp
bin.emergency_stop = cmd.brake > 0.95;
```

（外加 NaN 路径的 `:819-824` 强制 `emergency_stop = true`，那条路径同时把 `steering = 0`。）

> [!WARNING]
> **这是一个真实的安全语义缺口。** 一次 AEB 触发的紧急制动，车的**纵向**会立刻停住，但**转向会锁死在紧急制动发生时的角度**。如果制动发生在变道中途，车会停在一个横跨两条车道的位置。
>
> 现有测试 `test_pwm_e_stop_overrides_all`（`tests/test_adas_nodes_logic.c:331`）**只断言了 `esc == 1500`，从未断言 `steer`**。这个行为既没被规定，也没被验证——它到底是有意的还是疏忽，从代码里读不出来。
>
> 对比看门狗（`:338-339`）——**超时路径是把两个都回中的**。同一个文件里，「紧急停」不回中、「超时停」回中，这个不一致尤其刺眼。

### 7.2 刹车死区：0.01 到 0.95 整段都在反打

`pwm_map.c:20` 的 `brake > 0.01` 是一个**没有命名的字面量**（全文件唯一一个）。它的后果比看上去严重：

```c
} else if (brake > 0.01) {
    esc = (int)(PWM_CENTER_US - brake * throttle_scale);
}
```

`throttle_scale = 500` 时：

| `brake` | `esc` (μs) | ESC 行为 | `emergency_stop` |
|---|---|---|---|
| 0.005 | 1500（走油门分支，throttle=0） | 中位 | false |
| 0.01 | 1500 − 5 = **1495** | **轻微反向** | false |
| 0.10 | 1500 − 50 = **1450** | **反向** | false |
| 0.30 | 1500 − 150 = **1350** | **反向** | false |
| 0.70 | 1500 − 350 = **1150** | **反向** | false |
| 0.95 | 1500 − 475 = **1025** | 反向 | true → 强制 **1500** |
| 1.00 | （e_stop 分支）**1500** | 中位 | true |

**在 `brake ∈ (0.01, 0.95)` 这一整段里，ESC 被命令往中位以下走，也就是反打。** 而 `emergency_stop` 恰好在 0.95 处跳到 1500。

对 RC 电调来说这是**符合物理的**——无刷电调的反转就是靠反打刹车实现的，第 5.2 节说过。但对**真车**（线性制动 + 独立 CAN 通道）就完全不对了：第 5.2 节的 `encode_throttle_frame` 把 brake 编码成独立的 `b_q` 字段交给底盘，**由底盘决定是「减速」还是「倒车」**。同一份 `ControlCmd`，PWM 后端把它解释成反打，CAN 后端把它解释成正向制动减速度。

**这正是「执行器后端不是可互换的插件」的根本原因。** 换后端不是改一行配置，是换一套物理语义。

### 7.3 hazard 被完全丢弃

`hazard`（双闪）在两个执行器里**出现零次**。它被反序列化进 `ControlCmd` 结构体，然后没有任何人读。

第 18 章里，`safety_control_node.cpp:797/801` 在 L2/L3 降级时会设 `cmd.hazard = true`——**这个意图没有到达执行器**。双闪只在 `flowsim` 里被用来渲染车灯。

真车上这意味着：**L3 紧急停车时，车辆的警示灯不会闪。** 这不影响安全（车停了），但影响后续救援和二次事故防范。

### 7.4 完整的安全链条

```
control_node (40 Hz)                    ← 第 17 章
    │ control/raw_cmd
    ▼
safety_control_node (5 ms 轮询)         ← 第 18 章
    │ control/cmd
    │   + NaN → throttle=0, brake=1, steer=0, e_stop=true
    │   + e_stop = (brake > 0.95)
    ▼
actuator_pwm_node (事件驱动 + 1 Hz 看门狗)   ← 本章
    │   pwm_map_control_cmd()
    │   brake>0.01 → 反打；e_stop → ESC 中位（转向不回中）
    │   3 s 无 cmd → ESC + 舵机都回中
    ▼
PCA9685 寄存器 (I2C, 5 字节)
    ▼
ESC + 舵机
```

**每一层都有自己独立的失效保护，而它们的语义并不完全一致**——7.1 节的 e_stop 差异就是最直观的证据。跨层契约目前只存在于注释里，没有任何代码在构建期或运行期校验这些常量是否匹配。

---

## 8. 一个当前生效的配置不匹配

这是本轮核对中最有价值的发现。

### 8.1 0.22 这个数出现在四个地方

| # | 位置 | 形式 | 值 |
|---|---|---|---|
| 1 | `msg/adas_msgs.msg:208` | 消息契约注释 | 「约 ±0.22 rad，actuator 负责归一化」 |
| 2 | `modules/adas_nodes/pwm_map.h:21` | `#define PWM_MAX_STEER_RAD` | `0.22`（double） |
| 3 | `modules/adas_nodes/actuator_node.c:235` | `const float MAX_STEER_RAD` | `0.22f`（函数内局部） |
| 4 | `modules/adas_nodes/safety_control_node.cpp:77` | `max_steer` 默认值 | `0.22` |

**四处没有任何编译期关联。** 2 和 3 是同一个物理量在两个文件里的独立拷贝。

### 8.2 但实际配置不是 0.22

`config/pipeline_car.json:269`：

```json
"params": "{\"max_throttle\":0.6,\"max_steer\":0.35,\"low_speed_steer\":0.35,\"time_headway\":1.0}"
```

**安全层被配置成授权 ±0.35 rad——比执行器的量程大了 59%。**

### 8.3 后果：37% 的授权被静默丢弃

```c
/* pwm_map.c:26-28 */
double steer_norm = steering_rad / PWM_MAX_STEER_RAD;   /* 除以 0.22 */
if (steer_norm > 1.0) steer_norm = 1.0;                 /* 超过就截断 */
if (steer_norm < -1.0) steer_norm = -1.0;
```

| `steering` (rad) | `steer_norm` | `steer_us` | 说明 |
|---|---|---|---|
| 0.22 | 1.000 | 2000 | 满舵 |
| 0.26 | 1.182 → **1.0** | **2000** | 已截断 |
| 0.30 | 1.364 → **1.0** | **2000** | 已截断 |
| 0.35 | 1.591 → **1.0** | **2000** | 已截断 |

**安全层认为它授权了 0.22~0.35 rad 这一段，实际上这段指令全部变成同一个 2000 μs 输出。**

这不是一个抽象的隐患，它有具体后果：

- **控制层完全不知道自己在饱和。** 第 17 章的横向级联 PD 看到的是「我给出了 0.30 rad 的请求」，执行器给的是「满舵 2000 μs」。两者之间的差不会被任何一层察觉；
- **降级逻辑的判断依据失效。** `safety_control_node` 的 `+SAFE` 标签、`degrade_ladder` 的状态，都建立在「我发布的指令就是最终指令」这个假设上；
- **`ROAD_GUARD` 回正会变慢。** `control_node.cpp:1132-1148` 的强制回正请求 2.4 m/s² 权限下的 `steer_limit_for_speed`，高速时可能落在 0.16 rad 以内（不触发问题），但在 `pipeline_car.json` 把安全限幅放宽到 0.35 的前提下，中间区间是未定义的。

> [!IMPORTANT]
> **根本原因和第 4.3 节那个字面量钳位是同一个**：层与层之间的**约束没有单一事实来源**。安全层和执行器各自持有「转向可以到多大」这个知识，谁也不问谁。
>
> 这与第 17 章 3.5 节讲的那个问题完全同构——当时是「控制层限幅 1.4 < 规划层预算 4.25，导致控制层锁死一条合法轨迹」。**那次修复是把控制层的 `a_max` 从 1.4 调到 4.0 让它 ⊇ 规划层；这次的问题是安全层 0.35 > 执行层 0.22，方向刚好相反。** 两个问题，一个共同根因。

**最小修复**：把 `pipeline_car.json:269` 的 `max_steer` 和 `low_speed_steer` 改回 0.22（或把 `PWM_MAX_STEER_RAD` 提到 0.35 并确认舵机物理上够得到）。正确的修复是建立一个启动期的一致性校验。

---

## 9. 测试：只有 PWM 映射有，且编的是生产代码

### 9.1 10 个用例锁住映射层

`tests/test_adas_nodes_logic.c` 里的 10 个 PWM 用例（ctest 名 `adas_nodes_logic_tests`）：

| 测试 | 行 | 输入 (thr, brk, steer, e_stop) | 断言 |
|---|---|---|---|
| `test_pwm_throttle_full_forward` | `:295` | (1, 0, 0, 0) | esc=2000, steer=1500 |
| `test_pwm_throttle_full_reverse` | `:304` | (−1, 0, 0, 0) | esc=1000 |
| `test_pwm_brake_full` | `:312` | (0, 1, 0, 0) | esc=1000 |
| `test_pwm_brake_overrides_throttle` | `:320` | (1, 0.5, 0, 0) | **esc=1250**（不是 2000） |
| `test_pwm_e_stop_overrides_all` | `:331` | (1, 1, 0.5, 1) | **只断言 esc=1500** |
| `test_pwm_steering_max_left` | `:340` | (0, 0, +0.22, 0) | steer=2000 |
| `test_pwm_steering_max_right` | `:353` | (0, 0, −0.22, 0) | steer=1000 |
| `test_pwm_steering_clamp` | `:361` | (0, 0, ±0.5, 0) | 2000 / 1000 |
| `test_pwm_zero_cmd_is_neutral` | `:371` | (0, 0, 0, 0) | 1500 / 1500 |
| `test_pwm_custom_scale` | `:380` | (1,0,0,0), scale=300 | esc=1800 |

`test_pwm_brake_overrides_throttle` 和 `test_pwm_steering_clamp` 特别有价值——它们锁住的正是「钳位」和「优先级」这两个最容易被后续修改破坏的性质。

### 9.2 测试盲区

| 未覆盖 | 后果 |
|---|---|
| `test_pwm_e_stop_overrides_all` **不断言 `steer`** | 7.1 节的行为完全无约束 |
| `brake == 0.01` 边界 | `>` 而非 `>=`，0.01 本身走油门分支。差一个 LSB 的错配测不出来 |
| throttle 超出 [0,1] 时的输出钳位 | `pwm_map.c` 不钳位输入，全靠 `clamp_pulse` 兜底，这条路径没测 |
| `(int)` 截断行为 | 量化误差无感知 |
| **CAN 后端全部** | `encode_throttle_frame`、`encode_steering_frame`、`can_open`、`can_send`、`parse_hex_int` 零测试，且**没有像 `pwm_map.c` 那样被抽出来** |
| PCA9685 tick 数学与 prescale 公式 | 频率写错（比如设成 100 Hz）会静默产生错误脉宽 |
| 两个看门狗 | 超时行为无验证 |
| `gpio_set_pulse_us` | 见 9.4 |

### 9.3 两处过期的行号注释

```c
/* tests/test_adas_nodes_logic.c:322 */
"源文件 actuator_pwm_node.c:280 用 `else if (brake > 0.01)`"     /* 现在在 pwm_map.c:20 */

/* tests/test_adas_nodes_logic.c:342-344 */
"源文件 actuator_pwm_node.c:286-291 的实现是..."                  /* 现在在 pwm_map.c:26-29 */
```

**这是 `pwm_map` 抽取重构留下的残余。** 注释指向的代码已经搬家了。行为是对的（测试通过），但注释会误导下一个来读这段代码的人。

### 9.4 GPIO 后端：名义上存在，实际上不工作

```c
/* actuator_pwm_node.c:231-243 */
static int gpio_set_pulse_us(int channel_unused, int pin, int pulse_us) {
    (void)channel_unused;
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip0/pwm%d/duty_cycle", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char val[16];
    snprintf(val, sizeof(val), "%d", pulse_us * 1000);   /* duty_cycle 单位 ns */
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n > 0) ? 0 : -1;
}
```

四个问题，每一个都足以让它无法工作：

1. **硬编码 `pwmchip0`**，忽略操作系统实际分配的 chip 编号；
2. **把 GPIO 引脚号（12/13）当成 pwmchip 子索引**——这两个是完全不同的编号空间；
3. **从不设置 `period` 或 `polarity`**，所以 50 Hz 周期完全取决于用户预先的设备树配置；
4. **从不执行 `export`** 来启用通道。

而 `pwm_set_pulse` 的返回值被丢弃，**所以这些失败全部静默**。它的头部注释（`:229`）还声称「仅记录目标脉宽」——它连记录都没做。

`backend` 参数可以设成 `"gpio"`，但这条路径在实践中不可用。**因为没人用过，所以没人发现。**

---

## 10. 死代码清单

| 项 | 位置 | 状态 |
|---|---|---|
| **`actuator_node.c` 整个节点** | `modules/adas_nodes/CMakeLists.txt:627` 编译 | **零 config 引用** `libactuator_node.so` |
| **`serial_write()`** | `serial_port.c:163` | **全仓库零调用者**。`serial_open` 只有 gps/imu/lidar 三个**只读**驱动在用 |
| `parse_hex_int()` | `actuator_node.c:336-350` | 只被死节点用；且违反本文件 `:387` 自己声明的 cJSON 约定 |
| `g.pca9685_inited` | `:152` 写于 `:462` | 写入，从不读取 |
| `g.last_seq` | `:158` 写于 `:318` | 写入，从不读取（**没有陈旧序号检测**） |
| `g.transport` / `g.discovery` | `:128-129` | 存下来，init 之后再没用过 |
| 三处相同 `if/else` | `:284-290`、`:340-346`、`:528-535` | 两分支代码逐字节相同 |
| `gpio_set_pulse_us` | `:231-243` | 见 9.4 |
| `actuator_health`（CAN） | `:501-506` | 注释称「最近 10 秒内若收到过指令」，**代码里没有任何时间判断** |
| `watchdog_timeout_s`（CAN） | `:380` 写死 3 | 从不解析 JSON |
| GPIO 后端的 init 降级检查 | 缺失 | `/sys/class/pwm` 不存在只在首次写入时暴露，而错误被丢弃 |

> **关于 `serial_write`**：旧稿的叙述（以及 `docs/CODE_WIKI.md` 的相关描述）容易让人以为 RC 小车通过串口下发指令。**不是的。** 串口在本仓库里只有三个用途——GPS、IMU、激光雷达，都是**读传感器**。执行器走的是 I2C 或 CAN。

---

## 11. 真车上车前的约定（硬件层，非代码）

前面十节讲的都是仓库里真实存在的代码。但有几条约定是**软件管不到、必须由硬件保证**的——旧稿只写了两条且都不完整，这里按当前实现补齐。

### 11.1 物理急停（软件无法替代）

任何纯软件急停都随操作系统一起崩。底盘必须串一个**常闭触点的物理断电急停**，直接切断动力电池到电机的回路。

这与第 18 章的 `emergency_stop` 是**两条独立的路径**——前者不经软件，后者经消息总线。软件路径在任何情况下都是辅助。

### 11.2 接收端超时

ESC 和线控转向控制器内部必须有独立硬件定时器。KunAutoDrive 的软件看门狗是 3 s（`actuator_pwm_node.c:390` / `actuator_node.c:380`），但它在**软件线程**里——线程卡死、CPU 被实时任务抢占、I2C 阻塞都会让它失效。硬件计时器不受这些影响。

**数量级上不要照抄旧稿的 100 ms。** 3 s 是这个仓库基于「安全层 2 s 先于执行器 3 s 放弃」的分层策略选的；硬件计时器应该配合底盘的实际制动能力单独标定。

### 11.3 当前实现无法保证的两条

| 需求 | 现状 |
|---|---|
| 转向死区补偿（0.5°~1.0° 机械间隙） | **无**。`pwm_map.c` 只有一个 0.005 rad 的死区，且在第 17 章的 `control_node` 里，不在执行器 |
| 转向角速度限制 | **无**。第 17 章的 MPC 有 `max_dsteer`，但那在优化之后就被丢掉了；执行器只做位置量 |

这两条在真车上都需要：机械死区会让小转角指令完全失效，电机过热保护需要角速度限制。旧稿提到了这两点但没有说明仓库里**没有实现**。

---

## 12. 源码与资源对照

| 模块 / 层次 | 源码文件路径 | 核心符号 / API | 架构职责与设计要点 |
|---|---|---|---|
| **PWM 映射（纯函数）** | [`modules/adas_nodes/pwm_map.h`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/pwm_map.h)<br>[`modules/adas_nodes/pwm_map.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/pwm_map.c) | `pwm_map_control_cmd`（`.c:13`）<br>`clamp_pulse`（`.c:7`）<br>`PWM_CENTER_US`/`PWM_MIN_US`/`PWM_MAX_US`/`PWM_MAX_STEER_RAD` | 全文件 33 行。`e_stop` > `brake>0.01` > `throttle` 三级优先；**e_stop 只管 ESC 不管舵机**；输入不钳位，靠输出钳位兜底 |
| **PWM 执行器节点** | [`modules/adas_nodes/actuator_pwm_node.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/actuator_pwm_node.c)<br>[`modules/adas_nodes/CMakeLists.txt:733`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/CMakeLists.txt) | `apply_control_cmd`（`:273`）<br>`pwm_set_pulse`（`:250`）<br>`pca9685_set_pulse_us`（`:206`）<br>`pca9685_set_freq`（`:184`）<br>`actuator_pwm_execute`（`:330`） | 571 行，`add_library(actuator_pwm_node SHARED actuator_pwm_node.c pwm_map.c)`。订阅 `control/cmd`，不发布。事件驱动命令 + 1 Hz 看门狗线程；3 s 超时 ESC+舵机双双回中 |
| **CAN 执行器节点（未部署）** | [`modules/adas_nodes/actuator_node.c`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/actuator_node.c) | `can_open`（`:138`）<br>`can_send`（`:173`）<br>`encode_throttle_frame`（`:216`）<br>`encode_steering_frame`（`:232`）<br>`actuator_execute`（`:289`） | 523 行。**零 config 引用**。0x100/0x101/0x102 三报文；10 Hz 状态心跳 + 3 s 看门狗；单向降级到 dry-run 永不恢复 |
| **串口层（执行器未用）** | [`include/serial_port.h`](file:///home/caixuf/code/FlowEngine/include/serial_port.h)<br>[`src/algorithms/serial_port.c`](file:///home/caixuf/code/FlowEngine/src/algorithms/serial_port.c) | `serial_open` / `serial_read_line` / `serial_read`<br>**`serial_write`（`:163`，零调用者）** | 只有 GPS/IMU/激光雷达三个**只读**驱动在用。**无锁**；`O_NONBLOCK` 被清除，写阻塞无超时 |
| **消息定义** | [`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) | `ControlCmd`（`:204-213`） | 执行器唯一输入。`steering` 契约「±0.22 rad，actuator 负责归一化」 |
| **管道配置** | [`config/pipeline_car.json`](file:///home/caixuf/code/FlowEngine/config/pipeline_car.json) | actuator 段（`:272-278`）<br>**safety params（`:269`）** | `libactuator_pwm_node.so`，`throttle_scale`/`steering_scale` 均为 500，看门狗 3 s。**`:269` 的 `max_steer: 0.35` 与执行器的 0.22 不匹配** |
| **测试** | [`tests/test_adas_nodes_logic.c`](file:///home/caixuf/code/FlowEngine/tests/test_adas_nodes_logic.c) | 10 个 `test_pwm_*`（`:295-389`）<br>ctest `adas_nodes_logic_tests` | `CMakeLists.txt:1110` 编译**生产同一份** `pwm_map.c`。**CAN 后端零测试**；e_stop 不断言 `steer` |
| **被控对象** | [`modules/adas_nodes/flowsim/physics.cpp`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/flowsim/physics.cpp) | `step_bicycle`（见第 20 章） | 仿真里没有执行器——`control/cmd` 由 flowsim 直接消费 |

---

## 13. 思考题

1. **建立跨层的转向量程契约**：
   第 8 节发现 `pipeline_car.json:269` 的 `max_steer: 0.35` 超过执行器 `PWM_MAX_STEER_RAD = 0.22`，导致 37% 的授权被静默截断。根因是「转向可以到多大」这个知识在四个地方各存一份，彼此无关。
   (a) 设计一个修复：是把配置改回 0.22，还是把常量提到 0.35？请分别说明两种做法的前提条件（舵机物理量程、转向机构传动比、EPS 行程限位）。
   (b) 真正的修复应该是「单一事实来源 + 启动期校验」。请设计：常量应该定义在哪一层（消息 IDL？单独的 `vehicle_spec.h`？），校验应该在构建期（CMake 静态断言）还是运行期（节点启动自检）？如果两边都需要，各自负责什么？
   (c) 这个模式和第 17 章 3.5 节的「控制层限幅 1.4 < 规划层预算 4.25」是同一类问题。两次的**方向相反**（那次是上游大于下游，这次是下游大于上游）。请论证：一个系统的层间约束应该满足哪种偏序关系——是「下游 ⊇ 上游」（下游权限更大）还是「下游 ⊆ 上游」？为什么？

2. **e_stop 为什么不回正转向？**：
   第 7.1 节指出 `pwm_map.c` 的 e_stop 分支只覆盖 `esc`，转向保持原角度；而同文件的看门狗路径却把两个都回中。`test_pwm_e_stop_overrides_all` 也没有断言 `steer`。
   (a) 这可能是三种情况之一：(i) 有意的——AEB 时保持转向可以让车停在原车道上；(ii) 疏忽——写的时候忘了；(iii) 依赖上游——假定上游在 e_stop 时已经把 `steering` 置 0。请检查第 18 章的 `safety_control_node.cpp:819-824`（NaN 路径）和 `:829`（`brake > 0.95` 路径），判断实际是哪种。
   (b) 如果结论是 (ii)，修复应该放在哪里？直接在 `pwm_map.c` 的 e_stop 分支里加 `steer = PWM_CENTER_US`，还是在 `safety_control_node` 的 e_stop 推导里加？请分别评估这两种做法对**其他后端**（CAN）和**其他调用方**的影响。
   (c) 更根本的问题：`emergency_stop` 作为一个独立字段是否还有存在必要？它当前恒等于 `brake > 0.95`。如果删掉它、让每个后端自己从 `brake` 推导，能避免多少下游不一致？（提示：对比 CAN 后端——`encode_throttle_frame` 只是把 e_stop 当一个标志位传下去，刹车值本身不归零。）

3. **给 CAN 后端补测试**：
   第 9.2 节指出 CAN 后端零测试，且没有像 `pwm_map.c` 那样被抽成可链接的独立文件。
   (a) 参照 `pwm_map.c` 的做法，把 `encode_throttle_frame` 和 `encode_steering_frame` 抽到 `actuator_encode.c` / `.h`。这两个函数依赖 `g.throttle_scale` / `g.steering_scale` 两个全局量——抽取时如何处理这个依赖？是加参数，还是把 scale 移进一个显式的 context 结构体？
   (b) 为 `encode_throttle_frame` 设计至少 8 个用例：throttle=0/0.5/1.0/2.0/−0.5，brake=0.5，gear=−1，e_stop=1。特别地——**`throttle = 2.0` 应该编码成什么？** 当前代码钳到 1.0 → 1000。这个钳位是正确的吗，还是应该检出错误并拒绝发送？（考虑 CAN 接收方看到 1000 和看到「无效帧」的区别。）
   (c) 补上 `steering = 0.35`（第 8 节那个超量程值）的用例。当前会截断到 1000。这个用例应该断言什么？如果答案是「应该被检出为越界」，那么修复后行为是什么——拒绝发送、还是钳位并报警？两种选择的代价分别是什么？

4. **看门狗的三种策略对比**：
   第 6 节对比了三种超时保护。假设你要重新设计这一层：
   (a) **软件看门狗**（当前实现）：3 s 线程，失效于线程卡死/IO 阻塞。
   (b) **接收端硬件超时**（第 11.2 节）：ESC/底盘内部计时器，失效于配置不当。
   (c) **看门狗心跳报文**：执行器以固定频率在 0x102 里回送 `cmds_received`，底盘检测到心跳中断就停。
   请论证 (c) 相比 (a) 的优势——特别是：`0x102` 里加一个「我上一次看到 cmd 的时间戳」字段，底盘就能做比「有无心跳」精确得多的判断（比如「心跳在但 cmd 计数 5 秒没涨」说明指令流卡死但执行器还活着）。这个设计能覆盖哪些 (a) 和 (b) 都覆盖不了的失效模式？
   (d) 无论选哪种，**阈值应该怎么定**？当前是安全层 2 s < 执行器 3 s。这个排序的依据是什么？如果硬件超时设成 500 ms（远小于 3 s），会发生什么——是更安全还是更危险？
