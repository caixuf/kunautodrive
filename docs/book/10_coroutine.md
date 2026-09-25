# 第 10 章：用 C++20 协程拆掉回调地狱

写自动驾驶节点，最难忍的不是算法，是等待。一个融合节点要同时等点云、等 GPS、等定位服务的应答，每一路等待还得各自配一个超时看门狗。在传统的异步 C 写法里，这一整段业务会被拆成几十个回调函数，状态散落在全局变量之间，同步和停机的清理顺序稍微写错，就是死锁或者内存泄漏。

KunAutoDrive 的 FlowCoro 换了个做法：把 `MessageBus` 的 Pub/Sub、Req/Reply、Timer 和多路 Select 都包成标准 awaitable 原语，于是等待可以写成从上往下读的直序代码——看上去是同步的，跑起来不占任何线程。

## 同一段等待，两种写法

先看左边那种写法的后果，再看右边这种写法为什么能读得下去。

```
传统回调驱动 (切碎的逻辑与散落的状态):
┌──────────────────────────────────────────────────────────┐
│  void on_lidar(const Message* m, void* ctx) {            │
│      ctx->got_lidar = true;                              │
│      if (ctx->got_gps) trigger_fusion(ctx);              │
│  }                                                       │
│  void on_gps(const Message* m, void* ctx) {              │
│      ctx->got_gps = true;                                │
│      if (ctx->got_lidar) trigger_fusion(ctx);            │
│  }                                                       │
│  缺陷：超时看门狗、异常重试、优雅停机代码极度晦涩冗长。  │
└──────────────────────────────────────────────────────────┘

FlowCoro C++20 协程驱动 (直序、清晰、确定性):
┌──────────────────────────────────────────────────────────────┐
│  Task run() override {                                       │
│      while (!should_stop()) {                                │
│          // 50ms 超时等待点云，超时自动触发看门狗            │
│        auto r = co_await next_for("sensor/lidar", 50000);    │
│        if (r.timed_out()) { watchdog_alert(); continue; }    │
│          auto pose = co_await ask("service/locate", req);    │
│          publish("fusion/result", compute(*r, pose));        │
│      }                                                       │
│  }                                                           │
│  优势：代码自上而下直叙；挂起时不占 CPU 线程；无锁优雅停机。 │
└──────────────────────────────────────────────────────────────┘
```

## 无栈协程在编译器眼里是什么

C++20 协程是无栈协程。编译器在编译期把含 `co_await` 的函数改造成两样东西：一个堆上分配的协程状态帧（coroutine frame），和一个内部有限状态机。所谓挂起，并不是把函数卡在那里，而是把现场写进状态帧，然后返回。

```
FlowCoro 执行模型:
┌─────────────────────────────────────────────────────────────┐
│  RtExecutor (每个 Worker 线程独占一个确定性执行器)          │
│                                                             │
│  ┌───────────────┐     spawn()     ┌──────────────────────┐ │
│  │ 准备队列       │ ──────────────►│ 正在执行的 Coroutine │ │
│  │ (Ready Queue) │                 │ (执行至 co_await)    │ │
│  └───────▲───────┘                 └──────────┬───────────┘ │
│          │                                    │             │
│         │ post_ready() 唤醒                  │ await_suspend│
│          │                                    ▼             │
│  ┌───────┴────────────────────────────────────────────────┐ │
│  │             MessageBus / Timer 挂起等待监听器          │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 执行器怎么跟着协程走

为了让多线程下的调度结果是确定的，每个 Worker 线程都维护一个自己的 `g_node_exec` 线程局部变量：协程执行到 `await_suspend(handle)` 的那一刻，从当前线程的 TLS 里读出 `RtExecutor*`，记在 awaitable 内部。之后外部总线分发线程收到消息、要唤醒这个协程时，走的不是 resume，而是调用当初记下的 `exec_->post_ready(handle)`，把协程投递回它自己的执行器线程。跨线程直接 resume 引发的那类竞态，就这样被挡在了门外。

## 可以 co_await 的五样东西

这张表是 FlowCoro 针对通信场景定的原语，日常代码里出现的基本就这几个：

| 原语 | 功能语义 | 典型应用场景 |
| :--- | :--- | :--- |
| `co_await next(topic)` | 阻塞挂起直到该 Topic 收到下一帧消息 | 传感器数据周期订阅 |
| `co_await next_for(topic, timeout_us)` | 带微秒超时的消息挂起 | 传感器丢帧看门狗防死锁 |
| `co_await select({t1, t2, ...})` | 多路话题竞争挂起，首个到达者唤醒 | 激光雷达与毫米波雷达首帧触发 |
| `co_await ask(service, req)` | 异步 RPC 请求挂起直到收到响应 | 路径规划向高精地图查询车道 |
| `co_await sleep_us(duration_us)` | 协程定时休眠（不阻塞系统底层线程） | 控制循环高频降采样定频 |

## 写一个带超时和多路选择的融合节点

把原语拼起来，一个节点的主循环就这么长。这段代码里同时用到了 `select`（点云和毫米波雷达谁先到先处理谁）、`ask_for`（20ms 超时的 RPC 查询）和 `sleep_us`（定频 100Hz）：

```cpp
/* modules/adas_nodes/coro_fusion_node.cpp */

