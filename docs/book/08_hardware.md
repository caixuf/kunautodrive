# 卷八 · 第八课：真车与硬件——跨过那道鸿沟

## 本章问题

你的车在仿真里学会超车了（卷三到卷七）。现在把它搬上**真车**。

听起来很简单：同一套代码，同一个参数，直接跑。但你会很快撞上一堵墙——**仿真里调好
的参数，到真车上全不对**：车转弯转不到位、速度跟踪不上、传感器全是噪声。这就是
**sim2real（仿真到现实）鸿沟**。

这一章讲：怎么把整套系统搬上真车，以及这道鸿沟到底是什么。

## 你自己的答案：同一套参数直接上真车

最朴素的想法：仿真里 `lat_kp=0.5` 开得很好，真车上也用 0.5。

但你马上会发现三个问题：

1. **转向不对**：仿真的 `steer` 是「理想的前轮转角」，真车的 `steer` 要经过转向机构、
   轮胎的滞后和非线性——同样的 0.5 增益，真车上要么打不够（转不过来），要么打过头
   （蛇形）。
2. **速度不对**：仿真的油门 -> 加速度是干净的公式，真车的油门 -> 加速度受路面、坡度、
   电池电压影响。
3. **位置不对**：仿真里定位是上帝视角（真值），真车上 GPS 会漂、IMU 会飘。

**结论：仿真和真车必须当成两条独立的路径来对待。** 仿真的价值是「算法对不对」，真车
的价值是「参数准不准」——两件事不能混。

## 真正的方案 1：双 pipeline

项目用**两套配置**区分两种运行方式：

| | `pipeline.json`（仿真） | `pipeline_car.json`（真车） |
|--|------------------------|----------------------------|
| 世界来源 | flowsim 仿真世界 | 真实传感器（GPS/IMU/LiDAR） |
| 执行器 | 仿真物理模型 | CAN/PWM 执行器 |
| 参数 | 仿真标定 | 真车标定（差异可能很大） |
| 用途 | 算法开发/回归 | 实车部署 |

**为什么不是一套配置 + 开关？** 因为两套东西的差异太大：传感器类型不同、执行器不同、
参数不同、甚至节点不同（真车没有 flowsim，多了一堆驱动节点）。硬塞进一套配置，就是
一个巨大的 `if (real_car)` 分支地狱。**两套独立的配置，各自干净。**

### pipeline_car.json 的节点拓扑

```
gps_driver -> fusion -> perception -> planning -> control -> actuator_pwm
imu_driver -> fusion                                    |
lidar_driver -> perception                             |
                                                        v
                                                  servo + ESC
```

对比仿真拓扑，真车：
- 去掉了 `flowsim`、`sensor_model`（没有仿真世界了）
- 增加了 `gps_driver`、`imu_driver`、`lidar_driver`（真实传感器）
- 增加了 `actuator_pwm`（PCA9685 PWM 输出）或 `actuator_node`（SocketCAN 输出）

## 真正的方案 2：部署五步（每一步都是血泪）

真车部署不是「编译好拷过去」这么简单，它有固定的五步：

```
1. Compile       主框架 + node plugins
2. Permissions   Serial udev rules / CAN SPI overlay / USB permissions
3. Configure     pipeline_car.json pointing to real device paths
4. Dry-run       No hardware connected, verify software-side correctness
5. Live test     Hardware connected, watchdog test
```

### 第 1 步：编译

```bash
cmake -B build-car -DCMAKE_BUILD_TYPE=Release
cmake --build build-car -j$(nproc)
# 产出：build-car/bin/flow_launcher, build-car/lib/*.so
```

### 第 2 步：配置权限（最容易被忽略）

`/dev/ttyUSB*` 默认只有 root 可读。你的车「不动了」，查了半天代码没问题——结果是
串口权限没配。

