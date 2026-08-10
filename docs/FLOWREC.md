# flowrec 数据采集节点

`flowrec_node` 是通用业务 topic 留存节点：按配置写入 **Bag v2**，不替代
`data_recorder_node` 的训练 JSONL 样本采集。配置随 pipeline 的 `params` 传入，
因此不依赖 YAML 解析库。

## Pipeline 配置

在目标 pipeline 的 `nodes` 数组加入：

```json
{
  "name": "flowrec",
  "library_path": "build/lib/libflowrec_node.so",
  "params": {
    "status_hz": 1,
    "collectors": [
      {
        "name": "sensor_raw",
        "topics": ["sensor/lidar", "sensor/gps", "sensor/camera"],
        "output": "datasets/flowrec/sensor_%Y%m%d_%H%M%S.bag",
        "max_size_mb": 1024,
        "rotation": "hourly",
        "trigger": "always_on"
      },
      {
        "name": "emergency",
        "topics": ["perception/objects", "control/cmd", "fusion/state"],
        "output": "datasets/flowrec/event_%Y%m%d_%H%M%S.bag",
        "max_size_mb": 512,
        "trigger": {
          "type": "topic_value",
          "topic": "control/cmd",
          "field": "emergency_brake",
          "equals": true
        },
        "pre_buffer_sec": 5,
        "pre_buffer_mb": 32,
        "post_buffer_sec": 10
      }
    ]
  }
}
```

输出目录会以 `0750` 自动创建。每个实际文件在模板扩展后追加递增段号，例如
`sensor_20260811_000000_000001.bag`；这使同一秒内的大小轮转不会覆盖文件。

## Collector 契约

| 字段 | 说明 |
|---|---|
| `name` | 可选且唯一；省略时使用 `collector_N` |
| `topics` | 必填、精确 topic 名列表，1–32 项；不支持 wildcard |
| `output` | 必填的 Bag 路径模板，支持 `strftime` 时间占位符 |
| `max_size_mb` / `max_size_bytes` | 二选一；范围 256 KiB–16 GiB，默认 1 GiB |
| `rotation` | 可选：`none`、`hourly`、`daily` |
| `rotation_sec` | 与 `rotation` 二选一，范围 0–604800 秒 |
| `trigger` | `"always_on"` 或下述 `topic_value` 对象 |

`topic_value` 的 `topic` 必须同时存在于该 collector 的 `topics`。它只解析触发
消息的**顶层 JSON 字段**，`equals` 必须是布尔、数字或短字符串，并做类型相同的精确
匹配。v1 不执行字符串表达式（例如 YAML 设计中的 `== true`），避免把配置当脚本执行。

事件 collector：

- `pre_buffer_sec`：保留触发前的滚动窗口，范围 0–300 秒；
- `pre_buffer_mb`：pre-buffer 内存上限，范围 128 KiB–512 MiB，默认 32 MiB；
- `post_buffer_sec`：首次触发后继续写入的窗口，范围 0–600 秒；窗口内再次触发会延长截止时间。

内存上限先于时间窗口生效；超限时淘汰最旧记录。单条消息超过 pre-buffer 上限会被丢弃，
并计入状态。Bag 写入使用已有异步 ring buffer；其饱和造成的记录丢弃也会显式计数，
不会无限阻塞业务 topic 回调。

## 状态与运维

节点发布 `flowrec/status`（默认 1 Hz），并通过 discovery 宣告该 topic；flowmond/
monitor 可按普通 topic 统计、采样或转发。紧凑 JSON 包含：

```text
type, timestamp_us, healthy,
collectors[].{name, mode, active, messages_written, messages_dropped,
prebuffer_messages, prebuffer_bytes, prebuffer_dropped, triggers, rotations,
write_errors, last_output, last_error}
```

`healthy=false` 表示至少一个 collector 无法打开或写入 Bag。事件 Bag 在 post-window
结束、常开 Bag 在轮转或节点停止时关闭；关闭会写入 Bag v2 索引和 header，随后可用
现有 `bag_check`/`flow_bag` 工具读取。

## 构建与验证

```bash
cmake --build build --target test_flowrec
ctest --test-dir build -R '^flowrec_tests$' --output-on-failure

cmake -S modules/adas_nodes -B build/nodes -DFLOWENGINE_BUILD=build
cmake --build build/nodes --target flowrec_node
```

`flowrec_tests` 覆盖常开 Bag v2 写入、topic_value 的 pre/post 窗口、按大小轮转、
并发 collector 的独立 writer/ring wrap、状态 JSON 与拒绝未收集触发 topic 的配置。
