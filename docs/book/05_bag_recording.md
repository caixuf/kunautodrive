# 第 05 章：数据持久化引擎（Bag v2 与 MCAP 规范）

> **本章导读**：
> 在自动驾驶与机器人系统的全生命周期中，“可复现性（Reproducibility）”是算法迭代与责任认定的第一生命线。如果无法在事后毫秒级精确重演现场传感器数据，那么任何偶发性事故（如 Corner Case 误刹车或漏检）都将沦为无法定位的玄学悬案。
>
> FlowEngine 构建了完备的数据持久化子系统：不仅拥有自研的 **Bag v2 高性能时序二进制文件格式**，还全面兼容国际标准的 **MCAP 规范（开放容器格式）**，无缝对接 Foxglove Studio 与离线回放评估管道。

---

## 1. 数据持久化架构总览

```
  ┌─────────────────────────────────────────────────────────────┐
  │                    运行时数据流 (Dataflow)                   │
  │  sensor/lidar (10Hz)  │  sensor/gps (20Hz)  │  control/cmd  │
  └──────────────┬──────────────────┬──────────────────┬────────┘
                 │                  │                  │
                 ▼                  ▼                  ▼
  ┌─────────────────────────────────────────────────────────────┐
  │             BagWriter 异步录制引擎 (后台 Worker 线程)         │
  │     RingBuffer 异步解耦 ──► 批量流式落盘 (POSIX I/O)        │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
                 ┌──────────────────────────────┐
                 │     out.bag (v2 二进制)      │
                 │      或 recording.mcap       │
                 └──────────────┬───────────────┘
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │              BagReader 确定性离线回放 (Replay Engine)        │
  │     - 相对时延补偿 (us)   - 任意倍速播放 (0.5x~10x)         │
  │     - Topic 正则过滤     - 统一时钟注入 (Clock Service)     │
  └──────────────────────────────┬──────────────────────────────┘
                                 ▼
                     重新发布至 MessageBus
```

---

## 2. Bag v2 二进制存储规范深入推导

FlowEngine 自研的 Bag v2 格式针对 SSD 顺序写入进行了深度优化，包含**文件头（Header）、数据记录流（Records）与尾部索引区（Index Chunk）**：

