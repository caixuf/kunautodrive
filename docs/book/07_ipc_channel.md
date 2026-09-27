# 第 07 章：跨进程搬数据——为什么不能只用 socket

> **v2 范式章节**（2026-09 整治后保留）。本章保留低行号密度、真技术书
> 风格，参照 `docs/book/README.md` 写作风格约束与 `docs/book/09_discovery.md`
> 范式示范。v1 行号清单版本归档于 `docs/_archive/book_v1/04_ipc_channel.md`。

这一章在第一卷　微内核与系统编程。
上一篇是节点间总线的零拷贝消息池（见第 05 章）和那条线上的字节意义
（见第 06 章）。
本篇把这两者推到跨进程场景——`--multi` 模式下节点 fork+exec 成独立
进程，每进程的 `message_bus` 实例不共享，需要一条额外的传输层把它们
连起来。

读者也许会想：直接用 TCP / Unix socket 就行了，何必单独写一层？
理由很直接：

- **延迟**。一个 topic 一条消息，TCP 在控制环场景下要走 syscall 三次
  （send / recv / epoll），单条消息往返通常 50~150 µs。共享内存 memcpy
  是 1~5 µs，差一个量级。
- **广播**。一个 `perception/obstacles` 多订阅者（control、safety、
  monitor）时，TCP 要么开多 socket 要么 mDNS 转发，每个订阅者一份独立
  bytes-on-wire。共享内存允许所有订阅者读同一份字节——只付一次内存
  写代价。
- **零拷贝**。PCIe 直通场景下，shared memory 的 payload 指针就是序列化
  字节流的物理地址，省掉一次入队/出队的拷贝。

代价是要自己处理互斥、唤醒、版本兼容、清理——这就是 `ipc_channel.c`
做的事。

## 一块共享内存，两块视图

整个通道的核心是"一块共享内存，两端各自 mapping"。

```
        producer 进程                     subscriber 进程 A
        ┌─────────┐                      ┌─────────┐
        │ ipc_pub │                      │ ipc_sub │
        └────┬────┘                      └────┬────┘
             │ mmap("/flowipc_topic")            │ mmap("/flowipc_topic")
             ▼                                   ▼
   ┌─────────────────────────────────────────────────────┐
   │ /dev/shm/flowipc_topic (POSIX shm object)            │
   │ ┌─────────────────┬──────────────────────────────┐  │
   │ │ ShmHeader        │  ShmSlot[0..queue_depth-1]   │  │
   │ │  mutex / cond    │   - seq (写序号)              │  │
   │ │  head (写游标)    │   - Message (sequence/lifetime│  │
   │ │  queue_depth     │     /topic_id/data/data_size │  │
   │ │                  │     /endian_marker/...)       │  │
   │ └─────────────────┴──────────────────────────────┘  │
   └─────────────────────────────────────────────────────┘
                                                 ▲
                                                 │ mmap("/flowipc_topic")
                                                 │
                                       subscriber 进程 B
```

ShmHeader 里的 `mutex` 和 `cond` 是 **process-shared** 的 pthread
同步原语——Linux/glibc 默认 `PTHREAD_PROCESS_SHARED` 启用，所以两端
`pthread_mutex_lock` / `pthread_cond_wait` 可以互相同步。
Windows 用 `Local\\flowipc_<name>` 三件套（`Map` / `Mutex` / `Event`）
仿真同一行为。

每个进程对这块内存的视角相同，但**每端的私有状态在进程本地**——
具体说就是 `read_cursor`：订阅者各自维护一份，不会串扰。

## 广播语义：写只增 head，读只动自己

这是 IPC 通道与教科书"队列实现"的最大区别，也是最值得读者脑子里
想清楚的一处。

经典队列语义："读就走 head++，下一个消费者读取 head++_again"。
应用到广播场景——三个订阅者抢同一条消息，谁先读到谁拿走，下一个 reader
看到的是下一条。
结果就是三个订阅者**谁都只收到 1/3** 的数据——control 看到 1，safety 看到
2，monitor 看到 3。

