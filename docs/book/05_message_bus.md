# 第 05 章：节点之间搬运数据的那条线

> **v2 范式章节**（2026-09 整治后保留）。本章保留低行号密度、真技术书
> 风格，参照 `docs/book/README.md` 写作风格约束与 `docs/book/09_discovery.md`
> 范式示范。v1 行号清单版本归档于 `docs/_archive/book_v1/03_message_bus.md`。

这一章在第一卷　微内核与系统编程。
上一篇讲过参数和注册中心（见第 03 章）——那是节点对自己内部状态的访问。
这一篇讲一条贯穿整个管线的"血管"：节点与节点之间搬运数据的话题（topic）
总线。
代码层名字是 `message_bus`，对外暴露给节点的就是一条同步的 pub/sub。

整套系统最大的设计选择题不是"要不要总线"，而是"总线放哪儿"。
我们有三档候选：

1. **进程内总线**——所有节点 `dlopen` 进同一进程，bus 是一组全局表
   （`message_bus.h` 接口）。
2. **单机 IPC**——每个节点独立进程，bus 在进程间用 AF_UNIX（参数那条线）
   或共享内存（详见第 07 章）传递。
3. **网络**——多机部署，需要 TCP/UDP 转发。

我们做的是**前两条同进程可无缝切换**——这恰好是 plugin loader 模式下默认
走进程内、`--multi` 模式下走 IPC 通道的设计意图。

下面分三部分展开：

- 这条总线**对节点像什么**——`publish_loaned`、`subscribe`、通配符等
  几个 API 的语义。
- 它**内部**做了什么——分片环形缓冲、回调分片、零拷贝消息池。
- 它会**在哪些时刻翻车**——历史 v0 注释里被反复回炉的若干类 bug。

## 节点眼里的总线：发布、订阅、通配、同步

节点开发者真正用到的 API 大约六个。

```
publisher ──publish()/publish_loaned()──▶ topic ──subscriber.callback()──▶ consumer
                              │
                              ▼
                          req_call()        ←──►        req_reply()
                          (sync, with timeout)
```

`message_bus_publish(bus, topic, payload, len)`：阻塞语义由 topic 的
`qos_policy` 决定，默认 `QOS_DROP_OLDEST` + best effort。
`publish_loaned(bus, topic, msg)`：把已经预分配的消息放进队列，避免
malloc，零拷贝路径。

订阅：`message_bus_subscribe(bus, topic, callback, user_data)`；
`message_bus_subscribe_ex` 给协程用法用，回调返回时统计 in-flight。
取消订阅要等所有 in-flight 回调返回后才放行——这是为了**避免回调访问
已释放的 user_data**——协程 `awaitable` 在 `await_resume` 反注册后立即析构，
若不等到 in-flight 归零就会 use-after-free。

通配符 `*` 订阅所有 topic：调试用，生产慎用——总线不知道你要过滤掉哪些
topic，CPU 会全吃。

同步请求/响应：`req_call(topic, req, &reply, timeout_ms)`——总线在内部
维护一个 `ReplySlot[16]` 表，用 `req_id` 匹配。等价于节点之间的一次 RPC。
16 条上限是历史包袱：单线程序列化请求没问题，4 路并发就排队。
后续如果跑多 RPC 节点场景要考虑合并 slot 或加 caller hash。

## 总线内部：四个分片，零拷贝

总线主干是一条带 4 个分发线程的环形队列（`MSG_BUS_DISPATCH_THREADS=4`），
名字叫 shard。

```
producer  ─┐
producer  ─┤  msg_id % 4 ──▶ shard[0] (lock+cond)   ──▶ dispatch thread 0
producer  ─┤             ──▶ shard[1] (lock+cond)   ──▶ dispatch thread 1
producer  ─┤             ──▶ shard[2] (lock+cond)   ──▶ dispatch thread 2
producer  ─┘             ──▶ shard[3] (lock+cond)   ──▶ dispatch thread 3
                              │
                              ▼
                         callback(subscriber)
```

设计动机是**压单条队列的吞吐上限**。
原来的实现是一个 `bus` 拥有一个长队列 + 一个分发线程；高负载时
(mutex 竞争 + 单核) 上限 ~70K 条/秒。
切成 4 段后，按 `msg_id` 取模入队，各段互斥锁独立、各自一个 dispatch
线程——并发把吞吐抬到 ~280K 条/秒（同时把 CPU 烧上去了）。
**官方说明**：consumer 业务的 callback 还会再做递归互斥，所以单 producer
单 consumer 场景提升比这个数字看起来的更平。
参考数字不应作为"改成更多分片就一定能更快的依据"——4 条是经验值，不是优化解。

