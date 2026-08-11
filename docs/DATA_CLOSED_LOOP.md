# 数据闭环

KunAutoDrive 只维护一套可观测性采集内核，通过不同部署 profile 服务开发和量产，
而不是分别维护开发车与量产车两套采集栈。

## 部署 profile

| Profile | 用途 | 输出 |
|---|---|---|
| `development`（默认） | 仿真、可视化、事故复现 | 仪表盘 JSON、场景数据、samples、注册表和 IPC 桥接 |
| `production` | 车辆健康与车队数据采集 | 低频二进制基础设施、行程和事件记录 |

二者共用消息总线统计、`health`、`degrade_ladder`、时钟与事件语义。采样频率和
存储策略可以不同，但指标定义不得不同，以便离线评估与车端数据可直接对比。

## 闭环链路

```text
节点埋点
  -> 共用 topic/health 采集
  -> 指标与状态迁移记录
  -> CRC 保护、可轮转的二进制日志
  -> pem_dump.py 过滤与 JSONL 转换
  -> 事故分析、回放和评估
  -> 参数/模型更新
  -> 部署
  -> 使用同一套指标定义验证
```

每条二进制记录包含单调时钟和实时时钟时间戳、序列号、记录类型、关键标记与
CRC。实时时钟用于关联车外系统，单调时钟用于稳定的时段分析。关键状态迁移会
立即 `fflush` 和 `fsync`；新分段创建后同步其父目录；周期记录每 50 条或每秒
刷盘一次。记录布局只在 `include/pem_log_layout.def` 定义一次，由 C 写端和
`pem_dump.py` 共同使用。

PEM 按时间或单文件大小轮转。保留策略同时限制分段数量和目录总配额，打开新
分段后删除最早的已关闭分段，当前活跃分段永不删除。默认保留 96 个五分钟
分段和 1 GiB。写端以内部锁串行化公开操作；量产设计仍坚持每个流只有一个
writer，避免文件系统阻塞控制工作线程。

## PEM 有界指标/事件运行时

`modules/pem/` 是独立 PEM 模块：`pem_runtime.c` 是内存运行时，`pem_log.c` 是唯一
LogManager（轮转、CRC、fsync 和保留策略）。其公共 C API 位于
`include/pem_log.h` 和 `include/pem_runtime.h`（安装后为
`include/flowengine/`）；`pem_collector` 和 `monitor_node` 仅作为薄节点适配器，
提供采集策略和 durable-writer callback。monitor 的 production 基础设施流保持原有
system/topic/health/degrade 采集路径，只把写入前的 latest/event staging 接入同一运行时
API。

### 运行时分层

```text
TopicManager(JSON v1 配置订阅)
  -> 回调：校验长度 + 拷贝至预分配 MPSC pool
  -> 单个 pem_collector 协程：fault > metric > heartbeat
  -> MetricManager + WatchdogManager + EventManager
  -> LogManager(pem_log_write) / perf_diag JSON topic
```

| 区域 | 固定上限 | 行为 |
|---|---:|---|
| TopicManager | 16 个 topic | 从 `pem_runtime.schema_version=1`、`topics[]` 加载 |
| MPSC 输入池 | 96 条 × 512 B | 预分配、互斥保护的 MPSC；回调不解析、不分配、不发布、不写盘；fault 预留 16 槽、metric 再预留 16 槽，满或超长消息计入对应优先级的 drop |
| 优先级 | 3 队列 | `fault` 先于 `metric`，先于 `heartbeat`；同优先级 FIFO |
| latest metric cache | 128 个 `(record_type, name)` | 连续值覆盖为最新快照 |
| durable event FIFO | 64 条 | 满时拒绝新事件，保留尚未写入的旧事件 |
| 单次 durable drain | 当前最多 192 条 | 事件先于 dirty metric；写失败保留当前记录，下次重试 |

MetricManager 在 pipeline 线程计算：每 topic 一秒滑动窗口 FPS、发布→回调消息延迟、
发布→pipeline 端到端延迟、入池→处理排队延迟；现有 GPS/定位里程和行驶时长计算也移动到
该线程。每次 flush 还输出 input-pool 负载和丢弃计数。

WatchdogManager 输出 `watchdog:process` 与所有配置了 `heartbeat_timeout_ms` 的
`watchdog:<topic>` health 快照；超时和恢复都会生成 fault transition。它是**进程内**
pipeline/heartbeat watchdog：`process_alive=1` 只证明当前 collector 线程所属进程仍在运行，
不替代外部进程监督或跨进程存活探测。

EventManager 在 pipeline 线程将 fault transition 同步转换为 `perf_diag` 紧凑 JSON：

