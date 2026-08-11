# Skill 05 - 数据录制与回放（Bag）

## 核心思想

Bag 模块将运行时的消息流录制到文件，之后可以**离线回放**，让系统以为数据仍然来自真实传感器。这是机器人/自动驾驶领域的标准调试手段（类比 ROS bag）。

## 核心价值

- **复现线上问题** — 将现场数据录制下来，回到办公室离线调试
- **算法迭代** — 同一份数据反复跑不同版本的算法，比较效果
- **回归测试** — 将典型场景的 bag 作为测试用例，自动化验证
- **数据标注** — 离线慢速回放，方便人工标注

## 文件格式

```
┌──────────────────────────────┐
│   Bag File Header            │  魔数 + 版本 + 时间戳 + 话题索引偏移
├──────────────────────────────┤
│   Record 0                   │  时间戳 | topic_len | topic | data_len | data
├──────────────────────────────┤
│   Record 1                   │
│   ...                        │
├──────────────────────────────┤
│   Topic Index                │  话题名 → 文件偏移列表（可选，加速随机访问）
└──────────────────────────────┘
```

## 录制

```c
#include "bag.h"

// 最简单：直接挂到总线，自动录制所有 topic
BagWriter* writer = bag_writer_open("flight_20250722.bag");
bag_writer_attach(writer, bus);

// 或手动写入一条 Message
Message msg = {0};
snprintf(msg.topic, sizeof(msg.topic), "%s", "sensor/gps");
snprintf(msg.sender, sizeof(msg.sender), "%s", "gps_drv");
msg.timestamp_us = clock_now_us();
msg.type = MSG_TYPE_PUBLISH;
msg.data_size = payload_size;
memcpy(msg.data, payload, payload_size);
bag_writer_write(writer, &msg);

// 程序退出时关闭
bag_writer_close(writer);
```

## 回放

```c
#include "bag.h"
#include "clock_service.h"

clock_set_sim_mode(true);  // 当前进程切到 replay 驱动的逻辑时钟

BagReader* reader = bag_reader_open("flight_20250722.bag");

// 回放：按录制时间顺序投递消息到本地总线，1× 速度
bag_reader_play(reader, bus, 1.0f);

// 加速回放（3× 速度）
bag_reader_play(reader, bus, 3.0f);

// 进阶：精确 topic + 时间窗口（相对 bag 起点）+ loop/停止条件
BagReplayOptions opt = {0};
opt.bus = bus;
opt.speed = 0.5;                  // 半速
opt.topic_filter = "sensor/gps";  // 精确 topic
opt.start_offset_us = 2 * 1000000ULL;
opt.end_offset_us   = 5 * 1000000ULL;
opt.drive_sim_clock = true;
uint64_t played = 0;
bag_reader_play_with_options(reader, &opt, &played);

bag_reader_close(reader);
clock_set_sim_mode(false);
```

## 与统一时钟的配合

回放时必须让系统时钟与 bag 内的时间戳同步，否则依赖时间的算法会出错：

```c
clock_set_sim_mode(true);
// bag_reader_play_with_options(... drive_sim_clock=true) 会在每条消息前
// 自动 clock_set_sim_time(recorded_ts)
uint64_t now = clock_now_us();  // 算法代码无需感知当前是 live 还是 replay
```

## CLI / 端到端回放

```bash
# 查看元数据
./build/bin/flowctl bag info logs/demo.bag

# 启动单进程 pipeline，本地总线回放
./build/bin/flowctl bag play logs/demo.bag --config config/pipeline.json

# 启动多进程 pipeline，父 launcher 作为 IPC 注入器
./build/bin/flowctl bag play logs/demo.bag --config config/pipeline.json --multi

# 2× 倍速、只回放一个 topic、截取 2s~8s 片段、循环
./build/bin/flowctl bag play logs/demo.bag \
  --rate 2.0 --topic sensor/lidar --start 2 --end 8 --loop
```

限制：

- `flowctl bag play` 会**显式启动** replay pipeline；不支持后挂到已运行的 single-process launcher。
- `--multi` 走 IPC 注入，消息速率/顺序可重放，但子进程时钟仍是各自墙钟；只有单进程 replay 会驱动统一 sim clock。
- 同时间戳消息按 bag 文件顺序发出；下游若跨 topic 异步处理，执行先后仍受现有 MessageBus / IPC 调度影响。

## 注意事项

1. **时间戳精度** — 建议使用 `CLOCK_MONOTONIC_RAW`（纳秒级），避免系统时间跳变影响录制。
2. **文件大小** — 高频话题（如 100Hz 图像）会迅速产生大文件，录制前估算磁盘空间。
3. **话题过滤** — 只录制需要的话题，减小文件体积。
4. **索引** — 对长 bag 文件建立话题索引，支持按时间随机跳转。
5. **压缩** — 可选对数据块 LZ4 压缩，在 CPU 可接受范围内大幅减小文件体积。

## 参考文件

- `include/bag.h` — API 定义
- `src/core/bag.c` — 实现
- `src/flowctl.c` — `flowctl bag info|check|play`
- `src/flow_launcher.c` — replay mode + live source auto-skip
- `include/clock_service.h` — 统一时钟 API