**我们的 IPC 通道不用这个语义**。它用的是"读不删"——
发布者只递增全局写游标 `head`，从不阻塞；订阅者只在本地 `read_cursor`
上 +1，从不动 shm 上的 head。

```
        head 写到这里
            │
   ▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼
   ─── 0 ─ 1 ─ 2 ─ 3 ─ 4 ─ 5 ─ 6 ─ 7 ─ 0 ────  (覆盖最旧)
                                              ▲
                                  subscriber A：read_cursor=4
                                  subscriber B：read_cursor=2
                                  subscriber C：read_cursor=6

   写：head = (head + 1) % depth
   读：subscriber.read_cursor++
       （local 进程，不修改 shm）
```

这样每个订阅者按自己的速率独立读取——A 慢就慢慢看，B 跳到当前 head，
C 紧跟最新。
新订阅者加入时把自己的 `read_cursor` 初始化为 `head - queue_depth`（跳到
最新窗口），丢弃过期——这是 2024 年 11 月 PR #154 的设计变更，最早版本
会从头读，导致 bus flood。

订阅者落后超过 `queue_depth` 时会被**自动追帧**：把 read_cursor 拉到
`head - queue_depth`——丢弃中间的过期消息。
**不阻塞发布者**，也不阻塞订阅者——这是这套设计的硬约束。

## 延迟/唤醒：从轮询到条件变量

早期实现里订阅者后台线程用 `usleep(1ms)` 轮询 head。
结果：附加延迟 0.5~1 ms、跳动显著，多跳流水线上（perception→fusion
→control→safety）变成 4~6 ms 的不可控尾延迟。

当前实现：发布者写完数据后，在共享内存里 `pthread_cond_broadcast`——
所有等待的订阅者线程被**立即唤醒**，不是等到下一个 1 ms 窗口。
仍留一个**长 timedwait 兜底**（典型 100 ms），用于响应停止请求 / 容错
漏唤醒。

```
publish:
  payload → mmap slot[(head++) % depth]
  pthread_cond_broadcast(&hdr->cond)   // 唤醒所有订阅

subscriber (waiting on cond):
  for (cursor = local; cursor < head; cursor++) {
      msg = slot[(cursor) % depth]
      callback(msg)
  }
  timedwait(&hdr->cond, 100ms)         // 下一帧来了 → broadcast → 立即返回
                                       // 100ms 兜底 → 醒一醒看看该不该退出
```

读者也许会问：broadcast 不是"all-wake-up"？会不会惊群？
**会**，但订阅者数 <= 8（IPC 通道的硬上限，`callbacks[8]`），
且只有真正落后于 head 的订阅者会读新槽，wakeup 后的判断是 O(1)。
这是用简单的"广播"换实时性的设计选择——不为精细调度付费。

## 数据帧：不是裸字节，是带版本号的 Message

每条消息在 IPC 槽里按下表存放：

```
struct ShmSlot {
    uint64_t seq;             // 写序号，写者单调递增
    Message  msg;             // 见 include/message_bus.h
};
```

`Message` 内含：

| 字段 | 用途 |
|------|------|
| `type_id` | 见第 06 章——序列化层类型 ID |
| `topic_id` | 共享内存通道以 topic 命名（`/flowipc_<topic_id_slug>`） |
| `seq` | 节点级序列号（独立于 ShmSlot.seq） |
| `timestamp_us` | 发送时刻，CLOCK_MONOTONIC |
| `lifespan_us` | 过期时长，0=不过期 |
| `data_size` | 字节载荷长度 |
| `data[MSG_BUS_MAX_DATA_SIZE]` | 序列化后的字节流（64 KiB 上限） |
| `endian_marker` | 字节序 marker |
| `priority` / `qos_flags` | QoS 决策位 |

