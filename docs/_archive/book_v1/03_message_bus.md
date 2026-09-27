<!-- @archive-banner -->

> **归档说明**：本章是 docs/book v1（2026-09 前后"按源码逐条重建"产物）的版本。
> 内容大量绑定代码行号、文件名、函数符号，与代码强耦合 —— 维护成本高且易过时。
> v2 重写计划：见 `docs/book/README.md` 的写作风格约束 + 真技术书范式。
> 本归档文件保留供历史参考；引用时用 `docs/_archive/book_v1/03_message_bus.md` 而非 `docs/book/`。
> **现章节**：BOOK.md 第 05 章「消息总线」。

# 第 05 章：消息总线 —— 同进程里，节点怎么互相说话

> **本章导读**：
> 车跑起来的时候，激光雷达 20 Hz 推点云、控制层 40 Hz 推指令、规划 20 Hz 推轨迹。这些数据不是排队进来的，而是同时涌进来。这一章回答：同一个进程里的十几个节点，靠什么把消息送到该去的地方。
>
> 答案是 [`MessageBus`](file:///home/caixuf/code/FlowEngine/src/core/message_bus.c)（约 1700 行）。它做三件事：**把消息排队、把消息分给订阅者、记录延迟统计**。
>
> 但旧稿对这个模块的描述几乎每一处都有偏差，本章逐条按源码重建。最重要的三条：
>
> 1. **回调拿到的不是「不复制的数据」。** `publish` 路径**必定发生一次 `memcpy`**（`message_bus.c:963`）——把载荷拷进 `msg->data[65536]`。真正不复制的是另一条路径 `publish_loaned`。
> 2. **队列不是 1024 深，是 4 个分片各 1024。** 而且分片函数是 `msg_id % 4`（`message_bus.c:316-318`）——**同一话题的两条消息可能落在不同分片、被两个线程并发回调**。
> 3. **QoS 的枚举名旧稿全写错了。** 真实是 `QOS_BEST_EFFORT` / `QOS_RELIABLE` / `QOS_DROP_OLDEST` / `QOS_DROP_LATEST` / `QOS_BLOCK`。

---

## 1. `Message`：一个 64 KB 的信封

### 1.1 完整结构体

`include/message_bus.h:54-84`：

```c
typedef struct Message {
    char        topic[MSG_BUS_MAX_TOPIC_LEN];    /**< 主题 */
    char        sender[MSG_BUS_MAX_SENDER_LEN];  /**< 发送者名称 */
    uint32_t    msg_id;                           /**< 消息唯一ID */
    MessageType type;                             /**< 消息类型 */
    uint64_t    timestamp_us;                     /**< 发布时间戳（CLOCK_MONOTONIC 墙钟）*/
    int32_t     topic_idx;                        /**< 进程内路由元数据：topic_entries 索引 */
    uint32_t    data_size;                        /**< 有效数据字节数 */

    /* ── 类型安全序列化字段 (Phase 1) ─────────────────────── */
    uint32_t    type_id;          /**< FNV-1a hash 类型标识 (0=raw/unknown) */
    uint32_t    schema_hash;      /**< 字段级布局哈希（0 = 未提供/旧消费者）*/
    uint8_t     schema_version;   /**< schema 版本号 (0=unknown, 1=initial) */
    uint8_t     endian_marker;    /**< 0x12=LE, 0x21=BE, 0=unknown */
    uint8_t     _reserved[2];     /**< 对齐保留 */

    uint8_t     data[MSG_BUS_MAX_DATA_SIZE];      /**< 负载数据 */

    /* 异步 loaned payload：非 NULL 时 payload 由外部 buffer 提供，data[] 未填充。 */
    const uint8_t* _loaned_data;
    void (*_loaned_release)(void* data, void* user_data);
    void* _loaned_release_ctx;

    struct Message* _pool_next;
} Message;
```

**这个结构体 65736 字节**，因为 `data[65536]` 是**内联数组**，不是指针。

> [!IMPORTANT]
> **这与旧稿的「0 次拷贝（直接内联内存映射）」正好相反。** `data` 是内联的，所以**必须拷贝**——发布者的 buffer 和消息的生命周期不同。而「不拷贝」的实现方式恰恰是加一个 `_loaned_data` **指针**，走完全不同的代码路径（第 4 节）。

旧稿的结构体还漏了 `schema_hash` 字段、把 `_reserved` 写成 `[6]`，并且**列了 `_loaned_*` 三个字段却没解释它们是干什么的**。

### 1.2 布局：三处独立证据

[`examples/ipc_channel/chapter04.c`](file:///home/caixuf/code/FlowEngine/examples/ipc_channel/chapter04.c) 的 `run_layout`（`:217`）会打印真实偏移。实测输出：

```
sizeof(Message)                 65736
  offsetof topic                0
  offsetof sender               64
  offsetof msg_id               128
  offsetof type                 132
  offsetof timestamp_us         136
  offsetof topic_idx            144
  offsetof data_size            148
  offsetof type_id              152
  offsetof data                 164
  offsetof _loaned_data         65704
  offsetof _loaned_release      65712
  offsetof _loaned_release_ctx  65720
  offsetof _pool_next           65728
```

完整布局：

```
偏移    字段                    字节
0       topic[64]               64
64      sender[64]              64        128
128     msg_id        (uint32)  4        132
132     type          (enum)    4        136
136     timestamp_us  (uint64)  8        144
144     topic_idx     (int32)   4        148
148     data_size     (uint32)  4        152
152     type_id       (uint32)  4        156
156     schema_hash   (uint32)  4        160
160     schema_version(uint8)   1        161
161     endian_marker (uint8)   1        162
162     _reserved[2]            2        164
164     data[65536]             65536     65700
65700   填充对齐                 4        65704
65704   _loaned_data             8        65712
65712   _loaned_release          8        65720
65720   _loaned_release_ctx      8        65728
65728   _pool_next               8        65736
```

**三处互相印证**：

1. 上面的实测输出（`sizeof=65736`、`data@164`）；
2. `src/cpp/network_transport.cpp:74,76` 的 `static_assert`——`kWireHeaderSize <= 256` 且 `< sizeof(Message)`，**编译期**把 `offsetof(Message, data) = 164` 钉死在 $(0, 256]$ 区间；
3. `include/network_transport.h:46` 的 `#define NET_WIRE_HEADER_SIZE (offsetof(Message, data))`。

> **第 07 章的偏移表（`04_ipc_channel.md:98`）已经过期**：它写 `156 schema_version, endian_marker, _reserved[6]`。那是 `schema_hash` 加入、`_reserved` 缩小之前的旧布局。总大小 65736 和 `data@164` 仍正确，但 152~164 这一段错了。

### 1.3 载荷的正确读法

```c
// include/message_bus.h:86-88
static inline const void* message_bus_message_data(const Message* msg) {
    return msg && msg->_loaned_data ? msg->_loaned_data : (msg ? msg->data : NULL);
}
```

**回调里必须用这个函数，不能直接摸 `msg->data`。** 因为一旦有订阅者可能发 loaned 消息，`data[]` 就是空的。

仓库里 20 处生产代码遵守了这条（`perception_node.cpp:311`、`monitor_node.c:423`、`src/core/bag.c:240,273`、`network_transport.cpp:88` 等）。

### 1.4 进程内 vs 线上

`data[]` 之后的所有字段（`_loaned_data` / `_loaned_release` / `_pool_next`）**只在本机有效，绝不上 TCP**。跨机由 `NetworkTransport` 发紧凑帧：4 字节长度前缀 + 164 字节头 + `data_size` 长度的载荷（`network_transport.cpp:86-104`）。

---

## 2. 容量常量与它们真正的含义

`include/message_bus.h:34-44`：

| 宏 | 值 | 真实含义 |
|---|---|---|
| `MSG_BUS_MAX_TOPIC_LEN` | 64 | 话题名最大长度 |
| `MSG_BUS_MAX_SENDER_LEN` | 64 | 发送者名最大长度 |
| **`MSG_BUS_MAX_DATA_SIZE`** | **65536** | 单条载荷上限。注释（`:36-40`）记录了从 4096 上调的原因：StereoFrame 序列化后 44828 字节 |
| `MSG_BUS_MAX_TOPICS` | 64 | ⚠️ **只用于 `svcs[]` 上限，与 topic 数量无关**。topic 上限是 `BUS_MAX_TOPIC_ENTRIES = 128`（在 `.c:261`） |
| `MSG_BUS_MAX_SUBSCRIBERS` | 256 | 注释说「26 节点 pipeline 实测 >128」 |
| `MSG_BUS_QUEUE_SIZE` | 1024 | ⚠️ **是「每个分片」的深度**，不是总线总深度 |

`MSG_BUS_MAX_TOPICS` 是个**误导性命名**——它管的是 Req/Reply 的服务表大小，和话题目录毫无关系。

---

## 3. 架构：4 分片 + 4 线程

### 3.1 全景

```
  4 个发布者线程（任意节点）
         │
         │  message_bus_publish()
         │  ├─ 无锁扫描 topic_entries 找话题索引
         │  ├─ QoS 决策（满则丢/等）
         │  ├─ msg_alloc()  ← 唯一拷贝点：memcpy(msg->data, data, size)
         │  └─ rb_push(shard[msg_id % 4])
         ▼
  ┌──────────┬──────────┬──────────┬──────────┐
  │ shard 0  │ shard 1  │ shard 2  │ shard 3  │   各 1024 深
  │ 1024     │ 1024     │ 1024     │ 1024     │   合计 4096
  └────┬─────┴────┬─────┴────┬─────┴────┬─────┘
       ▼          ▼          ▼          ▼
  dispatch    dispatch    dispatch    dispatch     MSG_BUS_DISPATCH_THREADS = 4
   thread       thread      thread      thread     （message_bus.c:31）
       │          │          │          │
       └──────────┴────┬─────┴──────────┘
                      ▼
              订阅者回调（串行、同步执行）
```

**`message_bus_create`（`:690-745`）会立刻启动 4 个分发线程**（`:726-743`），失败时回滚：置 `running=false`、广播唤醒已起的线程、`join`、`free`。

### 3.2 分片：同话题可能并发

```c
// message_bus.c:316-318
static inline RingBuffer* shard_for(RingBuffer* shards, uint32_t msg_id) {
    return &shards[msg_id % MSG_BUS_DISPATCH_THREADS];
}
```

> [!WARNING]
> **这是本章最重要的一条并发警告。**
>
> 分片依据是 `msg_id`，而 `msg_id` 由 `msg_alloc` 递增分配。**同一个话题的两条连续消息，`msg_id` 相差 1，会落到不同分片、被两个不同的线程分发。**
>
> 所以：**订阅者不能假设「同一个话题的回调是串行的」。** 上一条消息的回调可能还在跑，下一条已经在另一个线程上开始了。
>
> 这与「一条消息的 N 个订阅者是串行的」（下一节）正好相反——**同话题跨消息并发，单条消息内跨订阅者串行。**

### 3.3 回调：单条消息内串行

`dispatch_message`（`:472-625`）：

```c
// :524-541 无锁 seqlock 快照
retry_snapshot:
  while ((s1 = atomic_load(&bus->subs_seq)) & 1u) { }   /* 写者进行中则自旋 */
  for (i<sub_count) if (subs[i].active && topic_match(...)) {
      atomic_fetch_add(&subs[i].in_flight, 1);
      snap[snap_count++] = {cb, user_data, src};
  }
  if (atomic_load(&bus->subs_seq) != s1) { 回滚 in_flight; goto retry_snapshot; }

// :563-567 串行遍历，同步执行
for (int i = 0; i < snap_count; i++) {
    snap[i].cb(msg, snap[i].ud);
    atomic_fetch_add(&bus->stat_delivered, 1);
}
```

**三个后果：**

1. **回调在分发线程上同步执行**，不是发布者线程，也不是协程；
2. **同一条消息的 N 个订阅者串行**——任一回调 `sleep()` 会拖住同一分片上后续所有消息。**回调里绝不能阻塞。**
3. 订阅表的并发安全靠 **seqlock**（`subs_seq` 奇偶）：写者置奇数位，读者自旋等偶数位，读完校验序号没变。**这让每条消息的热路径分发零加锁。**

`control/cmd` 上还有一处诊断：每 200 条打一次订阅者画像 `[CMD_DIAG]`（`:542-560`）。

### 3.4 话题匹配：只有两种形态

```c
// message_bus.c:310-313
static bool topic_match(const char* pattern, const char* topic) {
    if (strcmp(pattern, "*") == 0) return true;
    return strcmp(pattern, topic) == 0;
}
```

**没有前缀通配，没有正则。** 只有精确匹配和字面量 `"*"`。

`"*"` 的生产用例只有录包器一个（`src/core/bag.c:307`）：

```c
return message_bus_subscribe(bus, "*", bag_record_callback, w);   /* bag_writer_attach */
```

**每一条消息都会被它 memcpy 一次到磁盘环**（`bag.c:273`）。所以录包开着的时候，总线的内存带宽是平时的两倍。

---

## 4. 三条投递路径：只有一条是真零拷贝

旧稿把三者混为一谈。它们的行为完全不同：

| | `message_bus_publish` | `message_bus_publish_loaned` | `message_bus_publish_zero_copy` |
|---|---|---|---|
| 投递时机 | **异步**（入队） | **异步**（入队） | **同步**（发布者线程内） |
| 载荷拷贝 | **必拷贝一次** | **零拷贝** | **零拷贝** |
| 创建 `Message` | 是 | 是 | **否** |
| 进内存池 | 是 | 是 | **否** |
| 回调拿到 | `msg->data` | `message_bus_message_data()` | **裸指针**（签名不同） |
| 生命周期 | 总线管 | 总线管 + `release_fn` | **调用者管** |
| 生产使用 | **全部** | 1 处 | **零** |

### 4.1 `publish`：唯一拷贝点

`message_bus.c:946-963`：

```c
Message* msg = msg_alloc(bus);
...
msg->data_size    = size;
...
if (data && size > 0) memcpy(msg->data, data, size);   /* :963 —— 全链路唯一一次拷贝 */
```

**所以旧稿的「回调拿到的是消息指针，数据不复制」是错的。** 回调拿到的是**消息指针**（对），但**载荷已经被复制过**（错）。

`msg_id` 在**入队前**缓存（`:948` 有注释：`rb_push` 后 msg 可能已被分发线程回收）。

### 4.2 `publish_loaned`：真正的零拷贝

声明（`include/message_bus.h:164-167`）：

```c
/**
 * 异步借用发布。总线不复制 payload，分发完成后调用 release_fn。
 * 成功返回后 payload 所有权转移给总线；失败时仍由调用者负责释放。
 * 订阅回调须使用 message_bus_message_data()，不可直接假定 msg->data 有效。
 */
```

实现（`message_bus.c:1111-1120`）：

```c
msg->data_size = size;
msg->_loaned_data        = (const uint8_t*)data;    /* 只存指针，data[] 不填 */
msg->_loaned_release     = release_fn;
msg->_loaned_release_ctx = release_user_data;
int ret = rb_push(shard_for(...), msg);
```

**释放的五条路径**——这是「坑」的核心，漏掉任何一条就是内存泄漏：

| 时机 | 位置 |
|---|---|
| 正常分发完 | `message_bus.c:681` |
| 队满被驱逐 | `:419` |
| 队列销毁排空 | `:370` |
| `QOS_DROP_LATEST` 丢弃 | `:1084` |
| `QOS_BLOCK` 超时丢弃 | `:1096` |

`msg_release_loaned`（`:158-167`）是**幂等**的——调一次 release 后把三个字段清空。

唯一的生产用例是**双目相机**（`stereo_camera_node.c:313-329`）：

```c
uint8_t* buf = (uint8_t*)malloc(44828);
if (StereoFrame_serialize(&frame, buf, &len) == 0 && len > 0) {
    int rc = transport_publish_loaned(g.transport, "sensor/stereo",
                                      buf, (uint32_t)len, release_loaned_frame, NULL);
    if (rc == 0) { g.stereo_published++; } else { free(buf); g.frames_failed++; }
}
```

`44828` 就是 `message_bus.h:36-37` 注释里那个「StereoFrame 序列化后的固定字节数」——**`MSG_BUS_MAX_DATA_SIZE` 从 4096 升到 65536 的唯一动因。**

> 🐛 **这个调用点有一个真实 bug。** `message_bus_publish_loaned` 在 QoS 丢弃路径（`:1085`、`:1097`）**释放了 payload 却 `return 0`**。所以「丢弃」和「成功」无法区分，`g.stereo_published++` 把丢帧算成了成功。

### 4.3 `publish_zero_copy`：在别人线程上同步执行

`message_bus.c:1413-1450`：

```c
pthread_mutex_lock(&bus->zc_mutex);
for (i<zc_sub_count) if (active && topic_match) snap[n++] = {cb,ud};
pthread_mutex_unlock(&bus->zc_mutex);      /* 先快照，解锁后调用 */
for (int i = 0; i < snap_count; i++) {
    snap[i].cb(topic, sender, msg_id, ts, data, data_size, ud);   /* :1441 发布者线程内！*/
    count++;
}
message_bus_publish(bus, topic, sender, data, data_size);          /* :1447 额外再投一份 */
```

**三个不寻常的地方：**

1. **回调在发布者线程里同步执行**——和另外两条路径的语义完全不同；
2. **回调签名完全不同**——收的是 `(topic, sender, msg_id, ts, data, size, ud)` 七个参数，没有 `Message`；
3. **额外再投一份给普通订阅者**（`:1447`），返回值是「通知了几个 zc 订阅者」而非 0/1。

**整个零拷贝子系统（`subscribe_zero_copy` / `unsubscribe_zero_copy` / `publish_zero_copy`）在 `modules/` 和 `src/` 零使用**，只有 demo、benchmark 和测试在用。

### 4.4 内存池

```c
// message_bus.c:123-129
#define MSG_POOL_MAX 1024
typedef struct { Message* head; int count; pthread_mutex_t mutex; } MsgPool;
```

```c
// :303-306
static Message* msg_alloc(MessageBus* bus) {
    Message* m = msg_pool_take(&bus->pool);
    return m ? m : (Message*)malloc(sizeof(Message));   /* 池空就 malloc 64KB */
}
```

`message_bus.c:119-122` 记录了**为什么不用 Treiber 无锁栈**：4 生产者 + 4 分发线程下触发 ABA → double-free（glibc 崩溃），改用互斥链表。

> **旧稿称「整条生命周期里动态内存分配次数是 0」——不成立。** 池空时 `malloc`（`:305`），池满时 `free`（`:136`）。池的上限 1024 × 65736 ≈ **64 MB 常驻**。
>
> 另外旧稿写的字段名 `bus->free_pool` 实际是 `bus->pool`。

---

## 5. QoS：真实语义

### 5.1 枚举的真实名字

`include/message_bus.h:314-343`：

```c
typedef enum {
    QOS_BEST_EFFORT = 0,  /**< 尽力传输（允许丢帧，遵循 QosPolicy） */
    QOS_RELIABLE    = 1,  /**< 可靠传输（自动升级为 QOS_BLOCK，不丢帧） */
} QosReliability;

typedef enum {
    QOS_DROP_OLDEST = 0,  /**< 丢弃最旧消息（默认） */
    QOS_DROP_LATEST = 1,  /**< 丢弃最新消息（保留旧数据） */
    QOS_BLOCK       = 2,  /**< 阻塞发布者直到队列有空间 */
} QosPolicy;
```

**旧稿写的 `QOS_POLICY_RELIABLE` / `QOS_POLICY_BEST_EFFORT` / `QOS_DISCARD_OLDEST` / `QOS_DISCARD_NEWEST` 四个名字，一个都不存在**，而且**漏掉了 `QOS_BLOCK`**。

### 5.2 决策流程

`message_bus.c:874-943`：

```
pending_count >= depth ?
├── QOS_FLAG_RELIABLE 置位    → 強制 QOS_BLOCK（可靠覆盖策略，:884-885）
├── QOS_FLAG_DROP_LATEST 置位 → should_drop = true（:896）
├── QOS_FLAG_BLOCK 置位       → 忙等（:903-921）
└── 其它（默认）               → need_evict = true → rb_evict_oldest_topic_all
                                                 跨 4 分片驱逐一条（:932-943）
should_drop → stat_dropped++; return 0;      /* :926-929 ← 丢弃也返回 0 */
```

**`QOS_BLOCK` 是忙等，不是条件变量**（`:903-921`）：

```c
int max_waits = (qf & QOS_FLAG_RELIABLE) ? 5000 : 1000;    /* :906 魔数：5s / 1s */
while (atomic_load(&e->pending_count) >= depth && waits < max_waits) {
    flow_pal_sleep_us(1000); waits++;                        /* :909 1ms 轮询 */
}
```

不持锁所以不死锁，**但发布者线程会被挂住最多 5 秒**。消费方消失时靠超时兜底。

### 5.3 生产配置

`config/pipeline.json` 里 33 条 publish 声明，**全部是 `drop_oldest` + `best_effort`**，depth 从 1 到 4：

| depth | 话题 |
|---|---|
| 1 | `vehicle/state`、`road/geometry`、`road/traffic_lights`、`sim/tick`、`scene/frame`、`sensor/gps`、`sensor/camera` |
| 2 | `sensor/lidar`、`sensor/lidar_points` |
| 4 | `sim/collision` |

`deadline_ms`：`vehicle/state` / `sensor/lidar` / `sensor/lidar_points` 是 50，`sensor/gps` 是 100。

`control/cmd`（`:309`）的 depth=1 + drop_oldest 就是 `BusQueueBridge`「单槽覆盖」语义的来源（`coroutine_task.h:615-616`）。

**没有任何一条配置用 `block`。**

### 5.4 lifespan：分发侧的过期丢弃

`message_bus.c:480-503`：

```c
uint64_t age_ms = (clock_now_monotonic_wall_us() - msg->timestamp_us) / 1000ULL;   /* :488 */
if (age_ms > q->lifespan_ms) {
    atomic_fetch_add(&e->drop_count, 1);
    atomic_fetch_sub(&e->pending_count, 1);
    return;                                    /* :493-500 跳过整个分发 */
}
```

**用的是墙钟而不是逻辑时钟**——`message_bus.c:587` 的注释说明了原因：「仿真模式下 `clock_now_us()` 是逻辑时间，会失真」。

---

## 6. 线程安全：锁表与唯一一条锁序规则

### 6.1 九把锁

| 锁 | 位置 | 保护 | 热路径？ |
|---|---|---|---|
| `shards[i].mutex` + 2 个 cond | `:79-81` | 每个分片独立 | **publish 必取一次** |
| `sub_mutex` + `sub_cv` | `:220-221` | 订阅表写 + 等 `in_flight` | 否 |
| `subs_seq`（原子） | `:222` | 订阅表 seqlock | **dispatch 无锁读** |
| `zc_mutex` | `:229` | `zc_subs[]` | 否 |
| `svc_mutex` | `:234` | `svcs[]` + **handler 执行** | request 路径 |
| `pool.mutex` | `:128` | 空闲链表 | 每条消息 2 次 |
| `topic_mutex` | `:287` | topic 条目慢路径创建 + 统计读 | **热路径无锁** |
| `remap_mutex` | `:294` | `remaps[]` | 默认零加锁 |
| `reply_mutex` | `:251` | — | **从未加锁（死代码）** |

**「热路径无锁」的两处都有明确机制**：

- 订阅表靠 seqlock（3.3 节）；
- topic 条目靠 release/acquire（`message_bus.c:832-836`）：

```c
/* ── topic 条目无锁查找 ──
 * topic_entries 只追加、不删除：条目一经创建其 topic 名不再变化，且注册
 * 线程在写满所有字段后才以 release 递增 topic_count。读侧用 acquire 读
 * topic_count，即可安全地无锁遍历 [0, count) 已完整初始化的条目。
 * 热路径（topic 已注册）零加锁。 */
```

### 6.2 唯一的锁序规则

嵌套只存在一处：**`sub_mutex` → `topic_mutex`**（`update_subscriber_count`，`:333`）。

反序被**显式禁止并有防护代码**（`message_bus.c:340-343`）：

```c
/* 无锁统计某 topic 的活跃订阅者数（seqlock 读订阅表，不取 sub_mutex）。
 * 供 publish 创建 topic 条目时初始化 subscriber_count。不能复用持 sub_mutex 的
 * update_subscriber_count：publish 此时已持 topic_mutex，若再取 sub_mutex 会与
 * subscribe 的 sub_mutex→topic_mutex 构成 AB-BA 死锁。 */
```

为此专门写了 `count_active_subscribers`（`:344-354`）。

> **这 4 行注释是本章最值得学的东西。** 它记录的不是「我加了个锁」，而是「我**不能**在这里加锁，因为会构成 AB-BA」。正确性论证写在了防线的旁边。

分片锁序也论证过：`message_bus_peek_latest`（`:1625-1644`）按 $0 \dots N-1$ 全锁、反序解锁，注释（`:1621-1623`）说明「与各 dispatch 线程只锁自己分片不构成环」。

### 6.3 残留风险：`svc_mutex` 持锁执行用户代码

`message_bus.c:641-660`（Req/Reply 路径）：

```c
if (msg->type == MSG_TYPE_REQUEST) {
    pthread_mutex_lock(&bus->svc_mutex);              /* :643 */
    for (i<svc_count) if (active && strcmp(topic)==0) {found=&svcs[i];break;}
    if (found) {
        Message reply; memset(&reply, 0, sizeof(reply));   /* :653 栈上 65KB */
        ...
        found->handler(msg, &reply, found->user_data);     /* :659 在锁内执行！*/
        pthread_mutex_unlock(&bus->svc_mutex);             /* :660 */
```

**handler 里如果调 `message_bus_register_service` 之类走 `svc_mutex` 的路径 → 非递归互斥自死锁。** 慢 handler 也会阻塞所有服务请求。

**唯一的生产用例**是 `safety/status`：服务端 `safety_control_node.cpp:941` 注册，客户端 `control_node.cpp:1076-1085` 每 100 周期调一次，超时 100 ms。

> 旧稿说「总线收到 `MSG_TYPE_REPLY` 时按 `msg_id` 命中等待句柄」——**那段代码不可达**。真正的流程是 `dispatch_thread_fn:641-678` 直接在分发线程里处理 REQUEST 并填充 16 个 `ReplySlot` 之一（`:663-674`），`message_bus.c:611-624` 的 REPLY 分支永远走不到。

---

## 7. `BusQueueBridge`：为协程而生的桥

[`include/coroutine_task.h:607-826`](file:///home/caixuf/code/FlowEngine/include/coroutine_task.h)，header-only。

### 7.1 它解决什么

文件头注释 `:607-618` 记录了一次真实事故：

> WhenAnyBusAwaitableT 的订阅随 awaitable 生命周期反复注册/退订，多次循环后消息与超时 fire 双失效（**2026-07-31 事故**：safety_control 启动后 1-3s 永久挂起 → control/cmd 断流 → flowsim 内置巡航追尾；同一适配器在 flowsim 上亦复现）。本桥把订阅提升到节点生命周期：
> - 节点 init 注册一次（回调只覆盖槽，持互斥，dispatch 线程上轻量）
> - 协程每 tick try_take 取走最新消息
> - 彻底消除订阅生命周期竞态；resume 仍在 executor 线程

被它取代的 API 已标 `[[deprecated]]`：`when_any_bus_for`（`:554-560`）、`select_for`（`:562-568`）。

### 7.2 回调实现

`coroutine_task.h:787-816`：

```cpp
static void on_message(const Message* msg, void* user_data) {
    g_cb_count.fetch_add(1, std::memory_order_relaxed);
    auto* self = static_cast<BusQueueBridge*>(user_data);
    self->cb_count++;
    std::coroutine_handle<>   waiter;
    std::shared_ptr<AwaitCtl> ctl;
    flowcoro::rt::RtExecutor* exec = nullptr;
    {
        std::lock_guard<std::mutex> lk(self->mtx_);
        for (auto& [t, slot] : self->slots_)
            if (t == msg->topic) {
                message_bus_copy_message(&slot.msg, msg);   /* ★ 关键 */
                slot.has = true;   /* 覆盖旧值：depth=1 drop_oldest */
                break; }
        waiter = self->waiter_; ctl = self->waiter_ctl_; exec = self->waiter_exec_;
        self->waiter_ = {}; self->waiter_ctl_ = {}; self->waiter_exec_ = nullptr;
    }
    /* 在锁外 fire，避免 try_fire→post_ready→executor→await_resume 时重入 mtx_ */
    if (waiter && ctl && exec) if (ctl->try_fire(AwaitStatus::Ready)) exec->post_ready(waiter);
}
```

**`message_bus_copy_message`（`message_bus.h:91-100`）是本章「生命周期铁律」的正确解法**：它把 loaned 载荷**物化进 `slot.msg.data[]`** 并清空三个 loaned 字段——**从此这个槽拥有自己的副本，不依赖总线里那个 `Message` 的寿命。**

「在锁外 fire」也是必要的：否则 `try_fire → post_ready → executor → await_resume` 会重入同一把 `mtx_`。

### 7.3 API 与生产用例

| 成员 | 行 | 语义 |
|---|---|---|
| 构造（`initializer_list` 话题） | `:630-636` | 每话题一槽 + 一次订阅 |
| 析构 | `:637-643` | 逐话题 `unsubscribe_ex`（带 `in_flight` 等待） |
| 拷贝/赋值 | `:644-645` | `= delete` |
| `try_take(topic, out)` | `:648-659` | 取走指定槽 |
| `try_take_any(topic_out, out)` | `:681-692` | 返回第一个有值的槽 |
| `recv_any_for(timeout_us)` | `:782-784` | 协程 awaitable 工厂 |
| `reconnect()` | `:669-679` | 重订阅 + 清零计数 |
| `cb_count` / `take_count` | `:662-663` | 诊断：区分「回调没被调」vs「被调但没生效」 |

| 节点 | 订阅话题 | 位置 |
|---|---|---|
| `safety_control_node` | `control/raw_cmd`、`inference/raw_cmd` | `:463` |
| `flowsim_node` | `control/cmd` | `:1745` |

`flowsim_node.cpp:1877-1897` 有一处**自愈逻辑**：如果 `cb_count` 停止增长（回调不再被调），就 `cmd_bridge.reconnect()` 重新订阅，**10 秒防抖**。

> 🐛 `on_message` 的 `self->cb_count++`（`:790`）在 `mtx_` 之外且**非原子**。而 `flowsim_node.cpp:1877` 从协程线程读它——**技术性 data race**。（`:628` 的全局 `g_cb_count` 用了 `std::atomic`，那才是对的写法。）

---

## 8. 边界：总线在哪里终止

`transport_publish`（`src/core/transport.c:273-297`）是三条路的汇合点：

```c
int ret = message_bus_publish(t->bus, topic, "transport", data, size);   /* :278 永远先投本地 */
pthread_mutex_lock(&t->mutex);
for (i<route_count) if (strcmp(routes[i].topic, topic)==0) {
    if (routes[i].ipc_channel && routes[i].is_publisher) {
        route_ret = ipc_channel_publish(routes[i].ipc_channel, topic, "transport", data, size);
    } break; }
pthread_mutex_unlock(&t->mutex);
return ret != 0 ? ret : route_ret;
```

**边界规则：总线止于 `message_bus_publish` 返回。** 载荷在 `memcpy`（`:963`）的那一刻就与总线解耦了。

而 **loaned 借用跨不出进程**（`transport.c:306-313`）：

```c
if (transport_route_type(t, topic) == ROUTE_LOCAL) {
    return message_bus_publish_loaned(t->bus, topic, "transport", data, size,
                                      release_fn, release_user_data);   /* 只有本进程才借 */
}
int ret = transport_publish(t, topic, data, size);
if (ret == 0) release_fn(data, release_user_data);   /* 同步拷贝后立刻释放 */
```

**跨机走紧凑帧**：`network_transport.cpp:86-104` 发 `[4B 长度前缀][164B 头][data_size 载荷]`。**头只到 `data` 为止，loaned 指针绝不上线。**

跨进程订阅者的**回灌**（`transport.c:326-357`）有一个缺陷：`:356` 直接用 `msg->data` 而非 `message_bus_message_data()`，且**重新 publish 时丢失 `type_id` / `schema_hash` / `schema_version`**。

### 8.1 与第 07 章（共享内存）的边界

| 维度 | MessageBus（本章） | IpcChannel（第 07 章） |
|---|---|---|
| 作用域 | 单进程 | 同机跨进程（`shm_open`/`mmap`） |
| 内存 | 堆 `Message*` + 池 | 共享内存环 |
| 满时 | 按 QoS 丢/等 | **永不阻塞，直接覆盖最旧槽** |
| 借用 | ✅ `publish_loaned` | ❌ 槽里只有 `data[]` |

---

## 9. 生产实况

| 指标 | 数值 |
|---|---|
| 发布话题数 | **33**（`config/pipeline.json`） |
| 订阅总数 | **≈ 110~120**（29 个模块 104 处 + BagWriter 通配 1 + 调度器每任务 1 + 各桥槽） |
| 订阅上限 | 256 |
| 总线结构体 | ≈ 800 KB（其中 `topic_entries[128]` 占约 700 KB） |
| 队列最坏占用 | 4 × 1024 × 65736 ≈ **256 MB** |
| 内存池 | 1024 × 65736 ≈ **64 MB** |

**`modules/` 里 `message_bus_subscribe` 的直接调用是 0 处**——全部经 `transport_subscribe` 转发（`transport.c:373`）。生产代码里 `message_bus_publish*` 有 88 处、27 个文件。

> ⚠️ **`transport_subscribe` 完全不检查 `message_bus_subscribe` 的返回值，永远 `return 0`。** 订阅表满（>256）时节点静默收不到任何消息，只有 `message_bus.c:1170` 一条 WARN。

---

## 10. 测试与死代码

### 10.1 测试

**`tests/test_new_modules.c`（8 个，ctest `new_module_tests`）**：

| 测试 | 行 | 覆盖 |
|---|---|---|
| `test_bus_pub_sub_basic` | `:78-95` | 基本收发 |
| `test_bus_pub_sub_multi` | `:97-116` | 多话题多订阅 |
| `test_bus_wildcard_subscribe` | `:118-132` | `"*"` 匹配全部 |
| `test_bus_remap` | `:157-171` | 话题重映射 |
| `test_bus_zero_copy_basic` | `:173-189` | 同步投递 |
| **`test_bus_loaned_async`** | **`:218-248`** | **loaned 全生命周期**：投递=1、**释放=1**、**`_loaned_data == NULL`** |
| `test_bus_req_reply` | `:257-277` | 请求/应答往返 |

**`tests/test_modules.c` 有 6 个 QoS 测试**（`:755-853`），包括 `test_bus_qos_drop_oldest` 断言「40 条突发后最新一条必达」、`test_bus_qos_lifespan`、`test_bus_qos_deadline_violations`。

**`tests/coro_correctness_test.cpp` 的 `test_stress_concurrency`（`:618-712`）** 是专为 ASAN/TSAN 写的：8 轮，每轮一个 `BusQueueBridge` + `recv_any_for(2000)` 与 300 µs 间隔的高频发布竞争，外加随机时刻 `set_stop()`。

**`tests/test_bridges.c:114-138`** 走 `transport_*` 间接验证 bus。

### 10.2 死代码

| 符号 | 位置 | 状态 |
|---|---|---|
| `MessageBus::reply_mutex` | `:251` | 只 init/destroy，**从未 lock** |
| `RingBuffer::not_full` | `:81,423` | 只 signal，**无任何 wait** |
| `MSG_TYPE_REPLY` 分支 | `:611-624` | **不可达**——唯一给它赋值的是栈上局部变量 |
| `topic_entries[].lat_ring` | `:266` | 注释自称「兼容字段，勿在热路径直写」，**从未读写** |
| `message_bus_peek_latest` | `:1618-1646` | **零调用** |
| `message_bus_topic_pending` / `_is_full` | `:1578` / `:1592` | **零调用** |
| `message_bus_resolve_topic` / `_remove_remap` | `:1722` / `:1702` | **零调用** |
| `message_bus_publish_typed` | `:792` | **零生产调用**（详见第 06 章） |
| 整个 zero-copy 子系统 | `.h:223-288` | `modules/` 与 `src/` 零使用 |
| `MessageBus::name` | `:215,695` | 写入后从不读取 |

### 10.3 内存注释过时

`message_bus.h:39` 写「Message 队列 1024×~64KB ≈ 64MB」——**低估 4 倍**。实际是 4 分片 × 1024 = 4096 槽 ≈ 256 MB，加上 1024 块内存池 ≈ 64 MB，**最坏约 320 MB**。

---

## 11. 源码与资源对照

| 模块 | 源码文件 | 核心符号 | 职责与要点 |
|---|---|---|---|
| **消息总线** | [`include/message_bus.h`](file:///home/caixuf/code/FlowEngine/include/message_bus.h)<br>[`src/core/message_bus.c`](file:///home/caixuf/code/FlowEngine/src/core/message_bus.c) | `Message`（`h:54-84`）<br>`message_bus_publish`（`.c:787`）<br>`message_bus_publish_loaned`（`.c:1008`）<br>`message_bus_publish_zero_copy`（`.c:1413`）<br>`message_bus_message_data`（`h:86-88`） | 65736 B 信封。**4 分片 × 1024 + 4 分发线程**，`shard_for = msg_id % 4`。三条投递路径语义完全不同。seqlock 保护订阅表 |
| **QoS** | `message_bus.h:314-343` | `QosReliability` / `QosPolicy` / `TopicQos` | 5 个枚举值（**旧稿 4 个名字全错**）。`QOS_BLOCK` 是**忙等 1s/5s**，不是条件变量。**丢弃也返回 0** |
| **内存池** | `message_bus.c:123-156, 303-306` | `MsgPool` / `msg_alloc` / `msg_pool_push` | 上限 1024（≈64 MB）。**池空 malloc、池满 free**——「零分配」不成立 |
| **传输层** | [`src/core/transport.c`](file:///home/caixuf/code/FlowEngine/src/core/transport.c) | `transport_publish`（`:273-297`）<br>`transport_publish_loaned`（`:299-314`）<br>`transport_subscribe`（`:359-416`） | 恒先投本地。**借用在 `ROUTE_LOCAL` 时终止**。`transport_subscribe` **丢弃 `message_bus_subscribe` 的返回值** |
| **跨机** | [`src/cpp/network_transport.cpp`](file:///home/caixuf/code/FlowEngine/src/cpp/network_transport.cpp) | `bridge_outbound_cb`（`:194`）<br>`recv_thread_fn`（`:300-335`） | 紧凑帧 `[4B 长度][164B 头][载荷]`。`static_assert` 把 `offsetof(data)=164` 钉死在 $(0,256]$ |
| **协程桥** | [`include/coroutine_task.h:607-826`](file:///home/caixuf/code/FlowEngine/include/coroutine_task.h) | `BusQueueBridge::try_take` / `try_take_any` / `recv_any_for` / `reconnect` | 订阅提升到节点生命周期。`message_bus_copy_message` 解决 loaned 寿命问题 |
| **Req/Reply** | `message_bus.c:93-101, 641-678, 1262-1335` | `ReplySlot` / `message_bus_register_service` / `message_bus_request` | 16 个 pending 槽。**`svc_mutex` 持锁执行用户 handler** |
| **统计** | `message_bus.h:298-361` | `stat_published` / `stat_delivered` / `stat_dropped`<br>`TopicStats`（p50/p99 延迟、频率 EWMA） | 延迟用**墙钟**。被 `monitor_node` / `stats_bridge` / `flowmond` 消费 |
| **验证工具** | [`examples/ipc_channel/chapter04.c:217`](file:///home/caixuf/code/FlowEngine/examples/ipc_channel/chapter04.c) | `run_layout`（`./build/bin/ipc_chapter layout`） | **现场打印 `sizeof(Message)` 和全部关键字段偏移**——本章表格的实测来源 |
| **测试** | [`tests/test_new_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_new_modules.c)<br>[`tests/test_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_modules.c)<br>[`tests/coro_correctness_test.cpp`](file:///home/caixuf/code/FlowEngine/tests/coro_correctness_test.cpp) | ctest `new_module_tests`（8）<br>6 个 QoS 测试（`:755-853`）<br>`test_stress_concurrency`（ASAN/TSAN） | loaned 生命周期有完整测试。**peek/pending/is_full/zero_copy 子系统零覆盖** |

---

## 12. 思考题

1. **回调不能阻塞，但分片是随机的**：
   第 3.3 节说单条消息的 N 个订阅者串行同步执行；3.2 节说同一话题跨消息可能被两个线程并发回调。两者叠加产生一个尖锐问题：**`control_node` 的 `on_fusion` 回调里如果做了耗时计算，它会阻塞谁？**
   (a) 先算清楚：一条 `sensor/lidar` 消息（20 Hz）如果落在分片 0，而 `control/cmd`（40 Hz）的消息落在分片 1，两者会互相阻塞吗？结合 `msg_id % 4` 分析。
   (b) 假设 `on_fusion` 里有 5 ms 的耗时操作。在 20 Hz 的 `fusion/localization` 流上，这个阻塞会传播到哪里？请给出「一条消息卡 5 ms」对 4 个分片、4096 个槽位、110+ 个订阅者的具体影响。
   (c) 设计一个改进方案。考虑三个选项：(i) 每个订阅者一个线程；(ii) 按 topic 而不是 `msg_id` 分片，让同话题串行；(iii) 保持现状但在回调前检查预算。评估每个的代价——特别是 (ii)：它会让「同话题跨消息并发」变成「同话题永远串行」，这对**同话题的多个订阅者之间**（比如 `control/cmd` 同时被 safety 和 flowsim 订阅）意味着什么？

2. **loaned 的五条释放路径**：
   第 4.2 节列了 `release_fn` 被调用的五个位置。任何一条漏掉就是内存泄漏，任何一条**多调**就是 double-free。`msg_release_loaned`（`:158-167`）靠「清空三字段」实现幂等。
   (a) 请审查这五条路径 + `message_bus_destroy` 的排空逻辑（`:365-379`），论证幂等性是否真的覆盖了所有时序。特别是：**如果 `release_fn` 在多线程下被并发调用**（同一个 loaned 消息被两个分发线程同时处理——参考 3.2 节的分片随机性）会怎样？
   (b) QoS 丢弃路径返回 0 的 bug（`:1085`、`:1097`）已经导致 `stereo_camera_node.c:324` 把丢帧计成成功。设计一个修复：(i) 让 `publish_loaned` 在丢弃时返回 `ERR_OVERFLOW`；(ii) 或者返回一个能区分「已投递」和「已丢弃」的返回码。评估 (ii) 对现有 1 个调用点的改动成本，以及返回值语义变化会不会破坏 `publish` 的一致性。
   (c) 更根本的问题：**为什么只有双目相机这一个节点用 loaned？** 它是 44828 字节，拷贝成本是 44 KB 的内存写——在 20~30 Hz 下完全可接受。请论证：在 CPU 已经这么忙的情况下，为一个唯一的调用点维护一整套 loaned 机制（三条释放路径、一个幂等守卫、一份专门的文档说明、20 处 `message_bus_message_data()` 调用）是否划算？什么条件下这个权衡会反转？

3. **`QOS_BLOCK` 的忙等**：
   第 5.2 节说 `QOS_BLOCK` 不用条件变量而是 1 ms 轮询、上限 5 s。但 `RingBuffer` **已经有 `not_empty` 和 `not_full` 两个条件变量**——虽然 `not_full` 从未被 `wait`（第 10.2 节）。
   (a) 把 `QOS_BLOCK` 改成条件变量等待，需要动几处？请具体指出：条件变量在哪个 `RingBuffer` 上（4 个分片各有独立的？还是共享一个？）、`QOS_BLOCK` 的语义是「全局 `pending_count` 超限」还是「某个分片满」——这决定了等待哪个信号量。
   (b) 当前的忙等有一个「优点」：**不持任何锁**。如果改成条件变量等待，必然要持 `shards[i].mutex`，这会引入第 6.2 节说的哪一类新锁序风险？`publish` 当前已经持了哪些锁？
   (c) 一个替代方案：**不改 `QOS_BLOCK`，而是让所有生产配置都不用它**（当前 `config/pipeline.json` 里确实一条都没有）。请论证：如果「阻塞发布者」这个语义在自动驾驶里本来就是危险的（发布者被挂 5 秒意味着传感器数据在总线上排队到过期），那么正确的做法是不是**直接删掉 `QOS_BLOCK` 这个枚举值**，让任何配置写 `"policy":"block"` 都在解析期报错？对比「删掉」和「保留但不用」两种选择对代码库可信度的影响。

4. **零拷贝子系统的去留**：
   第 4.3 节说 `publish_zero_copy` / `subscribe_zero_copy` / `unsubscribe_zero_copy` 三个接口在生产代码里零使用，而且 `publish_zero_copy` 还有一个**违反直觉的语义**：在发布者线程里同步执行回调，还额外再投一份给普通订阅者。
   (a) 这个「同步 + 额外再投」的组合，如果被误用会怎样？设计一个场景：某个开发者以为 `publish_zero_copy` 是「只投给 zc 订阅者」，于是用它发布一条大数据。结果是 (i) 消息被投递几次？(ii) 回调在哪个线程执行？(iii) 载荷的释放责任在谁？
   (b) 统计 `message_bus.h:223-288` 那 65 行的 zc API 在整个仓库（排除 demo/benchmark/test）的引用数是 0。`publish_loaned` 引用数是 1。**按「使用次数」和「代码量」的比率，你建议删掉哪一个？** 论证标准。
   (c) 最后一个问题：把这三个 API 连同它们的 65 行、加上 `publish_loaned` 的三条释放路径一起删掉，只保留 `publish`（一律拷贝），会让 `MSG_BUS_MAX_DATA_SIZE` 从 65536 降回 4096 吗？请查 `StereoFrame` 的实际大小（44828 字节）并说明：**如果没有 loaned，双目相机的 44 KB 载荷要怎么处理？** 这个问题有没有不依赖 loaned 的解法？