```bash
# GPS 串口权限
sudo usermod -aG dialout $USER
# 或 udev 规则（推荐）
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"' | \
    sudo tee /etc/udev/rules.d/99-gps.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**车不动，先查权限，再查代码。** 这是真车调试的第一条铁律。

### 第 3 步：改配置

`pipeline_car.json` 里的设备路径必须指向真实设备：

```json
{
  "name": "gps_driver",
  "params": "{\"device\":\"/dev/ttyUSB0\",\"baudrate\":115200,\"dry_run\":0}"
}
```

### 第 4 步：dry-run（最聪明的一步）

不接硬件，让驱动打印「指令 -> 执行量」的映射，确认软件侧正确：

```bash
# 设置 dry_run=1（驱动打印指令但不实际输出）
# 启动后看日志：
#   [dry-run] ESC: target=1500us current=1500us
#   [dry-run] Servo: target=1500us current=1500us
# 确认「油门 30% -> PWM 1800us」对不对，再接真执行器。
```

**把「软件 bug」和「硬件问题」隔开——dry-run 通过 = 软件没问题，剩下的才是硬件的事。**

### 第 5 步：看门狗测试（安全底线）

如果控制节点失联（比如进程崩溃），执行器必须**自动回安全位**（油门归零、转向回中）。

测试方法：
```bash
# 启动真车（dry-run 模式先验证）
# 然后暂停控制节点
kill -STOP <control_pid>
# 3 秒后检查：执行器应回中位（1500us）
# 10 秒后检查：ESC 应切断动力
```

**没有看门狗的车，一旦控制进程死掉，就是失控的车。** 这是不可协商的安全底线。

## 硬件驱动节点详解

### actuator_pwm_node（RC 小车）

PCA9685 I2C PWM 芯片，16 通道。车用 2 通道：
- Channel 0: ESC（电子调速器）—— 控制油门/刹车
- Channel 1: Servo（舵机）—— 控制转向

```c
// ESC 标定：PWM 范围映射
//   1000us = 全刹车，1500us = 中位，2000us = 全油门
// Servo 标定：
//   1000us = 左满舵，1500us = 直行，2000us = 右满舵
```

### actuator_node（SocketCAN 真车）

通过 SocketCAN 协议发送控制指令到 CAN 总线：

```bash
# CAN 接口配置（MCP2515 SPI CAN 控制器）
sudo ip link set can0 up type can bitrate 500000
# 或 USB CAN（PCAN/Vector）
sudo ip link set can0 up
```

CAN 帧格式：`can_send(can_id, data, len)`，具体 ID 和编码取决于车型 ECU 协议。

### GPS 驱动

NMEA 0183 协议解析。支持的语句：
- `$GPGGA`：定位信息（经纬度、精度、海拔）
- `$GPRMC`：推荐最小定位信息（速度、航向）

```bash
# 配置
#   device: /dev/ttyUSB0
#   baudrate: 115200
#   dry_run: 0 (真车) / 1 (测试)
```

### IMU 驱动

六轴 IMU（加速度计 + 陀螺仪），输出 roll/pitch/yaw + 线性加速度。
用于融合节点的角速度观测（EKF 的 omega 输入）。

### LiDAR 驱动

2D LiDAR（如 RPLIDAR A1/A2），串口协议。输出角度-距离扫描数据，
由 `perception_node` 做 DBSCAN 聚类得到障碍物列表。

## 定位的接力策略

真车最麻烦的问题之一：**GPS 会漂**。仿真里定位是上帝视角，真车上 GPS 一进隧道就没了。

定位有一套「接力」策略：

```
GPS available --> GPS positioning (absolute, but noisy)
GPS lost       --> Dead reckoning: extrapolate with velocity/heading
                    (absolute position drifts over time -- integration error)
SLAM converged --> Real SLAM (lidar/visual, high accuracy)
                    (converged=1 means SLAM is reliable, replaces drifted DR)
```

**航位推算**是 GPS 丢了的应急：有速度、有朝向，就能推出「我大概在哪」。
但它是开环外推——误差随时间累积。所以有 SLAM 时优先用 SLAM（闭环，不漂移）。

**这又是一个「方向感」问题**：车必须知道自己在哪、往哪开。回想卷三那个「用世界坐标
判断威胁」的 bug——**在真车上，这类问题更致命，因为 GPS 本身就会错。**

## FAST-LIO2 SLAM 集成

项目支持 FAST-LIO2（LiDAR-惯性里程计）作为高精度定位方案：

```
LiDAR 点云 + IMU 数据 --> FAST-LIO2 --> 位姿估计（6DOF）
                                        |
                                        v
                                  sensor/pose (Pose2D)
                                  converged=1/0
                                  covariance
