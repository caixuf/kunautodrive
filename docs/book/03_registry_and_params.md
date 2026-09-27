# 第 03 章：热重载的真相——为什么改了值不能改行为

> **v2 范式章节**（2026-09 整治后保留）。本章保留低行号密度、真技术书
> 风格，参照 `docs/book/README.md` 写作风格约束与 `docs/book/09_discovery.md`
> 范式示范。v1 行号清单版本归档于 `docs/_archive/book_v1/03_registry_and_params.md`。

这一章在第一卷　微内核与系统编程。
上一篇是把节点拆成插件的接口契约（见第 02 章）。
这一篇讲插件里那些被运行时随时改写的数值：注册中心、参数表，以及
热重载到底走的是一条怎样的延迟通路。

把车开起来之后想换一个 MPC 增益、换一个 LiDAR 最大量程、换一个 BEV 检测
置信度阈值——这是真实工作的常态。
早期做法是改源码 → 重编译 → 重启管线；这个循环在我们的几个测试场景里是
30~40 秒，对日常调试不可接受。
需要的是「运行中的进程改个数值，下一帧生效」。
听起来简单，但 v1 注释里那些"已注册但改不动"的 bug 提醒我们——
文档必须对得起这套 API 的真实行为。

## 当「改一个值」撞上三个边界

把 `0.3` 改成 `0.5` 这一下，手指在终端敲下数字之前，有三层必须先想清楚。

第一层是**类型**。
节点里声明过的是 `float` 还是 `int`？`float` 在 IEEE 754 下的位流跟 `int`
完全不同；参数系统若把两者等同，就会出现「明明说要写 1，但写成 0.999999」
的灵异。
第二层是**范围**。
控制器的微分增益不可能接受 `1e6`；注册时要给出区间，运行期写入也要校验。
第三层是**时机**。
节点的多数运算在 tick 函数里跑。
注册表接受值之后，运行时必须能让节点下一帧取到新值，而不是让值在全局内存
里静静等一个永远不发生的 wake-up。

`docs/_archive/book_v1/03_registry_and_params.md` 在第 3 节里把这套行为描写
得过于"贴心"——它假设类型不匹配能返回一个明确错误，假设 `hot_reload` 字段
被设置就能立刻看到。
真实世界线里，这两条都是半真半假。
下文会把分界线一一画清。

## 一张表，两种语义：注册期 vs. 读取期

参数系统是单进程内的全局数组 `g_params[]`，每个槽位是一个变体记录，类型
字段是判别器。
`param_register_*` 这一族函数在节点启动阶段把参数塞进数组。
`param_get_*` 这一族函数每帧从数组读出值。

```
      ┌──────────────────────┐
      │    g_params[]        │  (intra-process)
      │ ┌────┐┌────┐┌────┐   │
      │ │ id ││ id ││ id │   │
      │ │  f ││  i ││  b │   │   字段 id / type / value / min / max
      │ └────┘└────┘└────┘   │
      └──────────────────────┘
           ▲          ▲
           │          │
   register_*         get_* (per-tick)
```

注册时的语义和读取时的语义**有意不一致**。
注册时若同名参数已经存在，旧的值会被丢弃，新值的范围/默认值生效。
读取时只能根据 `type` 字段解释 `value`，类型不匹配**不报错**，会强制做一次
C 语言默认转换（float → int 截断、int → float 重新分配位）。
这看起来是个 bug，但实际是历史产物：节点的初始化代码可能先按 `int`
注册、再按 `float` 覆盖，注册端不报错才允许这种"先占坑后替换"。
代价是：调用端必须知道自己拿到的是哪种类型，不能轻信 `param_get_int` 一定
返回整数——它在底层可能拿到的是上一次注册留下的 `float`，只不过被截断了。

## 范围校验只在「写入」一条路径上生效

注册时给 `[min, max]` 后，运行中的 `flowctl param set` 会校验。
注册时给 `[min, max]` 后，源码里手动 `param_set_float`——**不校验**。
源码里手动 `param_set_int`——也不校验。

这导致一个真实的 bug：节点 A 注册 `[0.0, 1.0]`，但节点 A 的某个早返回
分支里有人手写 `param_set_float(p, 5.0)` 写越界。
运行期 `flowctl` 一切正常，进程内部却悄悄有了一个 5.0。
后来要查"为什么这一帧控制器崩了"，翻 log 看不到，因为 `param_set_float`
根本不走校验路径。

设计上没有修：因为修改源码里的硬编码值越界属于"程序员错误"，运行期
`flowctl` 的越界是"运维错误"，两边分别处理。
代价是文档必须显式记下这一条，**不让读者误以为参数系统是统一守门人**。

## 热重载是「下一帧才看见」，不是「即时看见」