零拷贝消息池：

每条消息的载荷最大 64 KiB（`MSG_BUS_MAX_DATA_SIZE`）。
早期版本按值入队，结果每条消息进出队列各 memcpy 一次 64 KiB，压测下来
~70K 条/秒 ≈ 9 GiB/s memcpy——瓶颈全在内存搬运上。
现在改成"队列里放 `Message*`，载荷在堆上分配"。
后续又把每条 `malloc/free 64KiB` 用互斥保护的空闲链表摊薄成"取/还 O(1)"。
**注意**：早期无锁 Treiber 栈版本在高并发下触发 ABA——同一节点被两次并发
pop → double-alloc → 段错误。
现在用 mutex，回退到简单实现。

## QoS：发布端给三类提示

`topic_entries[].qos_flags` 在订阅时声明、发布侧读取。

```
QOS_FLAG_RELIABLE      (1u<<0)   // 等效 BLOCK，配合订阅者持有过载
QOS_FLAG_DROP_LATEST   (1u<<1)   // ring 满时丢新条
QOS_FLAG_BLOCK         (1u<<2)   // ring 满时阻塞发布者
默认（无位）= DROP_OLDEST + best_effort
```

`lifespan` 字段在 dispatch 端读：`now - msg.timestamp > lifespan` 的消息会被
直接跳过，不回调订阅者。
用途：传感器数据一帧过完就是过完，不要让慢消费者把过期帧回放给控制环。
**没有 lifespan 的 topic 不参与自动过期**——传感器显式声明 `lifespan_us`
之后才生效。

`QOS_DROP_OLDEST` 和 `QOS_DROP_LATEST` 的语义来自 DDS（OMG DDS spec
1.4），但**不是完整 DDS**——只实现了发布者侧的两类决策位，订阅者侧没有
显式 read/take 区分。
若节点需要历史采样回放，请用 bag（见第 06 章）和 topic-level remap（详见
下文）。

## Topic remap：调试期的"伪环境变量"

总线入口处的 remap 表允许把发出的 topic `A` 在订阅侧看到 `B`。

```
publisher: message_bus_publish(bus, "raw/lidar")
              │
              ▼
           remap table: {"raw/lidar" → "sensor/lidar"}
              │
              ▼
subscriber: callback sees "sensor/lidar"
```

`bus_add_remap(bus, "raw/lidar", "sensor/lidar")` 是配置时挂的，不是动态改的。
用途：bag 回放（详见第 06 章）记录原 topic，重放时 remap 到当前管线的
目标 topic。
这是 bus 唯一一个面向"配置而非运行时"的回调开关——发布端改 topic 名是
改不动的。

## in-flight 反注册：协程 awaiting 析构时如何不死

订阅回调运行期间可以再次调用 `message_bus_unsubscribe_ex`。
v0 实现里 unsubscribe 直接把 `SubEntry.active = false` 清掉——若此时
dispatch 线程正持有 `user_data` 执行回调，user_data 可能在协程 `awaitable`
析构后被 free，结果 use-after-free。

修复：`SubEntry.in_flight`（atomic_int）记录"现在有几个分发回调在 snapshot
里"。
`unsubscribe_ex` 把 active=false，**并 spin-wait 到 in_flight == 0** 才允许
返回。
`dispatch_message` 在快照期间 +1，回调返回后 -1。
这一条措辞很简单，但真实写出过 2 次段错误才稳下来——具体复现见下文
"我们踩过的坑"。

## Req/Reply：等价的 RPC，等价的上限

`message_bus_req_call(topic, req, &reply, timeout_ms)`：
caller 生成 `req_id`、在 `ReplySlot[16]` 里找一个空闲槽、把请求塞进
default shard（按 req_id 取模），等 `cond` 被 reply 唤醒或超时。
handler：`message_bus_register_service(topic, handler)` 接 reply。

上限 16 槽位是早期单车流程 RPC 的容量设计的。
多客户端并发调同一服务时，超过 16 个在途会被 caller 端的 "no slot" 路径
返回 `ERR_BUSY`。
**不做 priority queue**，不做 caller hash。
提高上限不难，但要不要——读者请先想清楚是不是真的有这个并发需求；
节点串行调用某个服务是常态。

