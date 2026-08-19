# 10 — 数据融合框架

> 本教程覆盖 EKF 多传感器融合的架构、时间对齐、降级策略和性能指南。
> 对应源码：`modules/adas_nodes/fusion_node.cpp`、`core/fusion.c`。

## 1. 核心架构

```
                    ┌──────────┐
 sensor/lidar ─────┤          │
                    │  Fusion  │ ──── fusion/localization
 sensor/gps ───────┤  Node    │ ──── fusion/obstacles
 sensor/imu ───────┤          │ ──── perception/obstacles
                    └──────────┘
                         ↑
              MessageBuffer × N（每传感器独立环形缓冲）
```

**设计原则**：回调只 push 到 MessageBuffer（不阻塞总线分发），协程体按需消费。

## 2. 生产范式 — FlowCoroTask + MessageBuffer

项目生产融合节点（`fusion_node.cpp`）采用此范式：
回调仅 push 到 MessageBuffer，协程体用 `select_for` 等待输入 +
`message_buffer_find_nearest` 做时间对齐，再执行融合逻辑并 `transport_publish`。

```cpp
#include "fusion.h"
#include "coroutine_task.h"
#include "adas_msgs_gen.h"

class SensorFusion : public FlowCoroTask {
public:
    SensorFusion(MessageBus* bus, Transport* t,
                 MessageBuffer* lidar_buf, MessageBuffer* gps_buf,
                 MessageBuffer* imu_buf)
        : FlowCoroTask(bus), transport_(t),
          lidar_buf_(lidar_buf), gps_buf_(gps_buf), imu_buf_(imu_buf) {}

protected:
    Task run() override {
        while (!should_stop()) {
            // 等任一输入到达或 100ms 超时（watchdog 防卡死）
            co_await select_for({"sensor/lidar", "sensor/gps", "sensor/imu"}, 100000);
            if (should_stop()) break;

            // ── 1. 取主输入最新帧 ──
            const Message* lidar_msg = message_buffer_latest(lidar_buf_);
            if (!lidar_msg) continue;
            uint64_t ref_ts = lidar_msg->timestamp_us;

            // ── 2. 时间对齐：找各传感器时间窗口内最近帧 ──
            const Message* gps_msg = message_buffer_find_nearest(gps_buf_, ref_ts, 50000);
            const Message* imu_msg = message_buffer_find_nearest(imu_buf_, ref_ts, 20000);

            const LidarFrame* lidar = (const LidarFrame*)lidar_msg->data;

            // ── 3. 降级策略：传感器丢失时自动降级 ──
            if (gps_msg && imu_msg) {
                // 全量融合：LiDAR 位置 + GPS 速度/航向 + IMU 角速度
                const GpsFrame* gps = (const GpsFrame*)gps_msg->data;
                const ImuFrame* imu = (const ImuFrame*)imu_msg->data;
                ekf_fusion_update_full(lidar, gps, imu, &state_);
            } else if (gps_msg) {
                // 降级 1：无 IMU，用 GPS 速度差分替代角速度
                const GpsFrame* gps = (const GpsFrame*)gps_msg->data;
                ekf_fusion_update_lidar_gps(lidar, gps, &state_);
            } else {
                // 降级 2：仅 LiDAR，纯位置观测
                ekf_fusion_update_lidar_only(lidar, &state_);
            }

            // ── 4. 输出 ──
            LocalizationOut out = build_output(&state_);
            transport_publish(transport_, "fusion/localization",
                              (uint8_t*)&out, sizeof(out));
        }
    }

private:
    Transport* transport_;
    MessageBuffer* lidar_buf_;
    MessageBuffer* gps_buf_;
    MessageBuffer* imu_buf_;
    EkfState state_{};
};

// 订阅回调：仅 push 到 buf，不做计算
static void on_lidar(const Message* m, void*) { message_buffer_push(g.lidar_buf, m); }
static void on_gps(const Message* m, void*)   { message_buffer_push(g.gps_buf,   m); }
static void on_imu(const Message* m, void*)    { message_buffer_push(g.imu_buf,    m); }
```

## 3. 时间对齐算法

```
1. select_for 等待任一输入到达（或超时 watchdog）
2. 取主输入（LiDAR）最新帧的 timestamp_us 作 reference_ts
3. 对其他输入：
   - GPS：message_buffer_find_nearest(gps_buf, ref_ts, 50000) → 50ms 窗口
   - IMU：message_buffer_find_nearest(imu_buf,  ref_ts, 20000) → 20ms 窗口
4. 在窗口内视为有效，参与融合；超窗口则丢弃（该传感器降级）
```

**窗口大小选择**：
- GPS（10Hz）：50ms 窗口 = ±0.5 帧容差
- IMU（100Hz）：20ms 窗口 = ±2 帧容差（IMU 频率高，窗口可更紧）
- LiDAR（10-20Hz）：作为主输入，不需窗口

