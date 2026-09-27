<!-- @archive-banner -->

> **归档说明**：本章是 docs/book v1（2026-09 前后"按源码逐条重建"产物）的版本。
> 内容大量绑定代码行号、文件名、函数符号，与代码强耦合 —— 维护成本高且易过时。
> v2 重写计划：见 `docs/book/README.md` 的写作风格约束 + 真技术书范式。
> 本归档文件保留供历史参考；引用时用 `docs/_archive/book_v1/` 而非 `docs/book/`。
> **现章节**：BOOK.md 第 03 章「注册中心与参数系统」。

# 第 03 章：注册中心与参数系统 —— 让运行中的车改参数

> **本章导读**：
> 前两章造了对象、拆了插件。但还有一个问题没解决：**车上跑着的程序，怎么在不停车的情况下改一个参数？**
>
> 调 PID 增益、换车道宽度、调限速——这些都需要在车跑着的时候改，改完下一拍就生效。这需要一个进程间可见的参数存储。这就是 `param_registry` + `param_bridge` 的全部职责。
>
> 本章的核心发现值得先说，因为它可能会让你对这套系统的理解完全改变：
>
> > **`param_registry.h` 里那套「注册回调 + hot_reload 标志」的机制，一行都没被调用过。它是死代码。**
> > **真正生效的热重载是另一套东西：每个节点的 `run()` 循环开头逐帧调 `param_get_float()` 重新读一遍。**
> >
> > 这两套机制**完全无关**。头文件里展示的是前者,代码里跑的是后者。`flowctl` 里那个代表「可热重载」的 🔥 图标,**永远不会出现**。

---

## 1. 参数表：一个进程内的全局数组

### 1.1 数据结构