注意：这个 `Message` 结构与进程内总线（见第 05 章）共用——所以两端 ABI
只要 type_id 对齐，序列化字节流就兼容。
版本兼容：今天 IPC 通道强制两端**编译期同版本**——ABI 不变。
未来若做协议升级（`docs/HANDOFF_2026-09-23.md` 提到过一次）会加
`channel_protocol_version` 字段在 ShmHeader。

## 队列参数：depth / 槽大小

每个 IPC 通道在创建时分配：

- `queue_depth`（默认 64，per channel）。64 条是经验值，覆盖一帧管道
  跨 3~4 跳时的扇出抖动；过深增加 mmap 内存占用，过浅容易追帧。
- `slot_size = sizeof(Message) = 64 KiB + header`。
  384 KiB（64 × 6 KiB）一个 topic；一个进程 16 个 topic = 6 MiB shm。

`/dev/shm/` 容量在嵌入式平台可能只有几十 MiB——RC car 模式下 (、
`profile=hw`) shm 总用量被监控在 `monitor_node`；超阈值会告警。

## 生命周期：谁创建、谁 unlink

`scripts/demo.sh --multi` 启动后，`flow_launcher` 第一个开始`fork` 的
子进程负责创建 shm object（`shm_open(name, O_CREAT | O_RDWR, ...)`
随后 `ftruncate` 到 `win_shm_total_size(depth)`）。
最后一个退出（或 SIGINT 终止）的进程负责 `shm_unlink(name)`。

这条生命周期是**两难之间的折中**：

- 若创建者 unlink，崩溃时 shm object 残留 `/dev/shm/`，下次启动
  `shm_open` 撞上 → `EEXIST`。
  v0 真实坑：crash test 之后 `/dev/shm/flowipc_*` 堆积，下次启动报
  `ShmOpen failed`。
- 若 unlink 由专门 cleanup 进程负责，需要 SIGTERM handler 干净退出——
  复杂。

最终选择：发布者（producer）负责 unlink，即"写者拥有生命"。订阅者
打开时若检测到残留则 `unlink` 再 `create` —— 自愈机制。
详细复盘见"我们踩过的坑"。

## 平台差异：Linux/macOS 与 Windows

```
Linux / macOS                       Windows (MinGW/Cygwin)
─────────────                       ──────────────────────
shm_open + mmap                      CreateFileMapping + MapViewOfFile
pthread_mutex PROCESS_SHARED         HANDLE Local\\... (Map/Mutex/Event)
pthread_cond PROCESS_SHARED          WaitForSingleObject / SetEvent
/dev/shm/                            Global\\<name>
```

行为差异收口在 `include/platform_compat.h`（force-include，仅 `_WIN32`
生效）+ `if(_WIN32)` 在 `src/core/ipc_channel.c` 里。
Linux 行为零变化——这是我们在 v0 里就定了的承重条款。

macOS 弱化：robust mutex 行为差异——`PTHREAD_MUTEX_ROBUST` 在 Darwin
下不完全一致，会触发 `ipc_channel.c` 内部一段 fallback：检测到
`EOWNERDEAD` 时**放弃**恢复链路，节点之间降级为 AF_UNIX。
这条降级路径写出来不是为了 macOS 的日常使用——主要是 CI pipeline
偶尔跑 macOS 时不会因为 robust mutex 行为差异而炸 build。

## 我们踩过的坑

### 坑一：单消费者队列语义导致多订阅者丢帧

如本篇开头所述——`subscriber_i.read_cursor == subscriber_j.read_cursor`
意味着只有最先抢到 mutex 的进程能读 head++，其余进程看到的是"下一条"
不是"同一份"。
结果三个 control 节点都只收到 1/3 数据。
修复改 ring 为"读不删、head 仅写"——这是 2024 年 11 月的 PR #154。
要**特别提读者**：这是 IPC 设计里的"招牌坑"——任何教科书"单消费者队列"
实现搬到广播场景都会翻车。

### 坑二：崩溃后残留 shm 累计