```

`converged=1` 表示 SLAM 收敛，融合节点会用 SLAM 位姿替代 GPS/DR。

**集成注意事项**：
- FAST-LIO2 需要 PCL、Sophus、yaml-cpp 外部依赖
- 首次运行需要建立地图（mapping 模式），之后定位（localization 模式）
- 室内/隧道环境 SLAM 可能退化（特征不足）

## RC 小车——最便宜的真车

不是每个人都有台真车。项目用 **RC 小车（遥控车）** 做 L2 验证：录一段航点，车照着走。

```
1. Record waypoints (GPS serial, or replay from NMEA log)
2. Check / resample (remove dense/sparse irregularities)
3. Configure waypoint_follower (cruise speed, loop enabled)
4. Run
```

RC 小车用的是 PWM 执行器（PCA9685 芯片），比 CAN 简单得多——它让你在没有真车的情况下，
把「从仿真到实物」这条完整链路走一遍：权限、dry-run、看门狗、参数标定，一个不少。

**RC 小车硬件清单**（约 500 元）：
- 底盘：1/18 遥控车底盘（含 ESC + Servo）
- 计算：树莓派 4B（4GB）
- GPS：BN-220 USB GPS 模块
- IMU：MPU6050 I2C 模块
- PWM 驱动：PCA9685 I2C 模块
- 电源：2S LiPo 电池

## 调试检查清单

真车调试的完整检查顺序：

```
1. ls /dev/ttyUSB*        -- GPS/IMU 串口存在？
2. ls /dev/i2c-*          -- I2C 总线存在？
3. sudo ip -d link show can0  -- CAN 接口 up？
4. flowctl list           -- 节点都在跑？
5. flowctl param get control.steer_trim  -- 参数对？
6. kill -STOP <pid>       -- 看门狗 3s 回中？
7. dry-run 日志           -- PWM 映射正确？
8. GPS fix                -- 定位收敛？
9. SLAM converged         -- 高精度定位就绪？
10. 空旷地低速试跑        -- 首次实际运行
```

## 动手实践

```bash
# 1. 看两套配置的差异
diff config/pipeline.json config/pipeline_car.json

# 2. 模拟一次 dry-run
#    把执行器驱动设成 dry_run=1，启动后看日志

# 3. 看门狗测试（如果有 RC 小车）
kill -STOP <control_pid>  # 3s 后执行器应回中位
```

## 常见陷阱

1. **车不动先查权限**：/dev/ttyUSB* 默认只有 root 可读。
2. **dry-run 通过才接硬件**：把软件 bug 和硬件问题隔开。
3. **看门狗是安全底线**：没有看门狗 = 失控风险。
4. **GPS 进隧道就丢**：必须有 DR/SLAM 接力。
5. **真车参数必须重标**：仿真参数只是起点。

## 小结

这一章你学会了跨过 sim2real 鸿沟：

- **仿真和真车是两条独立路径**：仿真是「算法对不对」，真车是「参数准不准」——参数
  必须重标。
- **部署五步**：编译 -> 权限 -> 配置 -> **dry-run** -> 看门狗。权限最容易被忽略，dry-run
  把软硬件问题隔开，看门狗是安全底线。
- **定位有接力**：GPS -> 航位推算（会漂）-> SLAM（不漂）。车必须知道自己在哪。
- **RC 小车是最便宜的真车**：让你在没有真车时走完「仿真到实物」的全链路。

## 练习（选做）

1. **思考题**：为什么「横向控制」在仿真和真车上差异最大？提示：轮胎侧偏——仿真里
   有侧偏吗？（回看卷一，运动学模型假设了什么？）
2. **挑战**：GPS 丢了，航位推算开始漂。你怎么判断「漂得太远、不能再信了」？提示：
   你需要一个「我可能在哪」的不确定性估计——外推时间越长，不确定性越大。
3. **读配置**：对比 `pipeline.json` 和 `pipeline_car.json`，找出 3 处仿真有而真车没有
   的节点（或反过来）。想想为什么。

---

**下一卷预告**：你造完了一整套系统，学会了怎么让它学习、怎么搬上真车。最后一课，是
全书最重要的一课：**怎么证明它没坏**。卷九讲验证体系。
