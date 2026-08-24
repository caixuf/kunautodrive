# 第 11 章：DAG 任务流与混合调度器（Choreo Scheduler）

> **本章导读**：
> 在自动驾驶系统中，各个算法任务存在严格的**先后拓扑依赖关系（DAG, Directed Acyclic Graph）**。例如：必须在“相机采集”和“激光雷达预处理”完成后，才能启动“多模态前融合”；而在“融合定位”输出后，才能并发触发“局部路径规划”与“速度规划”。
>
> 传统的独立线程轮询会导致严重的线程上下文切换开销与无序竞争。KunAutoDrive 设计了 **Choreo 混合调度器（Hybrid Scheduler）**：它结合了经典 FIFO 优先级队列与有向无环图依赖调度，支持 **CPU 核心亲和性绑定（CPU Affinity）、频率限制（RateControl）、资源配额（ResourceQuota）与微秒级延迟追踪（LatencyTracker）**。

---

## 1. 自动驾驶 DAG 拓扑依赖模型

```
自动驾驶 Pipeline DAG 调度拓扑图:
                     ┌───────────────────┐
                     │ 01. lidar_driver  │ (10Hz)
                     └─────────┬─────────┘
                               │
                               ▼
┌───────────────────┐┌───────────────────┐
│ 02. gnss_driver   ││ 03. lidar_cluster │ (DBSCAN 聚类)
└─────────┬─────────┘└─────────┬─────────┘
          │                    │
          ▼                    ▼
┌────────────────────────────────────────┐
│ 04. sensor_fusion (EKF 融合定位与跟踪) │
└───────────────────┬────────────────────┘
                    │
                    ▼
┌────────────────────────────────────────┐
│ 05. planning_node (Frenet 轨迹规划)    │
└───────────────────┬────────────────────┘
                    │
                    ▼
┌────────────────────────────────────────┐
│ 06. control_node (Stanley/MPC 跟踪控制)│
└────────────────────────────────────────┘
```

---

## 2. 调度器核心数据结构与 QoS 指标

```c
/* include/scheduler.h */

// 1. 频率控制器（防止高频传感器跑满 CPU）
typedef struct {
    uint64_t  period_us;        /**< 最小执行间隔（微秒） */
    uint64_t  last_run_us;      /**< 上次执行时间戳 (CLOCK_MONOTONIC) */
    double    max_frequency_hz; /**< 频率上限，如 50.0 Hz */
} RateControl;

// 2. 延迟追踪器（环形缓冲计算 P50 / P99 抖动）
#define LATENCY_BUFFER_SIZE 1024

typedef struct {
    uint64_t  recent[LATENCY_BUFFER_SIZE];  /**< 1024 样本环形缓冲 */
    uint32_t  head;
    uint32_t  count;
    uint64_t  sample_total;
    uint64_t  sample_count;
    uint64_t  min_us;
    uint64_t  max_us;
} LatencyTracker;

// 3. 资源配额与超额熔断
typedef struct {
    uint64_t  max_cpu_time_us;      /**< 单次执行最大 CPU 时间（超时则告警） */
    uint64_t  max_execution_count;  /**< 最大执行次数 */
    size_t    max_memory_bytes;     /**< 最大堆内存配额 */
} ResourceQuota;
```

---

## 3. 多核 CPU 亲和性绑定（CPU Affinity）

在 Linux RT 实时内核中，为了避免跨 CPU 核心缓存失效（L1/L2 Cache Miss）与线程抢占抖动，KunAutoDrive 支持将关键任务硬绑定到指定的 CPU 隔离核心（通过 `isolcpus` 内核启动参数保留的核心）：

```c
/* 绑定规划节点到 CPU Core 2 与 Core 3 */
uint32_t cpu_mask = (1 << 2) | (1 << 3);
scheduler_set_affinity(sched, planning_task_id, cpu_mask);
```

