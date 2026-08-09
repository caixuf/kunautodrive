# 算法集成指南

## 定位

```
FlowEngine = 中间件框架（调度 + 通信 + 状态机 + 监控）
第三方库  = 算法实现（感知数学 + 融合数学 + 规划数学 + 控制数学）

FlowEngine 不做算法，只做算法的"插座"。
```

## 架构

```
┌───────────────────────────────────────────────────┐
│                   FlowEngine                       │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐   │
│  │ Scheduler│  │  State   │  │  Discovery    │   │
│  │ (Choreo) │  │ Machine  │  │  + Transport  │   │
│  └──────────┘  └──────────┘  └───────────────┘   │
│       │              │               │            │
│       ▼              ▼               ▼            │
│  ┌─────────────────────────────────────────────┐  │
│  │     中间件能力（2026-07-29 已接入）         │  │
│  │  degrade_ladder │ backpressure │ select_for │  │
│  │  Req/Reply      │ heartbeat    │ Choreo触发 │  │
│  └─────────────────────────────────────────────┘  │
│       │         │         │         │             │
│       ▼         ▼         ▼         ▼             │
│  ┌────────┐┌────────┐┌────────┐┌────────┐       │
│  │ Fusion ││Planning││Control ││Safety  │       │
│  │ Plugin ││Plugin  ││Plugin  ││Plugin  │       │
│  └────────┘└────────┘└────────┘└────────┘       │
└───────────────────────────────────────────────────┘
         │         │         │         │
         ▼         ▼         ▼         ▼
   ┌──────────────────────────────────────────┐
   │         Third-Party Libraries             │
   │  Eigen │ cJSON │ pthread │ flowcoro      │
   └──────────────────────────────────────────┘
```

## 中间件使用现状（2026-08-09）

| 能力 | 状态 | 详情 |
|------|------|------|
| **Pub/Sub** | ✅ 完全使用 | 全部 12 节点通过 `transport_publish/subscribe` 通信 |
| **Topic 发现** | ✅ 使用 | `discovery_advertise()` 在 init 时广播 |
| **心跳 + 降级阶梯** | ✅ 已接入 | 9 个原因码，supervisor 自动递进 L0-L3 |
| **反压检测** | ✅ 已接入 | control/safety_control 发布前检查 `topic_is_full` |
| **消息驱动 select_for** | ✅ 已接入 | control/planning/fusion 用 `select_for` 替代 sleep_us polling |
| **Req/Reply** | ✅ 已接入 | safety_control 注册 `safety/status` 服务，control 每 5s 查询 |
| **Choreo 调度** | ⚠️ 部分接入 | 默认 pipeline 已启用 `mode: choreo`；节点由 `node_start_managed()` 注册，输入 topic 已绑定 Choreo trigger。节点执行循环仍以 `select_for`/队列轮询为主，尚未全面改为 `scheduler_choreo_wait()` 数据驱动 |
| **零拷贝** | ⚠️ 核心可用 | Message Bus 零拷贝发布/订阅已由 `bus_demo` 和 benchmark 使用；ADAS 主流水线尚未接入 `message_bus_publish_zero_copy()`，跨进程 Transport 仍按各通道协议传输 |

### 零拷贝接入边界

当前 `message_bus_publish_zero_copy()` 是**发布者线程内同步回调**，借用指针只在
回调返回前有效，并且为普通订阅者仍会再发布一份拷贝。它不能直接替换异步
Transport，也不适合让感知算法在发布者线程中执行。

| Topic | 单帧规模 | 结论 |
|-------|----------|------|
| `sensor/stereo` | 固定约 44 KB | 首选候选；需先实现引用计数/loaned buffer 异步队列，并保留 `--multi` IPC fallback，再接 stereo_vision/traversability |
| `scene/frame` | 最高约 64 KB | 不直接接入；订阅者多且包含 JSON 解析，同步回调会阻塞 FlowSim |
| `sensor/lidar` | 当前 `LidarFrame` 仅约 24 B | 暂无收益；升级为真实点云契约后重新评估 |
| 控制、定位、GPS 等 | 数十至数百 B | 拷贝成本远低于调度和序列化成本，不接入 |

因此生产链路的下一步不是把现有 API 机械替换到节点中，而是增加异步
loaned-message 所有权模型：发布者提交 buffer、队列持有引用、最后一个消费者
释放；单进程走引用传递，多进程仍走 IPC 序列化/共享内存。没有生命周期门禁
前禁止让节点保存 `ZeroCopyCallback.data`。

## 接入步骤

### 1. 实现 AlgorithmInterface

```c
#include "algorithm_plugin.h"

static AlgorithmInterface my_interface = {
    .get_info      = my_get_info,
    .initialize    = my_init,
    .process       = my_process,
    .get_state_json = my_get_state,
    .set_param     = my_set_param,
    .destroy       = my_destroy,
};

AlgorithmInterface* get_algorithm_interface(void) { return &my_interface; }
const char* get_algorithm_version(void) { return "1.0.0"; }
```

### 2. 编译为 .so

```bash
gcc -shared -fPIC -I include my_algo.c -o libmy_algo.so \
    $(pkg-config --cflags --libs opencv4 eigen3)
```

### 3. Launch 配置加载

```json
{
  "nodes": [{
    "name": "perception_node",
    "plugin": "lib/libmy_algo.so",
    "scheduling": {"priority": "critical", "cpu_affinity": [0,1]},
    "subscribe": [{"topic": "sensor/camera"}],
    "publish": [{"topic": "perception/objects"}]
  }]
}
```

### 4. 状态机联动

```c
// 驾驶模式切换 → 自动激活/停用算法
algorithm_activate_for_mode(&sm, SM_MODE_CP, plugins);
// ACC 算法停用, LaneDetection + SteeringControl 激活
```

## 推荐第三方库映射

| 模块 | 推荐库 | 为什么 |
|------|--------|--------|
| **感知-2D检测** | OpenCV + ONNX Runtime | 轻量、跨平台、C API 友好 |
| **感知-3D检测** | TensorRT / OpenPCDet | GPU 加速、点云处理 |
| **融合-EKF** | Eigen | 头文件库、零依赖、C++ 模板 |
| **融合-因子图** | GTSAM / Ceres | 非线性优化、批量平滑 |
| **定位** | GTSAM / cartographer | 图优化、实时 SLAM |
| **预测** | 自研轻量 LSTM / Apollo Prediction | 轨迹预测 |
| **规划-路径** | OMPL / Apollo Planning | 采样规划、搜索 |
| **规划-速度** | 自研 ST 图 + DP/QP | 速度规划 |
| **控制-PID** | 自研（参考 `modules/adas_nodes/control_node.cpp`）| 最简单、好调试 |
| **控制-MPC** | OSQP / acados | 模型预测控制 |

## 参考实现

- `modules/adas_nodes/control_node.cpp` — PID 纵向 + Stanley 横向控制
- `src/plugins/example_process.c` — 示例进程插件
- `src/plugins/example_task.c` — 示例任务插件

## 不推荐的做法

- ❌ 在 FlowEngine 内部写复杂的数学运算
- ❌ 把算法编译进核心库（应该作为独立 .so 插件）
- ❌ 用 C 写矩阵运算（用 Eigen/Ceres 等经过验证的库）

## 推荐的做法

- ✅ 算法作为独立 .so，通过 AlgorithmInterface 接入
- ✅ 状态机负责模式编排（NA→ACC→CP→NP→NOA）
- ✅ FlowEngine 负责调度、通信、监控
- ✅ 第三方库负责数学运算