#include "coroutine_task.h"
#include "logger.h"

class CoroFusionNode : public CoroutineTask {
public:
    CoroFusionNode(const TaskConfig* cfg) : CoroutineTask(cfg) {}

protected:
    Task run() override {
        LOG_INFO("CoroFusion", "协程任务已启动，进入主事件流循环...");

        while (!should_stop()) {
            // 1. 等待点云或雷达数据到达（谁先到处理谁）
            auto sel = co_await select({ "sensor/lidar", "sensor/radar" });
            if (sel.cancelled()) break;

            if (sel.matched("sensor/lidar")) {
                LOG_INFO("CoroFusion", "收到激光点云, 序号: %u", sel.message.msg_id);
            } else if (sel.matched("sensor/radar")) {
                LOG_INFO("CoroFusion", "收到毫米波雷达, 序号: %u", sel.message.msg_id);
            }

            // 2. 向定位服务发起 RPC 查询当前全局位姿
            Message req;
            auto rpc_res = co_await ask_for("service/localization", "coro_fusion", &req, sizeof(req), 20000 /* 20ms 超时 */);
            if (rpc_res.timed_out()) {
                LOG_WARN("CoroFusion", "定位服务超时，降级为航位推算");
            } else {
                LOG_INFO("CoroFusion", "融合定位成功对齐");
            }

            // 3. 定频休眠 10ms (100Hz)
            co_await sleep_us(10000);
        }

        LOG_INFO("CoroFusion", "协程任务安全退出");
    }
};

EXPORT_COROUTINE_TASK(CoroFusionNode, coro_fusion_node)
```

## 挂起的协程，怎么保证只被唤醒一次

协程挂在那里等消息的时候，宿主进程可能恰好下发了 `stop()`。消息到达、超时定时器触发、停机信号，这三件事是可能同时发生的，而它们之中只能有一个真正生效。

`coroutine_task.h` 里的 `AwaitCtl` 就是管这件事的：一个基于 CAS（Compare-And-Swap）的原子恢复守卫。三路唤醒源要竞争同一个恢复权，CAS 保证同一个挂起句柄 `std::coroutine_handle<>` 在它的生命周期里有且仅被 resume 一次。少了这道守卫，两次恢复撞在一起就是段错误。

## 顺手跑一遍 20Hz 心跳

`src/rt_heartbeat_demo.cpp` 用的是和生产节点相同的 `flowcoro::rt::RtExecutor` + TLS `g_node_exec`，以 `rt::sleep_until` 对齐 20Hz，打印 tick 间隔 / tardiness，然后 `request_stop` + `shutdown` 退出。可以拿它和上游 `flowcoro/examples/autonomous_driving/rt_control_loop_demo.cpp` 对照着看。

```bash
cmake --build build --target rt_heartbeat_demo
./build/bin/rt_heartbeat_demo 2
```

Sibling flowcoro：把仓库 clone 到 `../flowcoro` 后 configure，CMake 会优先用本地头，无需 FetchContent。

## 协程里翻过的三次车

### 挂起点前面留了个裸引用

现象很干脆：节点跑上几分钟，进程毫无征兆地 SIGSEGV，可一旦放慢复现节奏又什么都看不出来。最后是在代码里找到了这一段：

- 当时的写法：
  ```cpp
  // 错误代码:
  auto& ref = get_local_struct();
  co_await next("sensor/lidar");
  process(ref); // 灾难！挂起后局部栈帧可能已失效或被重新分配
  ```

问题出在 `ref`：它指向一个局部结构体，`co_await` 挂起之后那个栈帧可能已经失效或者被重新分配，再拿 `ref` 用，读到的就是垃圾内存。改法很简单也很死板：所有要跨越挂起点的持久变量，要么存成类的成员变量，要么按值拷贝一份。

### 在总线回调线程里直接 resume

`resume()` 会在当前调用者线程上立即同步执行协程后续代码。如果这条路径是总线工作线程走的，一个重型算法就能把整条总线堵在那里——现象是所有节点的消息延迟一起抬高，而不是某一个节点变慢。

改法是把任务交还给协程自己的 Worker 线程：调 `exec_->post_ready(handle)`，而不是 `handle.resume()`。这也是上面那句「记下自己所属的执行器」真正的用处。

### 同一个句柄被 resume 两次

前面说过 `AwaitCtl` 的 CAS 守卫，这里说它还没写出来之前的样子：进程偶尔崩在 resume 的调用点上，core dump 里两个线程的栈都停在同一个 `coroutine_handle` 上——消息到了、超时也到了，两边都认为自己该唤醒这个协程。修法就是给恢复权加一把原子锁，让三路唤醒源去竞争，输的那两路直接放弃。