[`include/param_registry.h`](file:///home/caixuf/code/FlowEngine/include/param_registry.h) 定义了三样东西。

容量与长度（`:29-31`）：

```c
#define PARAM_MAX_ENTRIES    128
#define PARAM_NAME_LEN       64
#define PARAM_DESC_LEN       128
```

四种类型（`:35-40`）：

```c
typedef enum {
    PARAM_INT    = 0,
    PARAM_FLOAT  = 1,
    PARAM_BOOL   = 2,
    PARAM_STRING = 3,
} ParamType;
```

值的存储是一个 union（`:42-47`）：

```c
typedef union {
    int64_t  int_val;
    double   float_val;
    bool     bool_val;
    char     str_val[64];
} ParamValue;
```

> [!NOTE]
> `str_val[64]` 里的 64 是**字面量**,不是 `PARAM_NAME_LEN`。同样的不一致在 `param_registry.c:127, 128, 270` 又出现三次。整个文件有四处该用宏的地方写了字面量。

回调类型（`:51-52`）：

```c
typedef void (*ParamChangeCallback)(const char* name, ParamValue old_val,
                                    ParamValue new_val, void* user_data);
```

条目的完整定义（`:56-68`），11 个字段：

```c
typedef struct {
    char        name[PARAM_NAME_LEN];
    ParamType   type;
    ParamValue  default_value;
    ParamValue  current_value;
    ParamValue  min_value;
    ParamValue  max_value;
    char        description[PARAM_DESC_LEN];
    bool        hot_reload;          /**< true = supports runtime change */
    ParamChangeCallback on_change;   /**< called when value changes */
    void*       change_user_data;
    bool        registered;
} ParamEntry;
```

**`hot_reload` 和 `on_change` 这两个字段，本章后面会告诉你它们从未被使用。**

### 1.2 全局状态与锁

[`src/core/param_registry.c:12-14`](file:///home/caixuf/code/FlowEngine/src/core/param_registry.c)：

```c
static ParamEntry    g_params[PARAM_MAX_ENTRIES];
static int           g_param_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
```

**一个进程级固定数组 128 条，一个互斥锁。** 简单到近乎粗暴——但对 58 个参数的场景完全够用。

> [!WARNING]
> **锁的使用不一致，这是一个真实的数据竞争。**
>
> | 函数 | 是否加锁 |
> |---|---|
> | `param_register_*` / `param_set_*` / `param_get_*` | ✅ 加锁 |
> | `param_get_entry`（`:281-286`） | ❌ **不加锁**，返回活数组里的指针 |
> | `param_list_all`（`:288-293`） | ❌ **不加锁**，`memcpy` 整个数组 |
> | `param_count`（`:295`） | ❌ **不加锁** |
> | `param_enable_hot_reload`（`:297-305`） | ❌ **不加锁** |
>
> `param_get_entry` 和 `param_list_all` 都被 **param_bridge 的服务线程**调用（`param_bridge.c:127, 162, 175`），而节点的 `param_register_*` 在 init 阶段跑在**节点线程**里。两者并发时，`param_list_all` 的 `memcpy` 可能抄到半初始化的条目。
>
> 这在当前配置下不容易触发（58 个参数、启动阶段注册完毕后就不再新增），但它是一个真实的隐患。

### 1.3 容量检查

`create_param`（`:26-33`）是唯一的容量闸门：

```c
static ParamEntry* create_param(const char* name) {
    if (g_param_count >= PARAM_MAX_ENTRIES) return NULL;
    ParamEntry* e = &g_params[g_param_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, PARAM_NAME_LEN, "%s", name);
    e->registered = true;
    return e;
}
```

**注意 `memset(e, 0, sizeof(*e))` 这个细节**——它把 `hot_reload` 清成 `false`、`on_change` 清成 `NULL`。这是后面「回调永不触发」的第一个前提。

溢出返回 `NULL`，调用方转成 `ERR_OVERFLOW`（`-7`）。**但没有任何调用方记录这个错误**——第 129 个参数会静默注册失败。

---

## 2. 注册：两阶段语义

### 2.1 四个注册函数的共同形状

四个 `param_register_*`（`param_registry.h:74-79` 声明）共享同一套两阶段逻辑。源码注释写在 `param_registry.c:36-40`：

```
 * 注册语义（A-1 修复）：
 *   - 若参数已存在（例如 bootstrap 已从 pipeline_*.json 把值预加载进 registry）：
 *     不覆盖 current_value，只刷新 type/default/min/max/desc 元信息。
 *   - 若参数不存在：create_param 并把 current_value 设为传入的代码默认值。
```

`param_register_int` 是代表（`:42-65`）：

```c
int param_register_int(const char* name, int64_t def, int64_t min, int64_t max, const char* desc) {
    if (!name) return ERR_INVALID_PARAM;
    pthread_mutex_lock(&g_mutex);
    ParamEntry* e = find_param(name);
    if (e) {                              /* ── 已存在：只刷元信息 ── */
        e->type = PARAM_INT;
        e->default_value.int_val = def;
        e->min_value.int_val = min;
        e->max_value.int_val = max;
        if (desc) snprintf(e->description, PARAM_DESC_LEN, "%s", desc);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }
    e = create_param(name);                /* ── 不存在：新建并设当前值 ── */
    if (!e) { pthread_mutex_unlock(&g_mutex); return ERR_OVERFLOW; }
    e->type = PARAM_INT;
    e->default_value.int_val = def;
    e->current_value.int_val = def;       /* ← 只有这条路径设 current_value */
    e->min_value.int_val = min;
    e->max_value.int_val = max;
    if (desc) snprintf(e->description, PARAM_DESC_LEN, "%s", desc);
    pthread_mutex_unlock(&g_mutex);
    return 0;
}
```

**「已存在就不覆盖 `current_value`」这个设计是有意的**，它让「从配置文件预加载的值」不会被节点的默认值盖掉。

> [!IMPORTANT]
> **但这个「bootstrap 预加载」路径在仓库里不存在。**
>
> `param_registry.c:16-19` 和 `:36-40` 都在描述它，`control_node.cpp:1370-1371`、`:1429-1430`、`:1470` 三处都依赖它。**但我找不到任何代码把 `pipeline.json` 的 params 预加载进 registry。** `src/flow_launcher.c` 里搜 `param_` 只有 param_bridge 服务和 `param_count()`（第 49, 50, 548, 551-554, 574 行）。
>
> 所以上面这个「A-1 修复」实际上是**防御性代码，描述的路径当前不执行**。它真正生效的场景只有两个：`flowctl`/launcher 的重复注册，以及 `tests/test_new_modules.c` 的重复注册用例。

### 2.2 bool 和 string 没有 min/max

```c
int param_register_bool(const char* name, bool def, const char* desc);   /* :92 */
int param_register_string(const char* name, const char* def, const char* desc);  /* :113 */
```

**签名里就没有 min/max 参数。** `min_value` / `max_value` 保持 `memset` 后的零值。

这个不对称有实际后果：`param_bridge` 的 LIST 输出对 bool/string 的 min/max 列填 `"-"` 占位符（`param_bridge.c:143-146`），注释说「min/max 只对数值类型有意义，其余给 `-` 占位保持列数固定」。

> **而 `param_register_bool` 和 `param_register_string` 在生产代码里零调用**——`tests/test_new_modules.c:820/835` 只用了 int 和 float。所以这个不对称目前是纯理论问题。

---

## 3. 读取：类型不匹配和「不存在」无法区分

四个 `param_get_*` 是同一个模式：**线性扫描 + 类型检查**，未命中返回零值。

`param_get_int`（`:147-158`）：

```c
int64_t param_get_int(const char* name) {
    if (!name) return 0;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_param_count; i++)
        if (strcmp(g_params[i].name, name) == 0 && g_params[i].type == PARAM_INT) {
            int64_t v = g_params[i].current_value.int_val;
            pthread_mutex_unlock(&g_mutex);
            return v;
        }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}
```

| 函数 | 未命中返回 |
|---|---|
| `param_get_int` | `0` |
| `param_get_float` | `0.0` |
| `param_get_bool` | `false` |
| `param_get_string` | `NULL` |

> [!WARNING]
> **类型不匹配和参数不存在，返回值完全一样。**
>
> `param_get_float("some.int_param")` 返回 `0.0`——和「这个参数根本没注册」无法区分。
>
> 这在当前代码里有一个**真实的后果**（见 6.3 节）：`control_node.cpp:937` 用 `param_get_float` 读一个 int 语义的参数，而那个参数如果因为任何原因没注册成功，拿到 0 之后会被 `if (mh < 1) mh = 1` 修正成 1——**静默地把预测时域变成 1 步**。

`param_get_string`（`:186-196`）还有一个额外问题：**它在 `pthread_mutex_unlock` 之后才返回指针**（`:191-192`），指向静态数组内部。因为条目永不删除，实践上安全，但这是对可变缓冲区的无锁读。

---

## 4. 写入：越界会被拒，但 hot_reload 根本不看

### 4.1 `param_set_int` 的完整逻辑

`param_registry.c:210-227`：

```c
int param_set_int(const char* name, int64_t val) {
    if (!name) return ERR_INVALID_PARAM;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_param_count; i++) {
        ParamEntry* e = &g_params[i];
        if (strcmp(e->name, name) != 0 || e->type != PARAM_INT) continue;
        if (val < e->min_value.int_val || val > e->max_value.int_val) {
            pthread_mutex_unlock(&g_mutex);
            return ERR_INVALID_PARAM;        /* ← 越界拒绝 */
        }
        ParamValue v; v.int_val = val;
        int ret = validate_and_set(e, v);
        pthread_mutex_unlock(&g_mutex);
        return ret;
    }
    pthread_mutex_unlock(&g_mutex);
    return ERR_NOT_FOUND;
}
```

三个问题的答案：

| 问题 | 答案 |
|---|---|
| **检查越界吗？** | **会**——但只有 `int`（`:216`）和 `float`（`:235`）。`param_set_bool`（`:248-261`）和 `param_set_string`（`:263-277`）完全跳过，因为没什么可查的 |
| **检查 `hot_reload` 吗？** | **不检查。** `param_set_*` 从不读 `e->hot_reload`，只要在范围内就无条件写入 |
| **调用 `on_change` 吗？** | 只通过 `validate_and_set` 间接调用，而它以 `hot_reload` 为门槛——**所以实际上永不调用** |

### 4.2 那个从不触发的回调

`validate_and_set`（`:200-208`）：

```c
static int validate_and_set(ParamEntry* e, ParamValue new_val) {
    ParamValue old = e->current_value;
    e->current_value = new_val;

    if (e->on_change && e->hot_reload) {
        e->on_change(e->name, old, new_val, e->change_user_data);
    }
    return 0;
}
```

**尽管名字叫 `validate`，它不做任何校验**——边界检查在四个 `param_set_*` 调用方里做完了。它永远返回 0。

而那个 `if` 条件**恒为假**，原因是一条完整的死链：

```
param_set_callback()      ← 零调用者
   ↓  （唯一能设置 on_change 的函数）
param_enable_hot_reload() ← 零调用者
   ↓  （唯一能把 hot_reload 置 true 的函数）
create_param() 的 memset   ← 把两者都清成 false / NULL
   ↓
ParamEntry 的初始状态：hot_reload = false, on_change = NULL
   ↓
if (e->on_change && e->hot_reload)  永远为假
```

> [!IMPORTANT]
> **`ParamChangeCallback` 这个整个类型、整套回调机制，在生产代码里完全不存在。**
>
> 穷举 `param_set_callback` 的匹配：只有 `include/param_registry.h:84`（声明）、`src/core/param_registry.c:134`（定义）、以及两份文档。**零个 C/C++ 调用点。**
>
> `param_enable_hot_reload` 同理。
>
> **后果**：仓库里全部 **58 个已注册参数的 `hot_reload` 都是 `false`，`on_change` 都是 `NULL`**。而 `hot_reload` 出现在两个输出路径里：
> - `param_export_json`（`:348`）——它本身就是死代码
> - `flow_registry_export_json`（`.c:509`）——活的，但**永远输出 `false`**
> - `flowctl list params` 的 🔥 标记（`src/flowctl.c:224`）——**永不出现**

> **还有一颗地雷**：如果将来有人真的注册了回调，`validate_and_set` 是**在持有 `g_mutex` 的情况下调用它的**（`:204-206`）。而那个回调如果回调进任何 `param_get_*` 或 `param_set_*`，会在非递归互斥锁上**自死锁**。目前不可达，仅仅因为没人注册回调。

---

## 5. 真正在跑的热重载：逐帧轮询

### 5.1 死掉的机制 vs 活着的机制

这是本章最重要的一张表：

| | 回调机制 | 轮询机制 |
|---|---|---|
| 入口 | `param_set_callback` + `param_enable_hot_reload` | 节点 `run()` 里的 `param_get_float` |
| 触发 | `param_set_*` 内部发现值变了 | 每一帧无条件重读 |
| 状态 | **零调用者** | **58 个参数、2 个节点** |
| 跨进程 | 靠回调 | 靠 `param_bridge` 套接字 |

**活着的那个 长这样**（`control_node.cpp:526-549`）：

```cpp
/* 热重载：每帧从 param_registry 重新读取参数，支持 flowctl param set 运行时修改 */
g.kp = param_get_float("control.pid_kp");
g.ki = param_get_float("control.pid_ki");
g.kd = param_get_float("control.pid_kd");
g.cfg_cruise_speed = param_get_float("control.cruise_speed");
...
g.mv_params.max_steer = param_get_float("control.mv_max_steer");
g.mv_tracker.setParams(g.mv_params);
```

`control_node.cpp:527-548` 一共 **22 次 `param_get_float`**，重装进 `g.*`；`:937-956` 还有 **8 次**（仅当 MPC 启用时）；`behavior_planner_node.cpp:698-714` 有 **17 次**。

**每帧 22 次字符串比较**（每次线性扫 128 个条目，最坏 2816 次 `strcmp`）——在 40 Hz 下是每秒 11 万次字符串比较。**对今天的 CPU 是完全可忽略的，但它确实不优雅。**

### 5.2 「三处都通」的纪律

`CLAUDE.md:353-360` 规定了纪律，而且**代码完全符合**：

```
新增一个可调参数，三处都要通，只做注册等于没做：
1. params_json 里加 cJSON_GetObjectItemCaseSensitive 解析分支
2. param_register_* 的默认值用 g.<字段> 而非硬编码字面量
3. 逐帧 tick 里 param_get_float 重读
```

`behavior_planner_node.cpp:697` 的注释更直白：

```cpp
/* 参数热重载（三处之三）：漏了这步，注册了也改不动，只能重启。 */
```

`behavior_planner_node.cpp:1820-1822` 解释了第 2 条为什么重要：

```cpp
/* ── 参数注册（默认值用 g.<字段>，即上面解析后的值，不用硬编码字面量，
 *    否则会把 params_json 解析到的值盖掉）。逐帧 param_get_float 重读
 *    见 BehaviorTask::run()，三处都通才能 flowctl param set 生效。 */
```

**为什么第 2 条重要**：如果注册时用硬编码默认值，`flowctl param set` 改的是 registry 里的 `current_value`，而 `run()` 读的是 `current_value`——看起来没问题。**真正的问题在第一次注册的时候**：`current_value` 被设成硬编码默认值，而 `params_json` 解析出来的值在 `g.<字段>` 里。`g.<字段>` 会被 `run()` 直接用吗？取决于节点。`control_node` 在 init 里就把 `g.kp` 赋成 `g.cfg_kp`（第 1357 行），所以顺序很关键。

### 5.3 一个故意不遵守第 2 条的地方

`control_node.cpp:1454-1457`：

```cpp
/* LTV MPC 横向控制器调参（默认启用；全部支持 flowctl param set 热重载）。
 * 用字面默认值注册（不用 g.ltv_mpc_cfg.*）：registry 在此刻 g.ltv_mpc_cfg
 * 仍为 0 初始化（init 的 default_config 在更后面才调用），若用 g.* 会注册
 * 成 0 导致默认失效。run() 每帧按 registry 值重新装配 MPC 配置。 */
```

**41 个 control 参数里有 9 个用了字面量默认值**，因为注册时 `g.ltv_mpc_cfg` 还是零初始化的。这是正确的权衡，还留了注释说明。

---

## 6. 谁在用这个系统

### 6.1 只有 2 个节点

穷举 `modules/` 里引用 `param_registry` 的文件，**只有两个**：

| 节点 | 注册 | 逐帧读 | 位置 |
|---|---|---|---|
| `control_node` | **41** | **30**（22 + 8） | `control_node.cpp:1431-1466` / `:527-548` + `:937-956` |
| `behavior_planner_node` | **17** | **17** | `behavior_planner_node.cpp:1823-1856` / `:698-714` |
| **合计** | **58** | **47** | |

`config/pipeline.json` 里有 16 个节点，**另外 14 个注册了零个参数**：

`flowsim`、`sensor_model`、`perception`、`object_tracker`、`fusion`、`navigation`、`planning`、`safety_control`、`inference`、`bev_detection`、`data_recorder`、`learner`、`model_ota`、`lane_detection`、`monitor`。

> **这 14 个节点的参数全部来自 `params_json`（启动时一次性解析进 `g.<字段>`），改不动。** 它们的参数只能改配置文件 + 重启。
>
> 这一点在第 18 章会再次出现——安全层那 30+ 个硬编码物理阈值就在 `safety_control_node` 里，它属于这 14 个之一。

### 6.2 `flowctl` 的两条命令

`src/flowctl.c:768-816` 实现了三条子命令，**全部走套接字**：

```c
/* 全部走 param_bridge 打到运行中的 flow_launcher。此前这里读写的是
 * flowctl 自己进程内那份 registry —— set 完打印"✓ updated"，跑着的
 * 车却什么都没变，是个纯粹的假象。 */
```

**这段注释本身就是一次事故记录。** 之前 `flowctl` 读写的是自己进程内那份空 registry——`set` 完打印成功标志，**跑着的车什么都没变**。修法是改成走 IPC。

| 子命令 | 请求 | 解析处 | 输出 |
|---|---|---|---|
| `param list` | `"LIST"`（`:779`） | `flowctl.c:785-790`，首行 `atoi` 取计数 | `%-30s %-8s %-12s [%s, %s]` |
| `param get <name>` | `"GET %s"`（`:798`） | 直接打印 | `%s = %s` |
| `param set <name> <val>` | `"SET %s %s"`（`:807`） | 值取自 **`argv[4]`** | `✓ %s = %s (applied to running process)` |

缓冲区：`char req[512]` 和 `char resp[16384]`（`:775-776`）。

> **`set` 的值从 `argv[4]` 取**，而不是像其他地方那样用解析出来的 `arg2`。这是个小的不一致。

### 6.3 一个真实的类型 bug

`control_node.cpp:937-939`：

```cpp
int mh = (int)param_get_float("control.mpc_horizon");
if (mh > LTV_MPC_MAX_HORIZON) mh = LTV_MPC_MAX_HORIZON;
if (mh < 1) mh = 1;
```

`control.mpc_horizon` 注册为 **float**，范围 `[5.0, (double)LTV_MPC_MAX_HORIZON]`（`:1459`，`LTV_MPC_MAX_HORIZON = 80`）。

**两个问题**：
1. `if (mh < 1) mh = 1` 是**死代码**——registry 的下限是 5.0，`param_set_float` 会先拒掉任何小于 5 的值，这个分支永远不会命中；
2. `tools/auto_tune_mpc.py:598, :630` 用 `flowctl param set control.mpc_horizon 0` 作为「关闭 MPC」的手法——**这个命令会被范围检查拒绝**，而脚本根据返回值打印 ✓ 或 ✗。

**所以那个自动调参脚本关不掉 MPC。** 结合第 17 章的结论（MPC 本来就默认关闭、`ltv_mpc_enable` 从没被任何 config 打开），这个 bug 一直没被发现。

---

## 7. 跨进程：一个 AF_UNIX 行协议

参数要能从 `flowctl`（另一个进程）改到 `flow_launcher` 里的 registry，跨越了进程边界。这是 [`param_bridge`](file:///home/caixuf/code/FlowEngine/src/core/param_bridge.c) 的职责。

### 7.1 常量与路径解析

`include/param_bridge.h:49-56`：

```c
#define PARAM_BRIDGE_DEFAULT_SOCK  "/tmp/flow_param.sock"
#define PARAM_BRIDGE_SOCK_ENV      "FLOW_PARAM_SOCK"
#define PARAM_BRIDGE_DEFAULT_PORT  18776
#define PARAM_BRIDGE_PORT_ENV      "FLOW_PARAM_PORT"
#define PARAM_BRIDGE_MAX_LINE      512
```

`resolve_sock_path`（`.c:46-51`）的解析顺序：显式参数 → `$FLOW_PARAM_SOCK` → 默认值。

> **Windows 上走的是 TCP 回环 18776 端口**（`.c:36-43, 257-280`），可以用 `$FLOW_PARAM_PORT` 覆盖。**但头文件（`:13, 17-19`）只描述了 AF_UNIX，Windows 那条 TCP 路径在头文件里没有文档。**

`ParamBridgeServer` 结构体里是 `char path[108]`（`.c:27-34`），匹配 `sockaddr_un.sun_path` 的容量。`/tmp/flow_param.sock` 只有 19 字符不会截断，**但任何超过 107 字符的环境变量覆盖都会被 `snprintf` 静默截断**（`.c:286`）。

### 7.2 线协议

`include/param_bridge.h:26-30` 记录了完整格式：

```
 *   请求:  "LIST\n"                        响应: "OK <n>\n<name> <type> <value> <min> <max>\n"×n
 *          "GET <name>\n"                  响应: "OK <value>\n"
 *          "SET <name> <value>\n"          响应: "OK <value>\n"
 *   失败:  "ERR <errno> <message>\n"
```

**一行文本，空格分隔，无引号无转义。** 服务端解析（`.c:159`）：

```c
sscanf(req, "%15s %63s %127[^\n]", verb, name, value)
```

所以：

- **含空格的值会被截断**——`SET name hello world` 只会设上 `hello`；
- **超过 63 字符的参数名静默截断**——然后 `strcmp` 匹配不上，返回 `ERR 2 unknown param`，而用户看到的名字和实际只差后缀。

服务端的两个防御设计值得一提：

- **1000 ms 读超时**（`.c:241`），注释说「加读超时防止客户端连上不发数据把服务线程挂住」；
- **`unlink` 在 `bind` 之前**（`.c:292`），注释说「清掉上次进程崩溃留下的 socket 文件，否则 bind 报 EADDRINUSE」。

### 7.3 SET 的类型转换

`apply_set`（`.c:55-91`）按类型分派：

| 类型 | 转换 | 校验 |
|---|---|---|
| `PARAM_INT` | `strtoll` | 检查尾部垃圾（`end == val_str \|\| *end`） |
| `PARAM_FLOAT` | `strtod` | 同上 |
| `PARAM_BOOL` | **精确字符串匹配** | `"true"`/`"1"` / `"false"`/`"0"`，**其余全拒** |
| `PARAM_STRING` | 直接传 | 无 |

> **bool 是大小写敏感的精确匹配**——`"True"`、`"TRUE"`、`"yes"` 都会被拒。脚本里写 `flowctl param set x True` 会失败。

错误回显做得很贴心（`.c:184-197`）：越界时**把区间一起回给用户**，注释说「越界是最常见的失败，把区间回给用户，省得他去翻源码」。

```c
if (rc == ERR_INVALID_PARAM) {
    snprintf(out, out_size, "ERR %d out of range [%s, %s]", rc, min_s, max_s);
```

### 7.4 客户端的静默截断

`param_bridge_client_request`（`.c:325-430`）连到 EOF（这样多行 LIST 才能工作），然后剥掉 `OK ` 前缀（`:415-419`）或解析 `ERR`。

> **`read` 循环（`:399, 406`）在缓冲区满时 `break` 出去，不给调用方任何错误信号。**
>
> `flowctl` 传的是 `char resp[16384]`（`flowctl.c:776`）。128 个参数的完整 LIST 响应约 7.7 KB，**装得下**。但如果注册满了 128 个参数且名字都很长，理论上可能溢出——**而溢出会静默截断，不报错**。

---

## 8. `flow_registry`：另一张注册表

除了参数，还有一种注册：把节点、话题、插件、类型登记下来，供 `flowctl topology` 和仪表盘导出。

[`include/flow_registry.h`](file:///home/caixuf/code/FlowEngine/include/flow_registry.h) + [`src/core/flow_registry.c`](file:///home/caixuf/code/FlowEngine/src/core/flow_registry.c)。

### 8.1 容量

`flow_registry.h:28-34`：

```c
#define FLOW_REGISTRY_MAX_TASKS    64
#define FLOW_REGISTRY_MAX_TOPICS   128
#define FLOW_REGISTRY_MAX_PLUGINS  32
#define FLOW_REGISTRY_MAX_TYPES    128
#define FLOW_REGISTRY_NAME_LEN     64
#define FLOW_REGISTRY_PATH_LEN     256
#define FLOW_REGISTRY_MAX_IO        8
```

> **`FLOW_REGISTRY_MAX_TYPES` 在头文件第 31 行定义了一次，又在 `flow_registry.c:160` 用相同值重复定义了一次。** 相同的记号序列，所以能编译过去——但这是一处潜在的漂移点。

**`FLOW_REGISTRY_MAX_IO = 8`** 限制每个任务的 inputs / outputs / params，以及每个插件的 tasks / types。**第 9 个及以后的静默丢弃，无错误无日志。**

### 8.2 四个注册函数

| 函数 | 记录什么 | 生产调用点 |
|---|---|---|
| `flow_registry_register_task`（`.c:37-78`） | 名字、描述、插件路径、≤8 输入、≤8 输出、≤8 参数 | `flow_launcher.c:839-842` |
| `flow_registry_register_topic`（`.c:105-132`） | 名字、`type_id`、`TopicQos` | `flow_launcher.c:845, 848` |
| `flow_registry_register_type`（`.c:168-170`） | **纯转发**，自己不记录 | **零调用者** |
| `flow_registry_register_plugin`（`.c:276-305`） | 名字、路径、≤8 任务、≤8 类型 | `flow_launcher.c:852` |

`flow_registry_register_type` 有意思（`.c:168-170`）：

```c
int flow_registry_register_type(const TypeRegistryEntry* entry) {
    return serializer_register_type(entry);
}
```

它自己不记录任何东西。真正的记录走 `flow_registry_on_type_registered`（`.c:172-193`），由 `serializer.c:112` 和 `:129` 回调。**这是唯一一条活的跨模块数据流。**

### 8.3 启动器传下来的数据有两个缺陷

`flow_launcher.c:839-852`：

```c
flow_registry_register_task(nd->name, nd->name, nd->library, inputs, outputs, NULL);
...
flow_registry_register_topic(nd->inputs[j], 0, NULL);
...
flow_registry_register_plugin(nd->name, nd->library, tasks, NULL);
```

**缺陷一：`desc` 传的是节点自己的名字。** `nd->name` 传给了 `description` 参数——这是重复字段，导出 JSON 里 `desc` 和 `name` 一样，没���信息量。

**缺陷二：`type_id` 恒为 0。** 启动器没有对载荷类型做 FNV-1a 哈希，所以导出 JSON 里每个话题的 `type_id` 都是 `0x00000000`。**这个字段对真实管道毫无意义。**

**缺陷三：`params` 传 `NULL`**——所以 `TaskMeta.params[]` 在生产环境**永远是空的**。

### 8.4 导出的 JSON：7 个顶层键

`flow_registry_export_json`（`.c:432-543`）用 cJSON 构建，`cJSON_PrintUnformatted` 输出（`:538`）：

| 键 | 类型 | 每项字段 |
|---|---|---|
| `tasks` | 数组 | `name`, `desc`, `plugin`, `inputs`[], `outputs`[] |
| `topics` | 数组 | `name`, `type_id`（十六进制字符串） |
| `plugins` | 数组 | `name`, `path`, `tasks`（**数字**）, `types`（**数字**） |
| `schemas` | 数组 | `topic`, `type`, `size` |
| `params` | 数组 | `name`, `type`, `value`, `hot_reload` |
| `types` | 数组 | `name`, `type_id`（十六进制字符串）, `size` |
| `summary` | 对象 | 六个计数 |

三个坑：

1. **plugin 的 `tasks` / `types` 是计数不是数组**（`.c:479-480` 用 `cJSON_AddNumberToObject`），而顶层 `tasks[]` 是数组。**同一个概念在同一个文档里两种表示。**
2. **`params` 数组上限 64**（`.c:499-500`），但 `summary.params` 报的是真实的 `param_count()`（`:534`）——**128 个参数时这两个数会不一致。**
3. **`params` 项没有 `desc`**（对比 `param_export_json` 里有）。

活着的调用者：`monitor_node.c:1023`（1 Hz，有缓存——`:1015-1017` 注释说之前是每帧 26 ms 的代价）、`flowctl.c:687`（topology 点图）、`flowctl.c:845`（仪表盘）。

### 8.5 那个从没被用过的宏

`flow_registry.h:180-184`，头文件里唯一的宏：

```c
#define FLOW_REGISTRY_DECLARE_PLUGIN(pname, pver, pdesc) \
    __attribute__((constructor)) \
    static void _flow_registry_declare_##pname(void) { \
        flow_registry_register_plugin(#pname, NULL, NULL, NULL); \
    }
```

**`pver` 和 `pdesc` 被静默丢弃了。** 它们在宏展开体里根本不出现。展开结果只是用字符串化的名字注册，`path=NULL, tasks=NULL, types=NULL`。

头文件 `:174-179` 的文档注释把用法写成 `FLOW_REGISTRY_DECLARE_PLUGIN(my_plugin, "1.0.0", "My ADAS plugin")`，暗示版本和描述被记录了。**它们没有。** 而且 `PluginMeta` 结构体（`:62-70`）里**根本没有 `version` 或 `description` 字段**。

**这个宏本身零使用**——仓库里没有任何插件调用它。

---

## 9. 配置文件解析

[`include/config_manager.h`](file:///home/caixuf/code/FlowEngine/include/config_manager.h) + [`src/core/config_manager.c`](file:///home/caixuf/code/FlowEngine/src/core/config_manager.c)，cJSON 解析。

### 9.1 一个曾经很危险的默认值

`config_manager.c:38-42`：

```c
    /* ... */
    cfg->scheduler.mode = 1;  /* SCHEDULER_MODE_CHOREO */
```

注释解释了原因：PR #72 的启动器硬编码了 CHOREO，而 `calloc` 出来的零值会把外部配置静默降级成 CLASSIC(0)。**DAG 编排 vs 每任务一线程轮询，是巨大的行为差异。**

### 9.2 `params` 的双格式陷阱

`config_manager.c:195-201` 有一段长注释，说的是本仓库最隐蔽的一个坑：

```c
/* ── params (key=value) ──
 * pipeline.json 里 "params" 既可以写成内嵌 JSON 对象，也可以写成
 * 转义后的 JSON 字符串 (当前 config/pipeline.json 全部采用后者)。
 * 只处理 Object 会导致字符串形式的 params 被静默丢弃，节点全部
 * 退回硬编码默认值 (例如 safety_control.max_throttle 变成 0.85
 * 而不是配置的 1.0, time_headway 变成 1.8 而不是 1.3) ... */
```

**`pipeline.json` 里的 `params` 可以是内嵌对象，也可以是转义后的字符串，而当前配置全用后者。** 只处理前者的后果：所有参数静默退回硬编码默认值。

**注意注释里举的例子**：`safety_control.max_throttle` 变成 0.85 而不是 1.0——这正是第 19 章那个「`max_stderr` 0.35 vs 0.22」问题的同类。

处理代码在 `:202-211`，`cJSON_IsObject` 和 `cJSON_IsString` 两个分支都处理。存储形式是**扁平的重新序列化字符串**，写进 `ProcessConfig.params[1024]`。

### 9.3 那个 1024 是一次热修复

`config_manager.h:73-77`：

```c
/* Per-node params (key=value strings from JSON)。
 * 必须 ≥ NodeDef.params_json[1024]（flow_launcher.c:53），否则超长 params
 * 会在 config_manager.c:193/197 的 snprintf 处被截断 ... 原值 256 截断了 sim_world
 * (272B)/traversability(300B)/control(443B) 等节点的 params. */
char params[1024];
```

**原值 256 截断了多个节点的 params**——`sim_world`（272 B）、`traversability`（300 B）、`control`（443 B）全都超了。这个 bug 的表现是「配置怎么改都不生效」。

> **头文件引用的 `config_manager.c:193/197` 行号已经过时**——经过编辑后实际的 `snprintf` 在 `:206` 和 `:210`。这是个小小的文档腐化。

`config_save`（`.c:272-399`）用 `cJSON_Print`（**带缩进**）写回。**`profile` 不保存**（读进来了但没写回）。

---

## 10. 跨进程可见性：一个平台相关的承重假设

这一节可能是全章最不直观的内容。

`include/param_bridge.h:20-24` 陈述了理由，`CMakeLists.txt:497-510` 为唯一需要它的平台实现了它：

```cmake
# PE/COFF 没有 ELF 的宿主符号 interposition。若 param_registry.c 被静态链接
# 到 launcher 和每个节点 DLL，每个映像都会得到独立的 g_params[]，导致节点
# 注册的参数对 flowctl 不可见。Windows 使用一个小型共享运行库作为参数状态的
# 唯一实例；其它平台继续依赖现有静态核心/扁平命名空间行为，保持零变化。
if(WIN32)
    list(REMOVE_ITEM CORE_SOURCES src/core/param_registry.c)
    add_library(flowengine_param_runtime SHARED src/core/param_registry.c)
```

**在 Windows 上**，`param_registry.c` 被编译成一个独立的共享库 `flowengine_param_runtime`。因为 PE/COFF 没有 ELF 那样的符号抢占，没这一步的话每个 DLL 映像都会得到自己的 `g_params[]` 副本。

**在 Linux/macOS 上**，`param_registry.c` 留在静态的 `flowengine_core` 里（`:438, 513`），正确性依赖 **ELF 全局符号抢占**——把 `dlopen` 出来的插件对 `param_register_float` 的引用绑定到启动器的那一份。头文件 `:23-24` 声称「已实测确认」，`ci/gates/plugin_symbol_check.py:8` 守护着启动器导出这一侧。

> [!IMPORTANT]
> **这是一个承重的、平台相关的假设，而且它在 Linux 上是靠一个不保证的语言特性成立的。**
>
> 三个前提：
> 1. `g_params` / `g_param_count` 必须是**全局符号**（非 static、非 hidden 可见性）；
> 2. 插件的符号查找必须**先搜全局**再搜本地（标准的 ELF 动态链接器行为）；
> 3. `-fvisibility=hidden` 或 LTO **不能**改变这个行为。
>
> 任何一条被破坏，参数就会在插件和启动器之间**静默分裂**——节点注册了 58 个参数，启动器一个都看不到。症状是「`flowctl param set` 报 `ERR 2 unknown param`」，而节点读到的永远是默认值。
>
> **CI 里有 `plugin_symbol_check.py` 守着一侧，但没有测试真的验证过「插件注册的参数，launcher 侧能读到」。** 这是本章最值得补的一个测试。

---

## 11. 测试

全部集中在 [`tests/test_new_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_new_modules.c)，ctest 名 `new_module_tests`（`CMakeLists.txt:1042-1048`）。

**`param_registry` 4 个**（`:932-935` 调用）：

| 测试 | 行 | 断言 |
|---|---|---|
| `test_param_int_range` | `:818-831` | 注册/默认值/正常 set/超上限拒绝/低于下限拒绝/**被拒的 set 不得改变值** |
| `test_param_float_range` | `:833-844` | 同上（float） |
| `test_param_not_found` | `:846-853` | 未知名 `ERR_NOT_FOUND`，NULL 名 `ERR_INVALID_PARAM` |
| `test_param_registry_bridge` | `:855-868` | `flow_registry_list_params` 能看到 `param_registry` 的条目 |

**`flow_registry` 9 个**（`:920-928` 调用）：`test_freg_register_task`、`_list_tasks`、`_topic`、`_schema`、`_list_schemas`、`_plugin`、`_json_export`（`:767`，断言 7 个顶层键全在，`:771-777`）、`_total_count`、`_list_types`。

**`param_bridge`：零测试。** 仓库里没有任何测试文件引用 `param_bridge`、`PARAM_BRIDGE_DEFAULT_SOCK` 或 `FLOW_PARAM_SOCK`。

> **这意味着**套接字协议、SET 的类型强制转换、越界回显错误路径——**全部无测试覆盖**。而这些正是 `flowctl param set` 唯一的执行路径。
>
> `param_set_callback`、`param_enable_hot_reload`、`param_export_json`、`FLOW_REGISTRY_DECLARE_PLUGIN` 同样零测试（它们是死代码，测了也没意义）。

**一个顺序依赖**：`test_param_int_range` 必须在 `test_param_registry_bridge` 之前跑（后者按名字 `"test.speed"` 查找），`:932` 确实在 `:935` 之前。但 `test_freg_list_tasks`（`:921`）只断言 `n >= 1`，**所以注册表为空时它也通过**。

---

## 12. 死代码与隐患清单

### 12.1 死代码（零生产调用者）

| 符号 | 位置 | 说明 |
|---|---|---|
| `param_set_callback` | `param_registry.c:134` | **整个回调机制** |
| `param_enable_hot_reload` | `param_registry.c:297` | **整个 `hot_reload` 标志** |
| `param_export_json` | `param_registry.c:319` | 手写 JSON，**无转义** |
| `param_register_bool` / `_string` | `:92` / `:113` | 零调用者 |
| `param_get_bool` / `_string` | `:173` / `:186` | 零调用者 |
| `FLOW_REGISTRY_DECLARE_PLUGIN` | `flow_registry.h:180` | 零使用；且丢弃 `pver`/`pdesc` |
| `flowctl list params` | `flowctl.c:212-228` | 读 flowctl 自己那份**永远为空**的本地 registry；🔥 标记永不出现 |
| `flow_registry_unregister_*` | `.c:332/349/366` | 零调用者 |
| `TaskMeta.params[]` | `flow_registry.h:46-47` | 启动器传 `NULL`（`flow_launcher.c:842`） |
| `*.registered` 字段 | `flow_registry.h:48/57/69` | 注册时置 true，**从未被读** |
| 「bootstrap 预加载」路径 | `param_registry.c:16-19,36-40` | 5 处描述，0 处实现 |

### 12.2 无锁共享状态（数据竞争）

- `param_get_entry`（`:281-286`）——被 bridge 服务线程调用
- `param_list_all`（`:288-293`）——`memcpy` 整个数组，无锁
- `param_count`（`:295`）
- `param_enable_hot_reload`（`:297-305`）
- `flow_registry_type_count`（`flow_registry.c:195-199`）

### 12.3 锁顺序

`flow_registry_export_json` 持有 `g_mutex`（`flow_registry.c:433`），然后调 `flow_registry_list_params` → `param_list_all`，以及 `flow_registry_param_count` → `param_count`。

**当前安全仅仅因为 `param_list_all` 和 `param_count` 不加锁。** 任何给它们加锁的改动都会创建一个没有文档记录的嵌套顺序。

### 12.4 固定容量与静默截断

| 容量 | 值 | 溢出行为 |
|---|---|---|
| `PARAM_MAX_ENTRIES` | 128（用了 58） | 返回 `ERR_OVERFLOW`，**无调用方记录** |
| `FLOW_REGISTRY_MAX_IO` | 8 | **静默丢弃第 9 个+** |
| `flow_registry_list_params` | 64（`:391-392`） | **静默丢一半**，且与 `summary.params` 不一致 |
| `ProcessConfig.params` | 1024 | 静默 `snprintf` 截断 |
| `param_bridge` 客户端缓冲 | 16384 | **静默截断，不报错** |
| `ParamBridgeServer.path` | 108 | 超长环境变量静默截断 |
| `MAX_TOPICS_PER_NODE` | 16（`flow_launcher.c:57`） | 与 `discovery.h:38` 的 32 **不一致** |
| `FLOWCTL_MAX_TOPICS` | 64（`flowctl.c:37`） | `flowctl list topics` 只能看到一半 |

### 12.5 文档腐化

- `include/param_registry.h:9-10` 的用法示例写的是 **`param_registry_register_int` / `param_registry_register_float`**——真实名字是 `param_register_int` / `param_register_float`。**示例里的函数不存在。**
- `include/config_manager.h:75` 引用的 `config_manager.c:193/197` 行号已过时（实际 `:206`/`:210`）。
- `include/param_bridge.h:13, 17-19` 只描述 AF_UNIX，**Windows 的 TCP 路径无文档**。

---

## 13. 源码与资源对照

| 模块 | 源码文件 | 核心符号 | 职责与要点 |
|---|---|---|---|
| **参数表** | [`include/param_registry.h`](file:///home/caixuf/code/FlowEngine/include/param_registry.h)<br>[`src/core/param_registry.c`](file:///home/caixuf/code/FlowEngine/src/core/param_registry.c) | `param_register_*` / `param_get_*` / `param_set_*`<br>`create_param`（`:26`）<br>`validate_and_set`（`:200`，static） | 128 槽进程级固定表 + 1 把锁。int/float 越界会被拒。**`on_change`/`hot_reload` 机制零调用者**。锁使用不一致 |
| **流程注册表** | [`include/flow_registry.h`](file:///home/caixuf/code/FlowEngine/include/flow_registry.h)<br>[`src/core/flow_registry.c`](file:///home/caixuf/code/FlowEngine/src/core/flow_registry.c) | `flow_registry_register_task/topic/type/plugin`<br>`flow_registry_export_json`（`.c:432`）<br>`FLOW_REGISTRY_DECLARE_PLUGIN`（**零使用**） | 任务/话题/插件/类型/Schema 的元数据登记，导出 7 键 JSON。`MAX_IO=8` 静默截断；启动器传的 `desc` 和 `type_id` 无信息量 |
| **跨进程桥** | [`include/param_bridge.h`](file:///home/caixuf/code/FlowEngine/include/param_bridge.h)<br>[`src/core/param_bridge.c`](file:///home/caixuf/code/FlowEngine/src/core/param_bridge.c) | `param_bridge_server_start/stop`<br>`param_bridge_client_request`<br>`apply_set`（`.c:55`） | AF_UNIX 行协议（Windows 走 TCP 18776）。`LIST`/`GET`/`SET`。**零测试**。值含空格会被截断，bool 大小写敏感 |
| **配置解析** | [`include/config_manager.h`](file:///home/caixuf/code/FlowEngine/include/config_manager.h)<br>[`src/core/config_manager.c`](file:///home/caixuf/code/FlowEngine/src/core/config_manager.c) | `config_load` / `config_save` / `config_free` | cJSON。**`params` 字段同时支持内嵌对象和转义字符串**（当前配置全用后者）。`params[1024]` 是一次截断热修复的产物 |
| **话题常量** | [`include/topic_registry.h`](file:///home/caixuf/code/FlowEngine/include/topic_registry.h) | 38 个 `TOPIC_*` 宏 | 编译期话题名常量。约定 `TOPIC_<类别>_<名字>` → `"类别/名字"`。`TOPIC_INFERENCE_CONTROL_DELTA`（`:68`）**违反这个约定**。`:111-222` 是 110 行的生产者/消费者图，CI 可自动解析 |
| **命令行** | [`src/flowctl.c`](file:///home/caixuf/code/FlowEngine/src/flowctl.c) | `param list/get/set`（`:768-816`）<br>`list params`（`:212-228`，**读空的本地 registry**） | 全部走套接字打到运行中的 launcher。`:769-771` 注释记录了「改自己进程那份空 registry，打印成功但车没变」的旧 bug |
| **消费方** | [`control_node.cpp:1431-1466`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/control_node.cpp)<br>[`behavior_planner_node.cpp:698-714`](file:///home/caixuf/code/FlowEngine/modules/adas_nodes/behavior_planner_node.cpp) | 41 + 17 个 `param_register_float`<br>47 次逐帧 `param_get_float` | **16 个节点里只有这 2 个实现了逐帧热重载**。另 14 个节点的参数改不动 |
| **平台特例** | [`CMakeLists.txt:497-525`](file:///home/caixuf/code/FlowEngine/CMakeLists.txt)<br>[`ci/gates/plugin_symbol_check.py`](file:///home/caixuf/code/FlowEngine/ci/gates/plugin_symbol_check.py) | `flowengine_param_runtime`（仅 WIN32） | Windows 必须用共享库（PE/COFF 无符号抢占）；Linux 依赖 ELF 全局符号抢占，**无端到端测试** |
| **测试** | [`tests/test_new_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_new_modules.c) | ctest `new_module_tests`：4 个 param + 9 个 freg | 覆盖越界拒绝和 JSON 导出。**`param_bridge` 零覆盖**；死代码不测 |

---

## 14. 思考题

1. **把死掉的回调机制删掉，还是修好它？**：
   第 4 节证明了 `on_change` + `hot_reload` 零调用者，而第 5 节证明轮询机制在实际工作。两种选择：
   (a) **删掉** `ParamChangeCallback`、`param_set_callback`、`param_enable_hot_reload`、`hot_reload` 字段，以及 `flowctl list params` 的 🔥 标记和 JSON 里的 `hot_reload` 字段。代价是什么？（提示：第 8.4 节说 JSON 导出有 `hot_reload` 字段，`monitor_node` 消费它。）
   (b) **修好它**——让 `param_enable_hot_reload` 真的被调用。但这要求每个节点在注册后显式开启，且第 4.2 节那个「回调在持锁时被调用」的地雷必须先解决（改成先解锁再回调？还是用递归锁？还是把回调派发到一个专用线程？）。
   (c) 第三条路：**承认两种模式并存**，把 `hot_reload` 重新定义为「这个参数的值变化是否需要**通知**下游」（区别于「是否**可**被修改」），然后给 58 个参数打上真实的标记。这条路的代价是概念负担。哪条路你选，为什么？

2. **修掉 14 个节点的热重载缺口**：
   第 6.1 节指出 16 个节点里只有 2 个实现了逐帧重读。其余 14 个（包括安全层）的参数只能改配置文件 + 重启。
   (a) 请为**安全层**（`safety_control_node`）设计逐帧热重载方案。注意第 18 章 9.2 节列的那 30+ 个硬编码物理阈值——**它们全都应该可调吗？** 哪些改了会立刻降低安全性（比如制动减速度 5.0 m/s²、TTC 触发 2.5 s）？请提出一个分级方案：哪些参数开放热重载，哪些必须编译期固定。
   (b) 每帧 22 次 `param_get_float` 意味着每秒 11 万次字符串比较（40 Hz × 22 × 128 条目）。如果要优化，有两条路：(i) 每次注册时分配一个**整数 slot 索引**，`run()` 里改读 `param_get_by_slot(idx)`；(ii) 引入**脏标记**——`param_set_*` 置一个全局 `g_generation` 计数器，`run()` 每帧只在计数器变化时重读。评估两条路的侵入性和 bug 风险，特别是方案 (ii) 在「多个节点共享同一个 registry」时如何避免重复读。
   (c) 更根本的问题：**`g.generation` 这个方案在多线程下安全吗？** 注意第 12.2 节列的无锁函数——如果 `param_set_*` 递增 `g_generation` 而 `run()` 无锁读它，这本身就是一个新的数据竞争。请给出一个既避免全量重读、又不引入竞争的方案。

3. **`auto_tune_mpc.py` 的参数名已经对不上了**：
   第 6.3 节发现 `tools/auto_tune_mpc.py:65-70` 的 `MPC_PARAMS` 列的是 `mpc_q_y`、`mpc_q_theta`、`mpc_r_a`、`mpc_r_ddelta`，而 registry 里是 `control.ltv_q_y` 和 `control.ltv_r_ddelta`，**且 `control.mpc_q_theta` 和 `control.mpc_r_a` 根本不存在**。脚本往这些名字写会一直收到 `ERR 2 unknown param`。
   (a) 这个脚本现在还能用吗？请追踪它完整的调用链（第 7 章讲的 `flowctl param set` → `param_bridge` → `param_set_float`），指出每一环的实际行为。
   (b) `auto_tune_mpc.py:64` 的注释说「通过 `inference_node` → `model.txt` 闭环」，说明**存在两条通道**：一条是 `param set` 直写 registry，一条是通过 `inference_node` 写模型文件。两条通道的参数名已经分叉了。请设计一个方案让它们统一——是让 `inference_node` 也从 registry 读？还是废弃其中一条？
   (c) 一个更普适的问题：**怎么让「配置漂移」这类问题在 CI 里被发现？** `ci/gates/` 下已有 7 个静态检查脚本。设计一个新 gate：扫描 `tools/*.py` 里出现的所有 `param set` 参数名，与 `param_register_*` 的调用点做交叉比对，不一致就失败。你需要用什么手段提取 Python 里的字符串字面量？为什么这个检查必须放在 CI 而不是文档里？

4. **`param_bridge` 零测试，怎么补**：
   第 11 节指出套接字协议、SET 类型转换、越界回显全部无测试，而这些是 `flowctl param set` 的唯一执行路径。
   (a) 设计测试方案。注意 `param_bridge` 的服务端是**一个后台线程**（`param_bridge_server_start`），测试需要能确定性地知道「服务端已就绪」——目前只有 `tools/auto_tune_mpc.py:242` 用 `SOCK_FILE.exists()` 轮询等待，这在测试里是不可靠的（竞态）。请提出一个可靠的同步手段。
   (b) 至少为 `apply_set` 写 8 个用例：int 正常/越界下界/越界上界/带尾随垃圾（`"12abc"`）、float 同上、bool 的 `"true"`/`"True"`/`"yes"`、string 含空格。**每个用例锁住的是什么性质？** 特别是「带空格的值被截断」这个行为——它应该被锁死为**当前行为**（作为已知限制记录下来），还是应该被改掉后锁死为**期望行为**？
   (c) 第 10 节指出一个承重但无测试的假设：**插件注册的参数，launcher 侧能读到**。请设计一个端到端测试：一个最小插件 `.so`，在 `init` 里调 `param_register_float("e2e.test_param", ...)`，然后从主进程侧调 `param_get_float("e2e.test_param")` 断言能读到。**如果这个测试在某个平台上失败（例如 Windows），你怎么让它在 CI 里既暴露问题又不阻塞构建？** 参考第 10 节说的 Windows 已经用共享库绕过了这个问题——那么这个测试在 Windows 上应该通过还是失败？这说明我们测的到底是「功能」还是「实现」？
