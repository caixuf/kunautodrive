# 第 05 章：录下来，再原样放一遍

算法迭代里最怕听到的一句话是「昨天还好的」。如果手边没有一份录下来的现场数据，一次
偶发的误刹车或者漏检就只能靠猜，改完也无从验证。这就是数据录制与回放存在的意义：它让
「可复现」变成一件自动的事。

这一章讲 KunAutoDrive 的持久化子系统怎么做到这一点：自研的 Bag v2 时序二进制格式负责快，
兼容 MCAP 标准负责和外部工具对接，两者共用同一套回放引擎。

## 一条数据从总线到磁盘，再回到总线

整条链路是单向的。运行时数据流经 BagWriter 异步落盘，再由 BagReader 按原时序重新灌回
`MessageBus`。中间那层 RingBuffer 把录制和仿真循环解耦，落盘走的是批量流式 I/O：

```
  ┌─────────────────────────────────────────────────────────────┐
  │                    运行时数据流 (Dataflow)                  │
  │  sensor/lidar (10Hz)  │  sensor/gps (20Hz)  │  control/cmd  │
  └──────────────┬──────────────────┬──────────────────┬────────┘
                 │                  │                  │
                 ▼                  ▼                  ▼
  ┌─────────────────────────────────────────────────────────────┐
  │             BagWriter 异步录制引擎 (后台 Worker 线程)       │
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
  │              BagReader 确定性离线回放 (Replay Engine)       │
  │     - 相对时延补偿 (us)   - 任意倍速播放 (0.5x~10x)         │
  │     - Topic 正则过滤     - 统一时钟注入 (Clock Service)     │
  └──────────────────────────────┬──────────────────────────────┘
                                 ▼
                     重新发布至 MessageBus
```

## out.bag 里到底存了什么

Bag v2 是冲着 SSD 顺序写入优化的，结构分三段：固定 64 字节的文件头、紧密排列的数据记录
流，以及尾部的索引区：

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
│   │     type_id(4B) | schema_ver(1B) | endian(1B) | timestamp_us(8B) |       │
│   │     topic_len(1B) | topic(N B) | data_size(4B) | data(N B)               │
│   ├── Record 1: ...                                                          │
│   └── Record N-1: ...                                                        │
├──────────────────────────────────────────────────────────────────────────────┤
│ [Index 尾部索引区: 加速 O(1) 随机 Seek 与统计]                               │
│   ├── entry_count: uint64_t                                                  │
│   ├── entries: [ topic(64B) | count(8B) | first_off(8B) | last_off(8B) ] × N │
│   └── crc32: uint32_t (全文件完整性校验)                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

三段各司其职：Header 记总量与索引位置，Records 是消息帧本身，尾部索引区把随机 Seek 和
按 Topic 统计降到 O(1)，最后再留一个 crc32 做整文件完整性校验。

## 顺手兼容 MCAP

除了轻量的自研格式，KunAutoDrive 在 `src/core/mcap_writer.c` 里把 MCAP 标准也实现了一遍：
Schema Chunk 记录 IDL 数据结构定义（Protobuf / JSON Schema），Channel Chunk 定义 Topic
名称与编码格式，Message Chunk 装高压缩比的时序载荷。好处很直接——录出来的 `.mcap` 不用
转换，拖进 Foxglove Studio 就能在浏览器里看 3D 轨迹、点云、时序曲线和视频流。

## 回放时怎么把时间对齐

回放最核心的挑战只有一句话：怎样让两个消息帧之间的间隔，和录制时完全吻合。

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

这里有个必须守住的前提：所有时延计算都只能用 `CLOCK_MONOTONIC` 单调时钟。真车运行或
回放跨越整点 NTP 对时时，系统时间可能往前或往后跳一下，如果用 `CLOCK_REALTIME` 做差值，
算出来的间隔就是错的。

## 录的时候怎么开，放的时候怎么用

### 编程接口

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

### 命令行工具

```bash
# 1. 查看 Bag 文件元数据与 Topic 统计
flowctl bag info out.bag

# 2. 检查 Bag 完整性并校验 CRC
./build/bin/bag_check out.bag

# 3. MCAP 文件离线回放
./build/bin/mcap_replay run.mcap --speed 1.5 --topics sensor/lidar,fusion/pose
```

## 一次断电事故

有次测试机在录制途中被直接断电。重启后 `out.bag` 能打开，但 `flowctl bag info` 显示
`msg_count` 是 0，整段录制看起来像空的。

查下来问题出在 Header 的回填时机。顺序写入过程中，`msg_count` 和 `duration_us` 是在内存里
动态累加的，只有走到 `bag_writer_close()` 时才会 `fseek(0)` 把它们写回文件头。进程被强杀
或断电，这一步就没机会执行，文件头里留下的还是初始的 0。

改法是让读的一端别那么依赖 Header：`BagReader` 打开文件时做一次向前容错扫描，如果发现
`index_offset == 0`，就从头顺序扫一遍记录，在线把索引重建出来。这样即便文件没被正常收尾，
里面的数据依然是完整的。