这是 v1 注释里最容易被误读的一条。

热重载路径：

```
flowctl param set → AF_UNIX 行协议 → param_bridge → g_params[idx].value
                                                         │
                                                 节点的下一帧 tick
                                                         │
                                                         ▼
                                                  param_get_*(id) 读出新值
```

`param_bridge` 写完 `g_params[idx].value` 之后**不主动通知节点**。
节点下次进入 tick 自然读到新值。
这对单帧场景（控制、规划）的延迟是一个节拍（典型 100 Hz 下是 10 ms）——
可接受。
但对订阅型 topic（节点订阅了某个 topic 才进入处理路径）来说，"看到新值"
的时刻还可能落在订阅回调里——具体取决于该节点是先 `param_get` 还是先
`topic_recv`。

## 三处全通，参数才真的"活的"

新增一个可热重载的参数必须三处一起改，**只做注册等于没做**。
这条铁律是过去几个月调试 cost 最贵的教训。

第一处是 `params_json` 解析：在 `cJSON_Parse` 之后要按 id 取值，否则
`config/pipeline.json` 里写的那行就只是字符串摆设。
第二处是 `param_register_*` 的默认值——必须用解析拿到的值 `g.<字段>`，
而**不是**硬编码字面量。
v1 写法里曾把 `target_speed = 12.0` 这样的字面量塞进 `register_float`
——结果是 `flowctl param set` 改了值，但节点的默认值还是 12.0，重启后又
回到 12.0。
读者若只读 v0，会以为"`flowctl` 改了没用"，其实是参数系统按设计只在
**当前进程生命周期**内 live，重启走 `params_json` 解析。
第三处是节点 tick 里每帧 `param_get_*`——注册了但只在初始化时读一次的
参数，运行时改了也看不见。

这三处看起来琐碎，但单条漏放就会复现成"我改了为何不生效"。
源码里每个节点的初始化函数、tick 函数、`params.json` 三件套请读者养成
视图切换看同参数的好习惯。

## 跨进程：`flowctl` ↔ 运行节点

节点对外暴露参数靠一条 AF_UNIX 行协议——一个进程内 `param_bridge` 打开的
Unix domain socket，路径在运行时由 `flow_registry` 分配（`/tmp` 下，
具体形式见 `docs/MONITORING_ARCHITECTURE.md`）。

```
┌────────┐  AF_UNIX (行协议)  ┌──────────────────┐
│ flowctl│ ───────────────►  │ param_bridge      │
│ (CLI)  │  ◄─────────────── │   (in-process)    │
└────────┘  行: SET id value  └──────────────────┘
                                  │
                                  ▼
                              g_params[idx]
```

协议是文本单行：`SET <param_id> <value>\n`，回 `OK\n` 或 `ERR reason\n`。
任何字段一行没结束就略过。

把 `flowctl param get control.mpc_r_ddelta` 拆开看：

1. `flowctl` 解析子命令 → `param_id="control.mpc_r_ddelta"`；
2. 通过 AF_UNIX 发 `GET control.mpc_r_ddelta`；
3. 目标节点的 `param_bridge` 在 `g_params[]` 里查 id；
4. 把 `(type, value)` 序列化成 `FLOAT 0.42\n` 回写；
5. `flowctl` 打印。

整个过程**不跨机器**。
跨机器要走我们没做的方案（如 SSH），原因是机器人系统一般单机；加层网络
转发会引出"两台机器上各有同名参数"的一致性问题，性价比不高。

## `flow_registry` 是另一张表

参数表和注册中心不是同一回事。
`flow_registry` 登记**进程、topic、类型、插件、schema**五类元信息——它的
目的是发现、查询、兼容性检查（见第 09 章）。
参数表只关心"运行期可改的数值"。
两者**有意不合并**：把元信息和可变状态混在一张表里，节点身份（不可变）会
被运行时写入污染，序列化和一致性都变难。

`flowctl list`、`flowctl inspect`、`flowctl inspect task` 全部消费
`flow_registry`；`flowctl param set|get|list` 全部消费参数表。
读者看到两个表时不要混——这是两条不同的总线。

## 配置文件：`params_json` 与 `params_string` 的两种形态

节点接收参数有两条路：

- `params_json`：标准 JSON 文本，**主路径**。
  `cJSON_Parse` 后 `cJSON_GetObjectItemCaseSensitive` 取字段。
- `params_string`（少见）：早期单字符串参数通道，遗留接口。
  新节点请用 JSON。

v0 注释里写"两种都行，请任选"是历史遗留。
真实情况：`pipeline_check.py` 跟 `flowctl param` 都在 JSON 上跑，
`params_string` 现在只剩一两个老节点还用。
新增参数请直接 JSON 路径；`params_string` 会在后续某个版本删除（具体时点
未定，见 `docs/ROADMAP_L3.md`）。

