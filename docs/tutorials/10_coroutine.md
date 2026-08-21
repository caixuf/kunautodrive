# 第 10 章：C++20 协程通信框架 FlowCoro

> **本章导读**：
> 在自动驾驶系统中，节点往往需要同时等待多个异构传感器数据（如等待点云、等待 GPS、等待控制应答、设置超时看门狗）。在传统的异步 C 语言编程中，这通常会导致严重的**回调地狱（Callback Hell）**——业务状态被强行打散到数十个全局变量与回调函数中，状态同步和停机清理极易引发死锁与内存泄漏。
>
> FlowEngine 构建了基于 **C++20 原生协程（Coroutines TS）** 的高性能通信运行时 **FlowCoro**。通过将 `MessageBus` 的 Pub/Sub、Req/Reply、Timer 与多路 Select 封装为标准 Awaitable 原语，开发者可以用**同步直序（Sequential）的代码逻辑编写高性能非阻塞异步系统**。

---

## 1. 编程范式转移：从回调地狱到协程直序

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
│  缺陷：超时看门狗、异常重试、优雅停机代码极度晦涩冗长。 │
└──────────────────────────────────────────────────────────┘

FlowCoro C++20 协程驱动 (直序、清晰、确定性):
┌──────────────────────────────────────────────────────────┐
│  Task run() override {                                   │
│      while (!should_stop()) {                            │
│          // 50ms 超时等待点云，超时自动触发看门狗        │
│          auto r = co_await next_for("sensor/lidar", 50000);│
│          if (r.timed_out()) { watchdog_alert(); continue; }│
│          auto pose = co_await ask("service/locate", req);│
│          publish("fusion/result", compute(*r, pose));    │
│      }                                                   │
│  }                                                       │
│  优势：代码自上而下直叙；挂起时不占 CPU 线程；无锁优雅停机。 │
└──────────────────────────────────────────────────────────┘
```

---

## 2. C++20 协程底层机理与 FlowCoro 执行器

C++20 协程是**无栈协程（Stackless Coroutines）**。编译器在编译期将包含 `co_await` 的函数转换为一个由堆分配的协程状态帧（Coroutine Frame）和一个内部有限状态机。

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
│          │ post_ready() 唤醒                  │ await_suspend│
│          │                                    ▼             │
│  ┌───────┴────────────────────────────────────────────────┐ │
│  │             MessageBus / Timer 挂起等待监听器          │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 线程局部执行器绑定（TLS 机制）
为了确保多线程环境下的调度确定性，每个 Worker 线程维护一个唯一的 `g_node_exec` 线程局部变量（Thread Local Storage）：
- 协程在 `await_suspend(handle)` 执行瞬间，从当前线程 TLS 读取 `RtExecutor*` 并记录在 Awaitable 内部；
- 当外部总线分发线程收到消息触发回调时，直接调用记录的 `exec_->post_ready(handle)` 将协程投递回原执行器线程，**杜绝跨线程直接 resume 导致的竞态条件**。

---

## 3. 核心 Awaitable 原语家族

FlowCoro 提供了一套针对自动驾驶通信量身定制的 Awaitable 原语：

| 原语 | 功能语义 | 典型应用场景 |
| :--- | :--- | :--- |
| `co_await next(topic)` | 阻塞挂起直到该 Topic 收到下一帧消息 | 传感器数据周期订阅 |
| `co_await next_for(topic, timeout_us)` | 带微秒超时的消息挂起 | 传感器丢帧看门狗防死锁 |
| `co_await select({t1, t2, ...})` | 多路话题竞争挂起，首个到达者唤醒 | 激光雷达与毫米波雷达首帧触发 |
| `co_await ask(service, req)` | 异步 RPC 请求挂起直到收到响应 | 路径规划向高精地图查询车道 |
| `co_await sleep_us(duration_us)` | 协程定时休眠（不阻塞系统底层线程） | 控制循环高频降采样定频 |

---

## 4. 实战：编写一个带超时与多路选择的融合协程节点

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

---

## 5. 优雅停机与无锁并发恢复保护（CAS Resume Guard）

在协程被挂起等待消息的同时，若宿主进程发起了 `stop()` 停机指令，如何保证协程干净退出而不发生资源泄漏？

FlowEngine 在 `coroutine_task.h` 中实现了 **CAS（Compare-And-Swap）原子恢复守卫 `AwaitCtl`**：

- **互斥唤醒**：消息到达、超时定时器触发、外部停机信号三者并发竞争恢复权；
- **唯一恢复保证**：原子 CAS 确保同一个挂起句柄 `std::coroutine_handle<>` 在其生命周期内**有且仅被 resume 一次**，彻底消除了“双重恢复（Double Resume）引发的段错误”。

---

## 6. 工业级避坑指南

### 避坑 1：严禁跨 `co_await` 捕获局部变量的裸引用（Dangling Reference）
- **致命陷阱**：
  ```cpp
  // 错误代码:
  auto& ref = get_local_struct();
  co_await next("sensor/lidar");
  process(ref); // 灾难！挂起后局部栈帧可能已失效或被重新分配
  ```
- **黄金准则**：跨越 `co_await` 挂起点的所有持久变量，必须作为类的成员变量存储，或使用值传递（By-Value Copy）。

### 避坑 2：禁止在异步回调线程直接调用 `handle.resume()`
- `resume()` 会在当前调用者线程立即同步执行协程后续代码。如果在总线工作线程直接 resume，会导致总线工作线程被重型算法占用而阻塞整条总线。必须通过 `exec_->post_ready(handle)` 将任务交还给专属 Worker 线程。

---

*下一章预告：第 11 章将探讨任务调度核心——DAG 有向无环图依赖流与多核 CPU 亲和性调度器。*