```
Bag v2 文件二进制布局：
┌──────────────────────────────────────────────────────────────────────────────┐
│ [Header 区: 64 字节固定]                                                     │
│   ├── magic: char[4] = "FLB_" (0x46, 0x4C, 0x42, 0x5F)                       │
│   ├── version: uint32_t = 2                                                  │
│   ├── msg_count: uint64_t (记录总数)                                         │
│   ├── duration_us: uint64_t (持续总微秒数)                                   │
│   ├── index_offset: uint64_t (尾部索引区的文件起始偏移量)                    │
│   └── _reserved: uint8_t[32] (保留对齐)                                      │
├──────────────────────────────────────────────────────────────────────────────┤
│ [Records 流: 紧密排列的消息帧]                                               │
│   ┌── Record 0:                                                              │
│   │     type_id(4B) | schema_ver(1B) | endian(1B) | timestamp_us(8B) |      │
│   │     topic_len(1B) | topic(N B) | data_size(4B) | data(N B)               │
│   ├── Record 1: ...                                                          │
│   └── Record N-1: ...                                                        │
├──────────────────────────────────────────────────────────────────────────────┤
│ [Index 尾部索引区: 加速 O(1) 随机 Seek 与统计]                               │
│   ├── entry_count: uint64_t                                                  │
│   ├── entries: [ topic(64B) | count(8B) | first_off(8B) | last_off(8B) ] × N│
│   └── crc32: uint32_t (全文件完整性校验)                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. MCAP 标准格式支持（Foxglove 生态打通）

除了轻量的自研 Bag v2，FlowEngine 在 `src/core/mcap_writer.c` 中完整实现了 **MCAP（Open Source Container Format for Multimodal Data）** 标准：
- **Schema Chunk**：记录 IDL 数据结构定义（Protobuf / JSON Schema）；
- **Channel Chunk**：定义 Topic 名称与编码格式；
- **Message Chunk**：高压缩比的数据时序载荷；
- **可视化利器**：录制出的 `.mcap` 文件无需转换，直接拖拽至 **Foxglove Studio** 即可在浏览器中可视化 3D 轨迹、点云、时序曲线与视频流。

---

## 4. 高精度时钟回放算法（Timing Alignment）

回放录制数据的核心挑战在于：**如何保证两个消息帧之间的时间间隔与录制时 100% 吻合？**

```c
/* BagReader 高精度回放循环逻辑 */
int bag_reader_play(BagReader* r, MessageBus* bus, float speed) {
    uint64_t prev_record_ts = 0;
    uint64_t play_start_wall = clock_now_monotonic_wall_us();
    
    while (bag_has_next(r)) {
        Message msg = bag_read_next(r);
        
        if (prev_record_ts > 0 && speed > 0.0f) {
            // 计算两帧之间的录制时间差
            int64_t delta_record_us = (int64_t)(msg.timestamp_us - prev_record_ts);
            
            // 按倍速缩放
            int64_t target_delay_us = (int64_t)(delta_record_us / speed);
            
            // 精确等待（自适应补偿调用开销）
            if (target_delay_us > 50) {
                usleep(target_delay_us);
            }
        }
        
        prev_record_ts = msg.timestamp_us;
        
        // 驱动统一时钟（仿真时钟注入）
        clock_set_sim_time(msg.timestamp_us);
        
        // 重新发布到总线
        message_bus_publish(bus, msg.topic, msg.sender, msg.data, msg.data_size);
    }
    return 0;
}
```

---

## 5. 核心 API 与 CLI 操作

### 5.1 编程接口
```c
#include "bag.h"

/* 1. 自动挂载总线录制 */
BagWriter* writer = bag_writer_open("simulation_run.bag");
bag_writer_attach(writer, bus); // 异步启动后台线程，自动录制全部 Topic

/* ... 仿真运行 ... */
bag_writer_close(writer); // 自动回填 Header 与 Index Chunk

/* 2. 离线过滤回放 */
BagReader* reader = bag_reader_open("simulation_run.bag");
// 仅回放激光雷达与 GPS，以 2.0 倍速执行
bag_reader_play_filtered(reader, bus, 2.0f, "sensor/*", 0, 0);
bag_reader_close(reader);
```

### 5.2 命令行工具（`flowctl bag`）
```bash
# 1. 查看 Bag 文件元数据与 Topic 统计
flowctl bag info out.bag

# 2. 检查 Bag 完整性并校验 CRC
./build/bin/bag_check out.bag

# 3. MCAP 文件离线回放
./build/bin/mcap_replay run.mcap --speed 1.5 --topics sensor/lidar,fusion/pose
```

---

## 6. 工业级避坑指南

### 避坑 1：进程被杀死时的“头部未回填（Unfinalized Header）”灾难
- **问题**：在顺序写入过程中，`msg_count` 和 `duration_us` 随着录制动态增加，只有在 `bag_writer_close()` 时才会 `fseek(0)` 回填 Header。如果车辆断电或进程被强杀，Header 中的 `msg_count` 仍为 0。
- **FlowEngine 解决方案**：`BagReader` 在打开文件时执行**向前容错扫描**——若发现 `index_offset == 0`，则自动从头顺序扫描记录并在线重建索引，实现零损坏恢复。

### 避坑 2：回放时千万不要使用 `CLOCK_REALTIME` 做差值
- 车辆真车运行或回放跨越整点 NTP 对时时，系统时间可能向前或向后跳跃。所有时延计算必须严格依赖 `CLOCK_MONOTONIC` 单调时钟。

---

*下一章预告：第 06 章将深入剖析自动驾驶系统的“时间基准”——统一时钟服务（Clock Service）与 μs 时间戳语义。*
