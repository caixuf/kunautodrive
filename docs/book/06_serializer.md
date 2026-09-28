# 第 06 章：写在总线上的字节——为什么 wire 协议必须固定

> **v2 范式章节**（2026-09 整治后保留）。本章保留低行号密度、真技术书
> 风格，参照 `docs/book/README.md` 写作风格约束与 `docs/book/08_discovery.md`
> 范式示范。v1 行号清单版本归档于 `docs/_archive/book_v1/07_serializer.md`。

这一章在第二部分　通信与时间。
上一篇是节点之间的总线（见第 05 章）——它告诉你"数据从 A 到 B"。
本篇解决"放到线上的时候是哪些字节、按什么规则读出来"。

读者也许会问：JSON 难道不行吗？snprintf 不也会？
这恰好是我们在 2024 年走过的路——把 `snprintf("{\"x\":%d}")` 拼出来的
字符串塞到 topic payload 里，让订阅者 `strstr` + `sscanf` 解析。
结果当时遇到的真实事故有：

- 跨语言（Python learner ↔ C++ 节点）浮点格式不一致
  （`1.0` vs `1,0` vs `1.000000`）；
- 字段加减半年后发现"没人改 ABI，旧订阅者照样按位置偏移读，结果错位"；
- 不同节点在同一段时间偏移里写一半中断 → 半字符串解析炸段错误。

于是在 2025 年统一：
所有总线 payload **必须**走序列化层，类型在 IDL `msg/adas_msgs.msg`
里定义、编译器生成 C 结构、自动注册进类型表。
本篇把这条规则讲清楚。

## 三个不可省的环节

序列化层由三部分组成：

1. **IDL**（接口描述语言）：`.msg` 文件，用类似 C struct 的语法声明字段
   类型；
2. **代码生成器**：`tools/msg_codegen.py`，输出 C 头 `adas_msgs_gen.h`，
   字段定义和 `(un)pack` 函数一一对应；
3. **运行时**：`src/core/serializer.c` 提供类型表查
   询、`(un)pack_*`、FNV-1a hash 校验。

```
msg/adas_msgs.msg ──┐
                    │ parse
                    ▼
        tools/msg_codegen.py
                    │
                    ▼
            adas_msgs_gen.h ── #include ──▶ 节点源码
                    │
        生成时附带 type_id = FNV-1a("adas_msgs::Obstacle")
                    │
                    ▼
          src/core/serializer.c
                    │
                    ▼
           运行时 type_registry 表
```

节点使用流程：

1. `#include "adas_msgs_gen.h"`（生成的，不要手写）；
2. 填字段：`obstacle.x = ...; obstacle.y = ...;`；
3. 序列化：`serializer_pack_Obstacle(&buf, &obstacle)`；
4. publish 到 topic；
5. 订阅者反序列化：`serializer_unpack_Obstacle(&payload, &out)`。

注意：**不要**自己写 `#pragma pack` 或手算结构体偏移——`serializer_*
` 函数知道字段顺序和对齐，结构体本身的对齐不是 ABI 的一部分。

## 类型 ID：FNV-1a 把字符串变成稳定的 32 位整数

每个 IDL 类型都对应一个编译期常量，名字形如
`TYPE_ID_ADAS_MSGS_OBSTACLE`，值是 `FNV-1a("adas_msgs::Obstacle")`。

FNV-1a 是 32 位字面量，不是加密哈希——它**有意不做**加密强度，
只求"32 位均匀分布 + 跨平台一致"。
hash 算法没有"分支安全"——理论上可能有冲突，我们跑了 23 个类型没撞过。
**风险**：`adas_msgs::Obstacle` 这个字符串若被改名，hash 全变，所有
存量 bag（详见 `docs/book/12_bag_recording.md`）记录下来的类型 ID 跟现
场对不上——这是改 schema 名要付出的代价，不是 bug。

```
hash("adas_msgs::Obstacle")    = 0x322bd084u   // codegen 编译期常量
hash("adas_msgs::ObstacleV2")  = 0x<新 ID>u    // 完全不同的另一个 ID（与字段布局哈希共同判别）
```

