# 附录 A：真车部署与硬件落地指南（SocketCAN & PWM）

> **本章导读**：
> 软件仿真的终点是物理世界的真车落地。无论是在 1:10 比例的 RC 智能小车（树莓派 / Jetson Orin Nano 底盘），还是在真实的乘用车/商用车线控底盘上，控制指令最终都必须通过物理电气接口（如 CAN 总线或 PWM 脉宽调制信号）发送给电子调速器（ESC）与转向舵机/线控转向机（EPS）。
>
> 本附录详细梳理从 **KunAutoDrive 软件流水线到硬件电气接口的最后一公里**：涵盖 **Linux SocketCAN 内核网络驱动、MCP2515 SPI-CAN 模块配置、PCA9685 PWM 舵机驱动以及真车安全互锁规范**。

---

## 1. 硬件连接拓扑全景

```
  ┌─────────────────────────────────────────────────────────────┐
  │         KunAutoDrive 软件流水线 (IPC / MessageBus)            │
  │  control_node ──► safety_control_node (TTC 限幅) ──►        │
  └──────────────────────────────┬──────────────────────────────┘
                                 │ control/cmd (ControlCmd 消息)
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │          执行器节点插件 (Actuator Node Plugin)              │
  │     ├── 分支 1: actuator_node (SocketCAN 线控底盘)          │
  │     └── 分支 2: actuator_pwm_node (I2C/PCA9685 RC 小车)     │
  └──────────────┬──────────────────────────────┬───────────────┘
                 │ (CAN 报文 / can0)             │ (I2C 脉冲 / dev/i2c-1)
                 ▼                              ▼
  ┌──────────────────────────────┐┌─────────────────────────────┐
  │ 线控底盘 / 乘用车 CAN 网络    ││ PCA9685 16路 PWM 驱动板     │
  │ (500kbps 差分信号 CAN_H/L)   ││ (50Hz 周期, 1.0~2.0ms 脉宽) │
  └──────────────┬───────────────┘└─────────────┬───────────────┘
                 │                              │
                 ▼                              ▼
     [真实线控转向 EPS / 刹车]           [ESC 电调油门 / 转向舵机]
```

---

## 2. Linux SocketCAN 驱动与报文编码

### 2.1 什么是 SocketCAN？
SocketCAN 是 Linux 内核原生的 CAN 总线抽象层。它将 CAN 控制器虚拟化为标准的网络设备（如 `can0`, `vcan0`）。应用程序通过熟悉的 POSIX Socket API (`socket(PF_CAN, SOCK_RAW, CAN_RAW)`) 进行收发，就像收发 UDP/IP 数据包一样简单。

### 2.2 树莓派 / Jetson 硬件使能（MCP2515 示例）
在树莓派 `/boot/firmware/config.txt` 中开启 SPI 与 CAN 覆盖层：
```ini
dtparam=spi=on
dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
```

启动并配置波特率（通常汽车底盘为 500kbps）：
```bash
# 配置 500kbps 速率并拉起接口
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up

# 验证收发数据 (使用 can-utils)
candump can0
cansend can0 100#0102030405060708
```

### 2.3 C 语言 SocketCAN 报文打包（`actuator_node.c`）
```c
#include <linux/can.h>
#include <linux/can/raw.h>

int send_can_frame(int socket_fd, uint32_t can_id, float throttle, float steer) {
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id  = can_id;
    frame.can_dlc = 8; // 8 字节负载

    // 将浮点控制量转换为 16 位整型 (定点数定标)
    int16_t throttle_raw = (int16_t)(throttle * 1000.0f);
    int16_t steer_raw    = (int16_t)(steer * 1000.0f);

    frame.data[0] = (uint8_t)(throttle_raw & 0xFF);
    frame.data[1] = (uint8_t)((throttle_raw >> 8) & 0xFF);
    frame.data[2] = (uint8_t)(steer_raw & 0xFF);
    frame.data[3] = (uint8_t)((steer_raw >> 8) & 0xFF);

    // 发送报文
    return write(socket_fd, &frame, sizeof(frame));
}
```

---

## 3. PCA9685 I2C-PWM 舵机驱动（RC 智能小车）

在 1:10 遥控模型小车中，电调（ESC）和舵机通常接收 $50\text{ Hz}$（周期 $20\text{ ms}$）的标准 RC PWM 脉冲：
- **$1.5\text{ ms}$ 脉宽（高电平）**：中立位置（停止 / 方向居中）；
- **$1.0\text{ ms}$ 脉宽**：最大反向制动 / 左转打满；
- **$2.0\text{ ms}$ 脉宽**：最大正向前进 / 右转打满。

```c
/* modules/adas_nodes/actuator_pwm_node.c */
void set_servo_pulse(int i2c_fd, uint8_t channel, float normalized_val) {
    // 将 [-1.0, 1.0] 映射为 [1000us, 2000us] 脉宽
    float pulse_us = 1500.0f + normalized_val * 500.0f;
    uint16_t off_count = (uint16_t)(pulse_us * 4096.0f / 20000.0f);
    
    // 写入 PCA9685 寄存器
    pca9685_set_pwm(i2c_fd, channel, 0, off_count);
}
```

---

## 4. 真实上车配置：`pipeline_car.json`

在真车上部署时，将宿主配置指定为真车专用的 `config/pipeline_car.json`：

```json
{
  "name": "car_real_hardware_pipeline",
  "services": [
    {
      "name": "gps_driver",
      "library": "libgps_driver_node.so",
      "params": { "serial_port": "/dev/ttyUSB0", "baudrate": 115200 }
    },
    {
      "name": "actuator",
      "library": "libactuator_node.so",
      "params": {
        "can_interface": "can0",
        "can_throttle_id": 256,
        "can_steering_id": 257,
        "enable": true
      }
    }
  ]
}
```

---

## 5. 工业级安全上车守则

### 守则 1：硬件物理急停开关（E-Stop）直连断电
- 任何基于软件的急停逻辑都可能因操作系统崩溃而失效。真车底盘必须串联一个**常闭物理断电急停蘑菇头按键**，在紧急情况下直接切断动力电池给电机的供电回路。

### 守则 2：CAN 总线超时自动锁死保护（Heartbeat Timeout）
- ESC 与底层转向控制器必须内置独立的硬件定时器。若连续超过 $100\text{ ms}$ 未收到来自工控机的有效 CAN 控制帧，底层硬件必须**自动执行刹车并回正方向盘**，防止工控机死机时车辆保持油门飞车。

---

*全书完结。祝您在 KunAutoDrive 的高性能自动驾驶与仿真开发之旅中取得丰硕成果！*
