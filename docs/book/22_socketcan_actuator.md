# 附录 A：把软件接到真车的最后一公里

仿真的终点是物理世界的那辆车。不管是 1:10 的 RC 小车（树莓派或 Jetson Orin Nano 底盘），还是乘用车、商用车的线控底盘，控制指令最终都得从软件变成电气信号，经 CAN 总线或 PWM 脉宽调制发给电调（ESC）和转向舵机或线控转向机（EPS）。这份附录讲的就是从 KunAutoDrive 软件流水线到硬件接口这最后一公里：Linux SocketCAN 驱动、MCP2515 这类 SPI-CAN 模块怎么配、PCA9685 怎么驱动舵机，以及真车上那几条不能破的安全约定。

## 先看整体接线

```
  ┌─────────────────────────────────────────────────────────────┐
  │         KunAutoDrive 软件流水线 (IPC / MessageBus)          │
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
  │ 线控底盘 / 乘用车 CAN 网络    ││ PCA9685 16路 PWM 驱动板    │
  │ (500kbps 差分信号 CAN_H/L)   ││ (50Hz 周期, 1.0~2.0ms 脉宽) │
  └──────────────┬───────────────┘└─────────────┬───────────────┘
                 │                              │
                 ▼                              ▼
     [真实线控转向 EPS / 刹车]           [ESC 电调油门 / 转向舵机]
```

## 在 Linux 上和 CAN 总线说话

### SocketCAN 是什么

SocketCAN 是 Linux 内核原生的 CAN 总线抽象层。它把 CAN 控制器虚拟化成标准的网络设备（比如 `can0`、`vcan0`），应用层用熟悉的 POSIX Socket API（`socket(PF_CAN, SOCK_RAW, CAN_RAW)`）收发，手感和收 UDP/IP 包差不多。

### 在树莓派 / Jetson 上把它打开（以 MCP2515 为例）

先在树莓派的 `/boot/firmware/config.txt` 里打开 SPI 和 CAN 覆盖层：

```ini
dtparam=spi=on
dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
```

再按车盘的波特率把接口拉起来，汽车底盘一般是 500kbps：

```bash
# 配置 500kbps 速率并拉起接口
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up

# 验证收发数据 (使用 can-utils)
candump can0
cansend can0 100#0102030405060708
```

### 在 C 里把控制量打成 CAN 报文（`actuator_node.c`）

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

## 用 PCA9685 驱动 RC 小车的舵机

在 1:10 的遥控模型小车上，电调和舵机收的是标准 RC PWM 脉冲，频率 $50\text{ Hz}$、周期 $20\text{ ms}$，脉宽落在下面几个点上：$1.5\text{ ms}$ 是中立，也就是停车、方向居中；$1.0\text{ ms}$ 对应最大反向制动或左转打满；$2.0\text{ ms}$ 对应最大正向前进或右转打满。

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

## 真车上的宿主配置

真车部署时，把宿主配置换成真车专用的 `config/pipeline_car.json`：

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

## 上车前必须守住的两条

第一条是关于急停的。任何纯软件的急停逻辑，都可能随着操作系统一起崩掉，所以真车底盘上要串一个常闭的物理断电急停开关，紧急时能直接切掉动力电池给电机的供电回路。

第二条是通信超时怎么办。ESC 和底层转向控制器里要各自带一个独立的硬件定时器，一旦连续超过 $100\text{ ms}$ 没收到工控机发来的有效 CAN 帧，硬件就得自己刹车并回正方向盘，免得工控机死机时车还闷着头往前冲。