增量改名请**新增**字段而不是改 IDL 字段名；旧字段 deprecate 但保留
二进制兼容位——下文 v0 翻车故事有详细复盘。

## wire 格式：小端 + type_id + payload

总线在 IPC 层（详见第 07 章）下传输的二进制格式约定：

```
┌────────────────┬─────────────────┬────────────────┐
│ magic (4B)     │ type_id (4B)    │ payload (N B)  │
│  "FMSG"        │ FNV-1a of name  │ 序列化后字节流 │
└────────────────┴─────────────────┴────────────────┘
        ▲
        │
   ENDIAN_MARKER_LE 跟随 header——永远是小端
```

（注：进程内总线的"消息"对象不带这个 4 字节 magic，那是 IPC transport
层封装的；详见第 07 章。）

`type_id == 0` 表示 raw payload —— 给那些还没规范化 IDL 的子模块用，
**是个过渡出口，不是稳定接口**。
CI 的 `topic-contract` gate 会警告 raw 类型的设计。

字节序：
**wire 上无条件小端**。
当前所有支持平台（x86、ARM64 Linux、苹果 Apple Silicon）都是小端，
统一 LE 让 `serializer_store_le` 在这些平台上只剩一次 `memcpy`——
速度最快也最简单。
`serializer_normalize_endian` 函数留了 BE 主机的兼容路径——未来若跑
big-endian 主机（很罕见但可能）才被启用。

字节序协商：消息头里有个 `endian_marker` 字段。
若发现对端的 marker 是 BE 而主机是 LE，由 `serializer_normalize_endian`
原地交换字节。
`endian_marker == 0`（未知）则跳过——视作 raw。

## 数组长度：固定 + 计数双驱动

`msg/adas_msgs.msg` 里所有动态数组都遵循同一个约定：

```
struct Trajectory {
    uint16   point_count        # 实际占用数
    TrajectoryPoint points[64]  # 容量上限
    ...
}
```

`point_count` 永远是"实际值"，`points[64]` 是容量上限。
`serializer_unpack_*` 会校验 `count <= capacity`——越界返回
`ERR_INVALID_PARAM`，不让读端盖到别人的栈上。
这一类防御性检查比单纯信任"我相信发送端不超" 要贵一点——单条消息多
个 memcpy——但比起内存损坏 0 容忍，这点开销是值得的。

## 版本号：消息头之外的"显式约定"

`adas_msgs::Obstacle` 当前是 schema v1。
我们**目前没有 runtime schema 版本协商**——所有节点必须用同一个版本的
IDL 重新编译。
这条规则有点死板，但避免了 DDS 那类复杂的 schema 兼容层（成本巨大，益处
在量产场景才显现）。
对当前阶段（开发期 + demo 期），同步重编译就是可接受的代价。

升级协议：保留旧字段（即使 deprecated），新增字段加在末尾，发布版本号
进 IDL 注释（用 `# @version 2` 这种）；节点读端忽略未知尾字段。

## 跨语言一致：序列化是契约

跨语言涉及 Python 训练（`tools/train_demo_model.py`）、C++ 上线节点。
若 Python/C++ 序列化结果不同，所有离线训练都没用。
**铁律**：跨语言特征提取器请让 `msg_codegen.py` 生成**两份**——一份
C 头，再加一份 Python skeleton（暂未实现，作者立场：未来应该做）。
今天 Python 端是手写的 `serialize_obstacle()`，单测要断言与 C 端在容差
$10^{-6}$ 内一致（这是上一章提到的"差点翻车"的补救，详见 `docs/book/24_e2e_learning_loop.md`）。

## 测试：怎么验证序列化层是对的

1. **互逆性**：每个结构 `pack` → `unpack` 字段相等；
2. **空字段**：`pack` 一个全零结构，`unpack` 后字段全零；
3. **容量上限**：超 `point_count <= N` 触发 `ERR_INVALID_PARAM`；
4. **跨语言**：Python `serialize.py` 与 C `pack()` 一致。
   `tools/train_demo_model.py` 启动会先跑这条。
5. **wire 字节序**：若平台大端（罕见），跑一次大小端互换场景。