旧版：第一个 publisher 创建 + unlink。
一个 subscriber 进程 segfault，没有 unlink。
下次启动再开 publisher → `EEXIST` → `flow_launcher` fail。
修复：发布者创建时先 `shm_unlink` 再 `shm_open`(O_CREAT)——把残留清掉。
这个修复把 `/dev/shm/` 当 durable storage 的隐式习惯打断了，所以会留
stamp 文件——若读者看到 `/dev/shm/flowipc_*` 残留，**别手动 rm**，
下次启动会自清；手动 rm 不会损坏，但下次启动会比直接启动多走一次
"清残留"。
详见 `docs/HANDOFF_2026-09-23.md`。

### 坑三：轮询订阅 1 ms 让控制环跑成 4 ms 尾延迟

第 05 章提到的"bus 1 ms 轮询"等价坑在 IPC 也踩过——条件变量 broadcast
替代轮询。
现在的 timedwait 是 100 ms 兜底，不再有 0.5 ms 抖动。
但代价是 timedwait 长，节点退出要等一个 100 ms 周期——这个是不是问题
取决于你怎么想 shutdown latency；我们跑了 RC car shutdown 测，3 fps
不抖，可接受。

### 坑四：`endian_marker` 被订阅端"再来一次 normalize"

第 06 章也提过的同一个 bug，这次是在 IPC 层触发的——订阅端 shm slot
读出 message 后调 `serializer_normalize_endian`，但**已经被发布的 producer
进程标记过了**。
双方都跑了 normalize（第二次 no-op 因为 marker = host-order），问题不大
但徒劳。
后来我们在 subscriber 侧改成"尊重 producer marker，不要私自 normalize"
——只有 read from wire 那一瞬间走一次 normalize。
两个章节提到同一个 bug 是因为两边都涉及；这只是把"语义不变量"再次强调。

### 坑五：CEXTERN 数组槽位用 8 而不是 N

IPC 通道早期间 `callbacks[8]` 上限写在头文件。
后来某节点（不记得是谁）想 listen 9 个 callback 直接爆。
这个不是 bug，是设计——限到 8 是因为 broadcast 时一次性唤醒 8 个 tid 不
超过调度延迟带宽；
9+ 回调的用例请把 callback 改成 dispatch table（一个 callback → 内部
多 callback）。
读者加新订阅者之前先看看现行 callbacks 是不是到 7 了——到 7 就该
触发表了。

## 与其它章节的关系

- 与第 03 章：参数系统有自己一份 AF_UNIX 行协议；本章 IPC 通道不
  直接走参数桥——它们是两条并列的进程间通路。
- 与第 05 章：进程内零拷贝池在 IPC 模式被换成"序列化字节流"，性能
  模型要乘以序列化耗时。
- 与第 06 章：序列化层 type_id 在 IPC 槽位 header 里复用——两端只看
  type_id 对不对得上。
- 与第 09 章：每个 IPC 通道在 `flow_registry` 登记，flowctl 可用
  `flowctl inspect ipc <topic>` 看当前 ring 深度、追帧次数。
- 与第 21 章（仪表盘）：仪表盘通过另一个仪表盘专用 `dashboard_bridge`
  共享内存分块通道拉数据；本篇不展开——它用的是同一份共享内存机制，
  协议不同。

## 思考题

1. 一个 topic 多 producer 时，谁负责 ++head？多 producer 写同一个 ring
   的 head 必须有 mutex；当前实现是单 producer per topic（典型）——
   请想清楚多 producer 会出现什么新问题。
2. 假如 `queue_depth` 满了 + 订阅者 100 ms 兜底超时醒来发现还没新数据，
   它该不该**主动**通知 publisher 减速？
3. 跨机器部署时能不能用同一份 IPC 通道？哪些 API 要改？
   （提示：POSIX shm 不跨主机。）

下一章是节点发现与注册中心（见第 09 章）——如果说本篇解决了"已经
认识了之后怎么传"，第 09 章解决"刚启动时怎么认识"。