### 内部实现原理：
```c
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
for (int i = 0; i < 32; i++) {
    if (cpu_mask & (1 << i)) {
        CPU_SET(i, &cpuset);
    }
}
pthread_setaffinity_np(worker_thread, sizeof(cpu_set_t), &cpuset);
```

---

## 4. M:N 协程与线程池调度模型

KunAutoDrive 的调度器在底层维护一个高效的 Worker 线程池，多个异步 Task/Coroutine 被多路复用调度到预分配的线程池中执行：

```mermaid
flowchart TD
    A[就绪队列 Ready Queue (按 PRIORITY 排序)] --> B{M:N 调度分发器}
    B --> C[Worker Thread 0 (CPU 0)]
    B --> D[Worker Thread 1 (CPU 1)]
    B --> E[Worker Thread 2 (RT Core 2)]
    
    C --> F[执行 Task 01]
    D --> G[执行 Task 02]
    E --> H[执行 Task 03 (高优先级)]
    
    F --> I[记录耗时到 LatencyTracker]
    G --> I
    H --> I
    I --> J{检查 RateControl 频率}
    J -- 达到周期 --> A
```

---

## 5. 实战演练：注册任务并配置 QoS 策略

```c
#include "scheduler.h"

int main(void) {
    // 1. 创建调度器
    SchedulerConfig cfg = {
        .worker_count   = 4,
        .enable_dag     = true,
        .enable_metrics = true
    };
    Scheduler* sched = scheduler_create(&cfg);

    // 2. 注册规控核心任务
    int fusion_id = scheduler_register_task(sched, fusion_task, "sensor_fusion");
    int plan_id   = scheduler_register_task(sched, plan_task,   "planning_node");

    // 3. 配置优先级、CPU 亲和性与 50Hz 控频
    scheduler_set_params(sched, fusion_id, TASK_PRIORITY_HIGH, 0x04 /* CPU 2 */, 50.0);
    scheduler_set_params(sched, plan_id,   TASK_PRIORITY_REALTIME, 0x08 /* CPU 3 */, 50.0);

    // 4. 声明依赖关系：planning 依赖 fusion
    scheduler_add_dependency(sched, plan_id, fusion_id);

    // 5. 启动调度循环
    scheduler_start(sched);

    // ... 运行 10 秒 ...
    sleep(10);

    // 6. 获取延迟统计
    LatencyStats stats = scheduler_get_task_latency(sched, plan_id);
    printf("Planning 延迟指标: Avg=%lu us, P50=%lu us, P99=%lu us\n",
           stats.avg_us, stats.p50_us, stats.p99_us);

    // 7. 停止与销毁
    scheduler_stop(sched);
    scheduler_destroy(sched);
    return 0;
}
```

---

## 6. 工业级避坑指南

### 避坑 1：DAG 循环依赖检测（Cycle Detection）
- **隐患**：若业务配置不当出现 `A -> B -> C -> A` 的闭环依赖，调度器就绪队列将永远无法满足入度为 0 的触发条件，导致整个 Pipeline 永久锁死。
- **防护**：`scheduler_add_dependency` 在每次插入依赖边时，自动执行基于 **Tarjan 算法或拓扑排序（Kahn 算法）** 的环路检测，若发现有向环立即报错并拒绝配置。

### 避坑 2：优先级反转（Priority Inversion）与线程池饥饿
- **隐患**：低优先级任务（如日志落盘）占满了 Worker 线程池中的所有工作线程，导致高优先级的急停和控制任务无法被及时调度。
- **最佳实践**：为 `TASK_PRIORITY_REALTIME` 预留独占 Worker 线程，或使用实时内核调度策略 `SCHED_FIFO / SCHED_RR`。

---

*第二卷完结。下一章将进入【第三卷：ADAS 算法栈从理论到实现】，深入探讨多传感器前融合与扩展卡尔曼滤波（EKF）定位框架。*