## 我们踩过的坑

### 坑一：snprintf 手拼 JSON 的 `1.0` / `1,0` 格式不匹配

v0 控制节点发的是 `snprintf(buf, ..., "{\"x\":%f}", x)`。
Subscriber 端是 Python learner，用 `json.loads` 解析——结果是 `1.0`。
另一个订阅者是另一个 C++ 节点，里面用 `sscanf("%f", &x)` 读，结果是
`1.000000`（如果精度足够高）。
两节点对同一字段读到 `1` 和 `1.000001`，轨迹规划跟 learner 的 reference
输入就不再一致——所有离线训练误差都从这里开始无意义。

修复：禁止手拼 JSON（CLAUDE.md 强制条款），所有结构走 IDL + 序列化。
代码 review 检查项之一："topic payload 不是 JSON 文本？字符串里有
`{`、`}`、`\"` 而没有走 cJSON？拒收。"

### 坑二：改了 IDL 字段名，旧订阅者按偏移读爆栈

subscribers 中有些是"不愿重编译也要能跑"的旧节点。
我们曾改 `Obstacle.x` 为 `Obstacle.position_x`——编译器生成的
`serializer_unpack_Obstacle` 按字段名（在新 IDL 里）对应到位置（在新 IDL
里）的偏移。
但旧订阅者还在用旧二进制——它看到的字节流是新版结构体，字段顺序变了
（x 在第 3 个，position_x 在第 12 个），结果错位读到 `vy` 当 `x`，
车辆就以"前车在我车正前方其实在我正后方"的状态启动，做出向前的决策——
事故。
修复：保留旧字段，仅 deprecate 而不删除；新增字段加末尾；订阅者忽略
未知尾字段。
这条规则与 ROS/Protobuf 是一致的，不是我们独创。

### 坑三：发布端忘记设 `endian_marker == 0`

曾经有个模块把 `endian_marker = serializer_endian_marker()` 写在了
**每次 pack** 上——结果 `serializer_normalize_endian` 把已转换的
payload 又转换一次。
但因为 LE 主机 ↔ LE marker = no-op，**bug 没冒头**，直到某个 CI 路径切
换到一台 BE 模拟环境才暴露出来。
修复：只在第一次 normalize 时改 marker，并且 normalize 完置 marker 为
`serializer_endian_marker()`——marker 字段语义是"我被转换为哪种字节序"
而非"原始字节序"。
这条规则 v0 没写——教训**这条 path 必须有一个不动点**。

### 坑四：trajectory `points[64]` 上限与 v2 升级

`TrajectoryPoint.points[64]` 上限是早期 30 帧 50m 规划产物，留 64 是
"够宽"。后来规划升级到 80 帧 100m，溢出 → 数据被截 → 实际轨迹点 64.
之后 16 帧被丢，控制环跑飞。
修复：见 `docs/HANDOFF_2026-09-21b.md`，把上限升到 128，再加越界返回
（非 silent drop）。**所有动态数组请记得把"实际值"和"容量上限"分成
两个字段**，下标别隐式约定。

## 与其它章节的关系

- 与第 05 章：总线是数据通道，序列化是通道上的字节意义。前者换实现
  不影响序列化。
- 与第 07 章：进程内用零拷贝指针，IPC 用序列化字节流。两端的 ABI 假设
  都靠 type_id 校验。
- 与第 09 章：所有类型 ID 在 `flow_registry` 里登记，订阅者启动时
  check 兼容性。
- 与第 19 章：Python 训练的特征提取必须与 C++ 序列化层一致——本篇是
  那条契约的物理基础。

## 思考题

1. `type_id == 0` 的 raw 路径现在是个"合法 escape hatch"，它会被滥用
   吗？CI 应该怎么强制？
2. 假设未来我们要做 BE 主机部署，哪些函数路径会触发？现在的代码
   `normalize_endian` 设计能 hold 住吗？
3. 升级 schema 时"保留旧字段" vs. "完全重命名" 各有什么代价？本篇的
   建议倾向哪种？

下一章看 IPC 层——即上面提到的"序列化在跨进程那一段实际怎么走"
（见第 07 章）。
