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

## 业务回调流

`pem_collector_node` 补充同一 PEM 协议中的车辆/业务数据部分。它是一个
FlowCoro 节点：transport 回调只做轻量校验和内存聚合（或向有界队列压入事件），
唯一协程拥有业务 PEM writer。因此 GPS、定位和降级事件的发布回调不会执行
文件系统 I/O。

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

新增业务采集应在 `pem_collector_node.cpp` 的 `PemSubscription` 注册表中加入
topic/回调对：回调必须保持非阻塞，只能由 FlowCoro 任务写入语义化快照或事件。
有界事件队列溢出会写出关键 `collector_event_overflow` 记录，不会静默丢失。

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

量产 profile 特意不订阅场景和其他大负载 topic，也不会打开 dashboard/stats IPC
桥接。