```json
{"schema_version":1,"source":"pem_collector","code":"degrade_transition",
 "severity":2,"monotonic_us":123,"realtime_us":456,"values":[0,0,0,0,0,0,0,0]}
```

`perf_diag` 是当前进程内/transport 边界；没有 Protocol Buffers、上传服务、Lua 规则或
QNX 专用实现。规则扩展仍可通过 `pem_runtime_set_rule()` 注册：它只在成功 durable write
之后、运行时锁外收到记录，必须保持非阻塞且不能写文件。

YAML 是未来 loader 的配置格式目标；当前仓库没有外部 YAML parser，因此唯一已实现且受测的
配置入口是 pipeline params 中的 JSON v1。`config/pipeline_car.json` 给出了完整 schema
示例，未配置时使用与旧 collector 等价的四 topic 默认表。

## 业务回调流

`pem_collector_node` 补充同一 PEM 协议中的车辆/业务数据部分。它是一个
FlowCoro 节点：transport 回调只做轻量校验并复制到有界输入池；唯一协程依优先级
解析、聚合并拥有业务 PEM writer。因此 GPS、定位和降级事件的发布回调不会执行
解析、诊断发布或文件系统 I/O。

| 回调 topic | 采集类别 | 当前记录 |
|---|---|---|
| `sensor/gps` | 持续指标 | GPS 里程、行驶时长、车速、位置和定位精度 |
| `fusion/localization` | 持续指标兜底 | GPS 定位超过两秒未刷新时的里程 |
| `navigation/region` | 事件 | 地区切换，JSON 负载为 `{"region":"..."}` |
| `pem/degrade_event` | 关键事件 | 量产 `monitor_node` 发布的降级等级/原因迁移 |

时间戳倒退、间隔超过五秒、或位移超过 `max_step_m` 的里程增量都会被丢弃，避免
重启或 GPS 瞬移虚增里程。GPS 新鲜时始终是权威来源。采集器以 `emit_hz` 周期
写入一条 `business` 记录，其字段为 `distance_m`、`driving_time_s`、
`speed_mps`、`latitude`、`longitude`、`accuracy_m`、`source` 和
`fix_age_s`，记录名为 `trip:<region>`。

新增业务采集应在 pipeline params 的 `pem_runtime.topics[]` 注册，并在
`pem_collector_node.cpp` 的 pipeline handler 增加对应 `kind` 的**线程内**解析逻辑；
不要新增回调解析分支。callback 只能校验并压入有界输入池，只有 FlowCoro pipeline 可以
计算指标、发布诊断或写入语义化快照/事件。

## 量产配置

`config/pipeline_car.json` 启用量产模式：

```json
{"mode":"production","rotate_sec":300,"rotate_mb":100,
 "retain_segments":96,"retain_mb":1024}
```

它同时启用 `pem_collector`。其独立的
`kunautodrive_pem_business_*.pem` 流与 monitor 基础设施流使用相同的
CRC 布局和保留策略。两个单 writer 流分开，避免跨节点 close/保留策略竞争，
同时仍可由 `pem_dump.py` 按时间戳统一解码和关联。

monitor 的可选参数为 `frequency_hz`（0.2-10）、`pem_log_path`、
`rotate_sec`、`rotate_mb`、`retain_segments`、`retain_mb`、
`cpu_critical_pct` 和 `mem_critical_pct`。未配置 `pem_log_path` 时，
日志使用跨平台 KunAutoDrive 临时目录，前缀为 `kunautodrive_pem`。

解码日志：

```bash
python3 tools/pem_dump.py /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --jsonl --type event /tmp/kunautodrive_pem_*.pem
python3 tools/pem_dump.py --type topic --name planning /tmp/kunautodrive_pem_*.pem
```

PEM 运行时门禁：

```bash
env -u LD_LIBRARY_PATH cmake -S modules/pem -B build/modules/pem
env -u LD_LIBRARY_PATH cmake --build build/modules/pem
env -u LD_LIBRARY_PATH ctest --test-dir build/modules/pem --output-on-failure \
  -R 'pem_log_protocol|pem_runtime_bounded'

# End-to-end monitor + collector adapter smoke test
env -u LD_LIBRARY_PATH ctest --test-dir build --output-on-failure -R pem_runtime_smoke
```

前两个测试由 `modules/pem/CMakeLists.txt` 注册，独立验证 PEM 日志协议与有界
运行时；adapter 的独立 CMake 构建也会包含它们。最后一个测试以仿真管线临时启用
production monitor 和 `pem_collector`，断言业务
`.pem` 文件实际生成，并由 `pem_dump.py` 成功解出 `trip:ci_simulation`。
它随 integration CI 运行，不依赖 GPS、串口或其他实车硬件。

量产 profile 特意不订阅场景和其他大负载 topic，也不会打开 dashboard/stats IPC
桥接。