## 跨进程可见性：一个平台相关的承重假设

```
        Linux / macOS                  Windows (MinGW)
   ┌──────────────────────┐       ┌──────────────────────┐
   │ launcher(DLL)        │       │ launcher + param.dll │
   │   param_runtime.a  ←─┼──┐    │   (shared runtime)   │
   │ node_a.so            │  │    │ node_a.dll           │
   │   param_runtime.a  ←─┼──┘    │ node_b.dll           │
   │ node_b.so            │       │   (同一份 param.dll)  │
   └──────────────────────┘       └──────────────────────┘
```

Linux/macOS 走的是「**扁平符号命名空间**」约定：所有节点和 launcher 都
隐式拿到 `g_params[]` 的同一实例。
原因是 ELF 与 Mach-O 都支持通过 `-Bsymbolic` 让节点 DLL 解析到宿主符号。
Windows 上 PE/COFF 没有这个 interposition，每个 DLL 会得到自己的
`g_params[]`，节点注册的值对 `flowctl` 不可见。
解决：Windows 走一份共享参数运行库（`param_runtime.dll`），强制每个节点链
接这一份。

具体怎么连请看 CMake 配置与 `include/platform_compat.h`——读者只需知道
**这是承重假设**，改文件列表时要小心。

## 我们踩过的坑

### 坑一：参数注册了但 `flowctl param get` 报不存在

最早期的 v0 写法里，"默认值"和"运行时值"用同一份 `param_register_*`
调用。
重启节点 → 解析 `params_json` → 拿着 12.0 调用 `param_register_float`。
此刻 `g_params[]` 里有这个 id，但运行时 `param_get_float` 返回的是
"未注册"的 sentinel——因为 `param_register_*` 内部判断"是否已注册"，若
已注册就返回旧的（默认值那次），新值被忽略。

修复：把"默认值走 static initializer + 延迟注册"和"运行时值走 `SET`"
完全分离。
现在的写法是：节点 `init` 函数里**先**把字段解析到 `g.<字段>`，**再**调
用 `param_register_float(g.id, g.<字段>, ...)`，让注册步骤读到的是已被
JSON 覆盖的字段，而非源码里那个字面量。
这条铁律在我们做 control 节点族时复盘过三遍才稳。

### 坑二：`flowctl param set` 改了一个不存在的字段

`flowctl` 把 `SET nonexistent.float 0.5` 发出去，目标节点 `param_bridge`
查不到 id，回 `ERR unknown_param`。
但 `flowctl` 端的 errno 路径里我们有一次错误地把 `ERR unknown_param` 当
成 success——CLI 直接打印"OK"，日志里没看到字段不存在，等到下一次 demo
跑偏才发现参数没生效。
修复：在 CLI 一端校验 `set` 命令的 stderr，`flowctl param set` 改完后
exit code 必须非零才视为失败。
详见 `docs/HANDOFF_2026-09-22.md`。

### 坑三：跨节点的同名参数导致 `flowctl param set` 改了错进程

旧版本里 `param_id` 是全局字符串，**没有进程前缀**。
两个节点（比如 `control` 和 `behavior`）都注册了 `target_speed`，
`flowctl param set target_speed 12.0` 会广播到所有监听 socket 的进程——
谁先 `accept()` 谁改。
行为不一致时排查极困难，行为一致时就是侥幸。

修复：现在 `param_id` 强制以节点名做前缀（`control.target_speed`、
`behavior.target_speed`），跨节点同名参数隔离。
配置 JSON 里的 `id` 字段请遵循 `<node>.<param>` 格式。

## 测试：怎么验证热重载真的好用

下列三种测法是 CI 必备：

1. **静态扫描**：`topic-contract` gate 校验 `pipeline*.json` 里写的参数 id
   是否在节点 `s_params` 里也存在；
2. **运行时回环**：起 `pipeline.json`，`flowctl param set` 后看
   `g_params[idx].value` 是否变更，再拿 `flowctl param get` 核对；
3. **跨节点**：`flowctl param set control.target_speed 0` 看 `control`
   进程响应，且 `behavior` 进程不变。

## 思考题

1. 假如某节点不订阅任何 topic，它会进入 tick 吗？`flowctl param set` 改了
   一个只有该节点注册的参数后，该节点能不能"自我唤醒"读新值？
2. 范围校验放在 `param_set_*` 一侧（而不是 `param_register_*` 一侧）的
   好处是什么？坏处呢？
3. 如果今天新增 `sensor_model_node` 想加一个参数，你会改哪三处？

下一章看的是一个在不同节点之间搬数据的话题总线（见第 05 章）。