## 我们踩过的坑

### 坑一：把消息池里的 Message 还回去时把它 free 了

零拷贝消息池的 `free_list` 取/还都加锁，看似简单。
有段时间我们用 Treiber 无锁栈：4 个生产者线程 + 4 个分发线程 + 驱逐压力
下，CAS 在 `head` 上的 `pop` 出现了**经典 ABA**：节点 A 出栈，节点 B
紧接着 push（同样的值，因为数据指针已经分配新的），节点 A 在它的 CAS 上
认为 top 没变并继续——结果丢一个节点，第二个 `pop` 把同一节点二次消费，
double-alloc。
修复：所有取/还走同一个 mutex；无锁栈版本在注释里标"@deprecated，
remove-by=历史已删"。详见 `docs/HANDOFF_2026-09-22.md`。

### 坑二：订阅回调在协程 awaitable 里被释放触发 use-after-free

`unsubscribe_ex` 没等 in-flight 归零就返回，结果 dispatch 线程还在调
协程 user_data 时协程已经析构——段错误。
bug 的"经典样本"在 v0 commit message 里就出现过几次。
修复：atomic `in_f_light` + spin-wait（详见上文）。

### 坑三：ring 队列满配 QOS_BLOCK 把控制环卡死

control 节点订阅 `perception/obstacles`，某次高负载下 ring 满，control
线程同步 `publish` 阻塞——结果上游 `fusion` 发送了 8 ms wait，control 还
以为上游死了。
v0 默认配置曾经把控制环相关 topic 都设 `QOS_BLOCK`："我宁愿阻塞也别丢"。
我们后来把所有控制环 + 感知环相关 topic 显式 `DROP_LATEST`——控制节点下
一帧仍能拿到（哪怕不是 100% 全量），发布者不会卡。
**这是技术决策，不是规范**：保留 `BLOCK` 选项给"丢不得"那些数据
（`vehicle/state` 这类单一权威源），其它会过期的（`sensor/lidar`
一帧）请用 `DROP_LATEST`。

### 坑四：通配符 `*` 把 publish 端也吞了

订阅用 `*` 等于听所有 topic。
v0 写法把通配符的语义放在订阅侧的"是不是字符串相等于 `*`"判断上。
结果发布者内部因为某种 debug log 调用而 self-publish 时，回调里又订阅了
`*`——回调里递归地再 dispatch 进去。
修复：通配符用户自己小心（这是文档一句话），但**不为这个写专门 debug
开关**——会引诱依赖。

## 在多线程模式下：IPC 通道才是真总线

`--multi` 模式（`scripts/demo.sh --multi`）下，节点是 fork+exec 出来的
独立进程，每个进程的 `message_bus` 实例不共享。
此时总线在 IPC 层走共享内存（详见第 07 章）——总线对象本身在两端各有一份
副本，靠 transport 层把 topic 名 + 序列化 payload 在分片队列之间搬运。

这里有一个**不在 v0 注释里**的细节：transport 包了一层回调适配——
进程内零拷贝消息池在 IPC 模式下被换成"序列化 + 反序列化"，所以 publish
`~8 µs/条` 的本地数字在 IPC 下要乘以序列化耗时（通常 1~3 倍，取决于
payload 大小和 schema 复杂度）。
单线程 demo 数字别照搬到 IPC 模式看。

## 测试：怎么验证总线是活的

`message_bus_smoke`（已有，在 `tests/`）：

1. 一个订阅，N 个生产者并发 publish；
2. 校验所有 callback 都收到，且 payload 内容一致；
3. 强杀一条订阅者 → 其余订阅者不受影响；
4. force unsubscribe 时校验 in-flight 等待 → 不抛错；
5. req_call 超时 1 ms → `ERR_TIMEOUT`，不污染后续 req_call。

## 思考题

1. `QOS_BLOCK` 配在 `vehicle/state` 这条 topic 上比配在 `sensor/lidar`
   上更"安全"——为什么？反过来呢？
2. 为什么不把 `flow_registry` 的元信息和 `g_params[]` 合并到一张表里？
   （提示：可变 vs. 不可变。）
3. 如果节点 A 给 `perception/obstacles` 的 callback 在执行到一半时
   重新订阅了同一 topic，会发生什么？

下一章看的是总线上"二进制那些玩意儿"：类型 ID、IDL 和序列化协议
（见第 06 章）。