## 4. EKF 状态模型

融合节点使用 5 维扩展卡尔曼滤波器：

```
状态向量 x = [x, y, v, θ, ω]
              │  │  │  │  └── yaw rate (rad/s)
              │  │  │  └───── heading (rad)
              │  │  └──────── speed (m/s)
              │  └─────────── y position (m)
              └────────────── x position (m)
```

**观测模型**：
| 传感器 | 观测量 | 维度 | 噪声特性 |
|--------|--------|------|---------|
| LiDAR | (x, y) 位置 | 2D | σ=0.08m（DBSCAN 聚类精度） |
| GPS | (vx, vy) 速度 + 航向 | 3D | σ=0.5m/s（民用 GPS） |
| IMU | yaw_rate 角速度 | 1D | σ=0.01rad/s（MEMS 陀螺仪） |

**预测步**：自行车运动学模型（`v, θ, ω` → `Δx = v·cos(θ)·dt, Δy = v·sin(θ)·dt`）

## 5. 传感器降级策略

| 场景 | 丢失传感器 | 降级方案 | 精度影响 |
|------|-----------|---------|---------|
| GPS 丢失（隧道/室内） | GPS | SLAM Pose2D 接管（`sensor/pose`，`converged=1` 且协方差<100 时启用） | 中 |
| LiDAR 丢失（遮挡） | LiDAR | GPS 速度积分 + IMU 航向积分 | 低 |
| IMU 丢失 | IMU | GPS 速度差分估算 yaw_rate | 低 |
| GPS+IMU 同时丢失 | GPS+IMU | LiDAR 纯位置观测 + 运动学预测 | 中 |
| 全部丢失 | 全部 | 纯预测（匀速模型），持续 >5s 触发告警 | 无（开环） |

**判断逻辑**（`fusion_node.cpp`）：
```cpp
if (gps_msg && imu_msg) {
    ekf_fusion_update_full(lidar, gps, imu, &state_);    // 全量
} else if (gps_msg) {
    ekf_fusion_update_lidar_gps(lidar, gps, &state_);    // 无 IMU
} else {
    ekf_fusion_update_lidar_only(lidar, &state_);         // 纯 LiDAR
}
```

## 6. 何时用 MessageBuffer vs 直接回调

| 场景 | 选择 | 原因 |
|------|------|------|
| 融合节点（需时间对齐） | **MessageBuffer** | 多源异步数据必须缓存后按时间戳对齐 |
| 单输入反应式处理（如红绿灯识别） | **直接回调** | 无需缓存，拿到即处理 |
| 高频输入降采样（如 100Hz IMU → 10Hz 融合） | **MessageBuffer** | `latest()` 天然取最新帧，中间帧自动丢弃 |
| 需要历史回溯（如轨迹回放） | **MessageBuffer** | 环形缓冲保留最近 N 帧 |

## 7. API 速查

| 函数 | 用途 |
|------|------|
| `message_buffer_create(capacity)` | 创建传感器环形缓冲（capacity=最大帧数） |
| `message_buffer_push(buf, msg)` | 回调里推入消息（值拷贝，线程安全） |
| `message_buffer_find_nearest(buf, ts, max_delta)` | 时间戳最近邻查找（时间对齐核心） |
| `message_buffer_latest(buf)` | 取最新消息（降采样用） |
| `message_buffer_destroy(buf)` | 销毁缓冲 |
| `FlowCoroTask::run()` | 重写协程体，`co_await select_for` 等输入 |
| `FlowCoroTask::select_for(topics, timeout_us)` | 等任一 topic 消息或超时 |
| `FlowCoroTask::should_stop()` | 检查优雅停止信号 |

## 8. 性能指南

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 融合延迟（LiDAR 到定位输出） | < 50ms | 含时间对齐 + EKF update + publish |
| MessageBuffer 容量 | 8-16 帧 | 足够覆盖传感器异步；过大浪费内存 |
| EKF 矩阵运算 | < 1ms | 5×5 矩阵，无瓶颈 |
| 回调 push 延迟 | < 0.1ms | 值拷贝，不阻塞总线 |

**常见瓶颈**：
1. **DBSCAN 聚类**（LiDAR 处理）：点云 >10k 点时耗时增加 → 降采样或分区聚类
2. **GPS 串口读取**：NMEA 解析在回调线程 → 确保波特率匹配（115200）
3. **IMU 数据堆积**：100Hz 高频 → MessageBuffer capacity 需 ≥16

## 9. 历史 API（不推荐新代码使用）

`core/fusion.c` 另提供 FusionNode C API（事件驱动时间对齐模型）。
生产代码已迁移到上述 FlowCoroTask 范式，该 API 保留供参考。
新融合节点请参照 `modules/adas_nodes/fusion_node.cpp`。
