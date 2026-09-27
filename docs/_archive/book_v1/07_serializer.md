<!-- @archive-banner -->

> **归档说明**：本章是 docs/book v1（2026-09 前后"按源码逐条重建"产物）的版本。
> 内容大量绑定代码行号、文件名、函数符号，与代码强耦合 —— 维护成本高且易过时。
> v2 重写计划：见 `docs/book/README.md` 的写作风格约束 + 真技术书范式。
> 本归档文件保留供历史参考；引用时用 `docs/_archive/book_v1/03_message_bus.md` 而非 `docs/book/`。
> **现章节**：BOOK.md 第 06 章「类型 ID、IDL 与序列化」。

# 第 06 章：类型 ID、IDL 与代码生成 —— 消息怎么变成字节

> **本章导读**：
> 消息层里最危险的一行代码，是那个谁都不想写的强制转换。发布者发的是 `ControlCmd`，订阅者要是照着 `Behavior` 去解引用，读出来的字段全是错位的值——而编译器一句话都不会说。
>
> KunAutoDrive 的答案是：**先用 IDL 描述消息，由代码生成器在构建期算出每个类型的指纹，再在订阅侧校验指纹。** 本章讲清三件事：
>
> 1. `msg/adas_msgs.msg` 这个 IDL 和 `tools/msg_codegen.py` 这条生成流水线；
> 2. `type_id` 究竟是什么的哈希——**不是类型名，是「字段签名串」**；
> 3. 那条让数据静默错位的陷阱：**wire 布局 vs 内存布局**。
>
> 但先说破旧稿最关键的一处错误：
>
> > **旧稿说「0 次拷贝（直接内联内存映射）」，并把这当作方案的核心优势。这是错的——第 05 章已经证明 `publish` 必定 `memcpy` 一次。而旧稿展示的 `msg_cast` 代码（`TypeTraits<T>`、`LOG_ERROR`、`data_size < sizeof(T)`、`msg_cast_c` / `MSG_CAST` 宏）在仓库里一个都不对。**

---

## 1. IDL：一份 294 行的消息定义

### 1.1 文件本身

[`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg)（294 行）头部就是一份自述（第 1-20 行）：

```
# 每个 struct 由 msg_codegen.py 生成:
#   - C struct 定义
#   - TYPE_ID 常量 (FNV-1a hash)
#   - SCHEMA_VERSION 常量
#   - serialize/deserialize 函数
#   - register_type() 初始化函数
...
# 域格式: <type> <name> [= <default>]
# 数组格式: <type> <name>[N]
# 枚举格式: enum <Name> { <values> }
# 嵌套引用: 直接用 struct 名作为类型
```

**20 个 struct、3 个枚举**（`ObstacleType`、`Gear`、`BehaviorCommand`）。

**旧稿举的 `struct VehiclePose` 例子在 IDL 里不存在。** 真实的位姿类型叫 `Localization`。

### 1.2 真实的样子

```
enum Gear {                     # :32-36
    GEAR_REVERSE = -1
    GEAR_NEUTRAL = 0
    GEAR_DRIVE   = 1
}

struct LidarPoint {             # :51-57  单个点
    float   x
    float   y
    float   z
    float   intensity
    uint64  time_offset_us
}

struct LidarPointCloud {        # :59-65  一帧点云
    uint32      frame_id
    uint64      timestamp_us
    uint32      count
    uint32      sensor_id
    LidarPoint  points[2048]
}
```

`msg/adas_msgs.msg:49-50` 的注释说明了为什么把单点和点云分开：「与单点 LidarFrame 分离：`count <= 2048`」。

### 1.3 解析器是手写正则，不是 PEG

`tools/msg_codegen.py:113-220` 定义 `IDLParser`——**逐行正则匹配**：

```python
195:  m = re.match(r'(\w+)\s+(\w+)(?:\[(\d+)\])?\s*(?:=\s*(.+))?$', line)
```

三个可证实的局限：

1. **默认值被丢弃**。`:201` 的注释说得很直白：`# default_val = m.group(4)  # reserved for future`——IDL 支持 `= <default>` 语法，解析后直接扔掉。
2. **`#` 注释被无条件剥离**（`:136-137`），所以字符串字面量里不能含 `#`。
3. **依赖解析是单遍的**。`:185-188` 只检查「**已解析过**的」struct：

```python
185:  if base_type in self.structs:      # ← 只看已解析过的
186:      deps.append(base_type)
```

**这意味着前向引用会被漏掉。** `LidarPointCloud` 引用 `LidarPoint` 恰好是逆序声明所以能工作；但如果 IDL 把 `Obstacle` 写在 `ObstacleList` 之后，`Obstacle` 就进不了 `depends_on`，生成的头文件也不会 `#include` 它。**这是一个没有任何文档的隐式约束：被嵌套引用的类型必须先声明。**

### 1.4 类型映射

`msg_codegen.py:47-70`：

```python
PRIMITIVE_SIZES = {
    'bool':    1,  # uint8 in serialization
    'uint8':   1,  'int8':    1,  'uint16': 2,  'int16':  2,
    'uint32':  4,  'int32':   4,  'uint64': 8,  'int64':  8,
    'float':   4,            # IEEE 754 single
    'float64': 8,            # IEEE 754 double
    'char':    1,            # single byte (arrays = fixed strings)
}
```

**两个值得注意的映射：**

- `bool` 在**线格式里是 1 字节**；
- **枚举被强制为 `int8_t`**（`:210`）：

```python
210:  c_type = 'int8_t'  # enums are serialized as int8
```

> ⚠️ **这是一个类型截断隐患。** `Gear` 定义了 `GEAR_REVERSE = -1`，int8 装得下；但如果某枚举加一个 `= 200`，会**静默截断为 −56**，没有任何编译警告或运行期检查。反序列化也不校验枚举合法性。

### 1.5 构建期怎么调

`CMakeLists.txt:134-167`：

```cmake
137: set(MSG_CODEGEN "${CMAKE_SOURCE_DIR}/tools/msg_codegen.py")
138: set(GEN_DIR     "${CMAKE_BINARY_DIR}/gen")
...
156: add_custom_command(
157:     OUTPUT ${GEN_HEADER}
158:     COMMAND ${Python3_EXECUTABLE} ${MSG_CODEGEN} "${CMAKE_SOURCE_DIR}/${MSG_FILE}"
159:             --split --output-dir ${CMAKE_BINARY_DIR}
160:     DEPENDS ${MSG_CODEGEN} ${CMAKE_SOURCE_DIR}/${MSG_FILE}
162: )
```

**旧稿给的命令行 `python3 tools/msg_codegen.py msg/adas_msgs.msg build/gen/adas_msgs_gen.h` 走的是 monolith 模式，构建从不调用它。** 实际用 `--split --output-dir`，产出 23 个文件。

> ⚠️ **一个真实的增量构建缺陷**：CMake 只声明了 1 个 `OUTPUT`（`adas_msgs_gen.h`），而 `--split` 实际写 23 个文件。**其余 22 个对增量构建不可见**——改了 IDL 触发重新生成是对的，但反过来「生成物被手工改过」不会被检出。

---

## 2. `type_id` 究竟是什么的哈希

### 2.1 FNV-1a 本身

`src/core/serializer.c:15-21`：

```c
15: uint32_t fnv1a_hash(const uint8_t* data, size_t len) {
16:     uint32_t hash = FNV1A_INIT;
17:     for (size_t i = 0; i < len; i++) {
18:         hash = (hash ^ data[i]) * 0x01000193u;
19:     }
20:     return hash;
21: }
```

标准 FNV-1a 32 位：offset basis `0x811c9dc5`、prime `0x01000193`（`serializer.h:51,55`）。

> **旧稿说这是 `static inline` 写在 `serializer.h` 里——它是一个外部函数**，在 `.c` 里。头文件里的 `fnv1a_byte`（`:54`）和 `fnv1a_update`（`:59`）**零调用者**。
>
> 另有一个 `ser::fnv1a_const`（`serializer.h:295-303`）号称「编译期 FNV-1a」，**也是零调用者**——而且它用了 `__builtin_strlen`，**MSVC 下连编译都过不了**（项目有 Windows 构建线）。

### 2.2 被哈希的不是类型名，是字段签名串

**这是本章最重要的一条。** `tools/msg_codegen.py:283-300`：

```python
283:  def _sig_string(self, s: StructDef) -> str:
284:      """Build type signature string for FNV-1a hashing."""
285:      parts = []
286:      for f in s.fields:
287:          if f.is_nested:
288:              # Include nested struct's full signature
289:              nested = self.structs.get(f.nested_typename)
290:              if nested:
291:                  inner = self._sig_string(nested)      # 递归展开
292:                  parts.append(f"{inner}{...}")
293:              else:
294:                  parts.append(f"{f.idl_type}_{f.name}")
295:          else:
296:              parts.append(f"{f.idl_type}_{f.name}")
297:      return f"{s.name}:" + ",".join(parts)
299:  def _type_id(self, s: StructDef) -> int:
300:      return fnv1a_hash(self._sig_string(s))
```

生成的头文件里能直接看到结果（`build/gen/ControlCmd.h:22`）：

```c
/* Sig: ControlCmd:uint32_seq,float_throttle,float_brake,float_steering,Gear_gear,bool_e... */
```

**三个后果：**

1. ✅ **加字段必然改 `type_id`**——签名串含全部字段名 + 类型；
2. ⚠️ **改字段就改 `type_id`，老订阅者直接被拒收**——这与旧稿 L143 说的「加字段 + 升版本号就能向后兼容」**直接矛盾**；
3. ⚠️ **嵌套结构体改字段会连锁改掉所有父类型的 `type_id`**（`:288-292` 递归展开）。

### 2.3 `schema_hash` 是另一套哈希，敏感度不同

`msg_codegen.py:302-311`：

```python
302:  def _layout_string(self, s: StructDef) -> str:
307:          parts.append(f"{f.name}:{f.idl_type}:{arr}")
308:      return f"{s.name}#" + ";".join(parts)
```

**`schema_hash` 不递归进嵌套结构体。** 所以：

| 改动 | `type_id` | `schema_hash` |
|---|---|---|
| 改本类型的字段名/类型 | **变** | **变** |
| 改嵌套类型的内部字段 | **变**（递归） | **不变**（不递归） |

**两个指纹的敏感度不同，这是一个值得记住的设计细节**（也是第 18 章 `serializer_check_compat` 三判别的基础）。

### 2.4 一个「半真」的标题

旧稿标题写「类型 ID 是算出来的，不是分配出去的」。**对 IDL 生成的 20 个类型成立，对手写类型不成立。**

`flowsim_node.cpp:82-90` 和 `safety_control_node.cpp:38-40` 有一批**手写臆造的 type_id**，它们既不在 registry 里，也不是 FNV-1a 产物。

> 🚩 **其中两个还是错的。** `safety_control_node.cpp:38-39`：
> ```cpp
> 38: constexpr uint32_t CONTROL_RAW_TYPE_ID = 0x871712d1u;  /* CONTROLRAW_TYPE_ID (adas_msgs_gen.h) */
> 39: constexpr uint32_t CONTROL_CMD_TYPE_ID = 0x2D95C6D2u;  /* CONTROLCMD_TYPE_ID (adas_msgs_gen.h) */
> ```
> 而生成头文件里是：
> ```
> build/gen/ControlRaw.h:24:  #define CONTROLRAW_TYPE_ID   0xafeb3d23u
> build/gen/ControlCmd.h:24:  #define CONTROLCMD_TYPE_ID   0xed9c7088u
> ```
> **`0x2D95C6D2` 是旧版 IDL 的值**——旧 `ControlCmd` 只有 6 个字段 / 18 字节（无 `turn_signal`、无 `hazard`）。这个 stale 值冻结在 `build-algo/gen/adas_msgs_gen.h:618` 里（一个旧构建目录的残留），并扩散到了 5 个位置：
>
> | 位置 | 内容 |
> |---|---|
> | `safety_control_node.cpp:39` | `CONTROL_CMD_TYPE_ID = 0x2D95C6D2u` |
> | `manual_drive_node.c:61` | `#define CONTROLCMD_TYPE_ID 0x2D95C6D2u` |
> | `flowsim_node.cpp:87` | `#define CONTROL_CMD_TYPE_ID 0x2D95C6D2u` |
> | `data_recorder_node.c:502` | `discovery_advertise(..., 0x2D95C6D2u, ...)` |
> | `tools/flowboard/js/app.js:1305` | 仪表盘里写死 `type_id:"0x2d95c6d2"` |
>
> `0x871712d1` 更糟——`build-algo/gen/` 里根本没有 `ControlRaw`（它比那版 IDL 晚加），**这个值是纯臆造**。

---

## 3. 生成物长什么样

### 3.1 `build/gen/ControlCmd.h` 逐段

**常量**（`:20-27`）：

```c
21: /* Struct: ControlCmd (size=20B, type_id=0xed9c7088) */
22: /* Sig: ControlCmd:uint32_seq,float_throttle,float_brake,float_steering,Gear_gear,bool_e... */
24: #define CONTROLCMD_TYPE_ID         0xed9c7088u
25: #define CONTROLCMD_SCHEMA_VERSION  1
26: #define CONTROLCMD_SCHEMA_HASH     0x18b4cb38u
27: #define CONTROLCMD_TYPE_NAME        "ControlCmd"
```

> **旧稿写的 `LIDAR_FRAME_TYPE_NAME "sensor/LidarFrame"` / `LIDAR_FRAME_TYPE_ID 0x9A4F2C18` 两行都不存在。** 真实宏名是 `LIDARFRAME_*`（无下划线），没有 `TYPE_NAME` 字面量前缀，值是 `0xd712aa51`。

**结构体**（`:29-38`）——注意 `Gear` → `int8_t`，**没有任何 packing 指令**：

```c
29: typedef struct {
30:     uint32_t    seq;             /**< uint32 */
31:     float       throttle;        /**< float */
32:     float       brake;           /**< float */
33:     float       steering;        /**< float */
34:     int8_t      gear;            /**< Gear */
35:     bool        emergency_stop;  /**< bool */
36:     uint8_t     turn_signal;     /**< uint8 */
37:     bool        hazard;          /**< bool */
38: } ControlCmd;
```

**C++ traits**（`:40-49`）：

```c
43: template<> struct msg_traits<ControlCmd> {
44:     static constexpr uint32_t TYPE_ID = CONTROLCMD_TYPE_ID;
45:     static constexpr uint8_t  SCHEMA_VERSION = CONTROLCMD_SCHEMA_VERSION;
46:     static constexpr const char* TYPE_NAME = "ControlCmd";
47: }};
```

> 🚩 **这个特化是死代码，而且暴露了一个接口断裂。** `serializer.h:315-318` 的 `msg_cast<T>` 走的是 **`T::TYPE_ID`**（struct 的静态成员），而生成的 struct **没有这个成员**——`TYPE_ID` 在 `msg_traits` 里。**所以 C++ 的 `msg_cast<ControlCmd>(msg)` 连编译都过不了。**

**serialize**（`:51-68`）：

```c
52: static inline int ControlCmd_serialize(const ControlCmd* src,
53:         uint8_t* buf, size_t* out_size) {
54:     if (!src) return -1;
55:     size_t total = 20;
56:     if (out_size) *out_size = total;
57:     if (!buf) return 0;  /* size query only */
59:     if (buf) serializer_store_le(buf + 0, &src->seq, 4);
63:     if (buf) buf[16] = (int8_t)src->gear;
64:     if (buf) buf[17] = (uint8_t)src->emergency_stop;
```

**每个字段前都有冗余的 `if (buf)` 守卫**，而 `:57` 已经 `if (!buf) return 0;` 了。

**deserialize**（`:70-86`）：

```c
73:     if (!dst || !buf) return -1;
74:     if (size < 20) return -1;      /* ← 只检查下界 */
75:     memset(dst, 0, sizeof(*dst));
81:     dst->gear = (int8_t)((int8_t)buf[16]);   /* 双重转换，冗余 */
```

**两个小问题**：`:74` 只查下界，传 4096 字节缓冲区静默忽略尾部；`:81` 的双重转换来自生成器模板。

**endian_swap**（`:88-97`）：

```c
89: static inline void ControlCmd_endian_swap(void* data) {
90:     if (!data) return;
91:     uint8_t* p = (uint8_t*)data;
92:     serializer_swap32(p + 0);
```

> ⚠️ **这个函数用的是线格式偏移，在 padded 内存上是错的。** `ControlCmd` 的 `sizeof` 恰好也是 20（无 padding），看不出问题。但 `Behavior`（wire 22 / struct 32）就错位了：`build/gen/Behavior.h:89-90` 写 `serializer_swap32(p + 14)` 和 `p + 18`——**真实的 `offsetof(Behavior, target_speed)` 是 16 而不是 14**。
>
> 所以**生成的 `endian_swap` 只能作用于线格式缓冲区，不能作用于 `msg->data` 里的 padded 内存**。而 `serializer_normalize_endian`（`serializer.c:85`）传的正是 `msg->data`——**它对 12 个 mismatched 类型会损坏数据**。所幸它零调用者。

**FieldDesc 表**（`:99-110`）：

```c
100: static const FieldDesc ControlCmd_fields[] = {
101:     { "seq", FIELD_KIND_UINT, (uint16_t)offsetof(ControlCmd, seq), 4, 1 },
105:     { "gear", FIELD_KIND_ENUM, (uint16_t)offsetof(ControlCmd, gear), 1, 1 },
```

> ⚠️ **`offset` 用 `offsetof`（内存布局），`elem_size` 用线格式大小——两者混在一张表里。** 这正是 4.2 节那个 gate 存在的原因。
>
> 另外 `offset` 是 `uint16_t`，而 `StereoFrame` 的 `depth_data[4800]` 在 offset 8440——**再加字段就会溢出 65535**。

### 3.2 全部 20 个类型的实际数字

| struct | type_id | wire (B) | padded (B) |
|---|---|---:|---:|
| `LidarFrame` | `0xd712aa51` | 24 | 24 |
| `LidarPointCloud` | — | 40980 | **40984** |
| `GpsData` | — | 36 | **40** |
| `Pose2D` | — | 29 | **32** |
| `StereoFrame` | — | 44828 | 44828 |
| `Obstacle` | `0x322bd084` | 35 | **40** |
| `ObstacleList` | `0xf2485e48` | 4496 | **5144** |
| `Localization` | — | 57 | **60** |
| `ControlRaw` | `0xafeb3d23` | 59 | **60** |
| **`ControlCmd`** | **`0xed9c7088`** | 20 | 20 |
| `Behavior` | `0x6fe14940` | 22 | **32** |
| `Trajectory` | `0xcdffffab` | 2581 | **2592** |
| `PredictionSet` | — | 3068 | **3088** |

**加粗的 12 个 = wire ≠ padded**，与 4.2 节那个 gate 的 `KNOWN_MISMATCHES` 清单**完全吻合**。

> **注意 `Obstacle`：wire 35 字节，padded 40 字节。** `build/gen/Obstacle.h:62-72` 写死了 `buf[28]` `buf[29]` `buf[30]` `buf[34]`；而真实的 `offsetof(Obstacle, confidence)` = 32、`offsetof(Obstacle, obs_lane_match_hint)` = 36。**差 2 和差 6。**

### 3.3 `char mode[24]` 怎么编码

`build/gen/ControlRaw.h:71-73`：

```c
71:     for (size_t i = 0; i < 24; i++) {
72:     if (buf) buf[32 + i * 1] = (uint8_t)src->mode[i];
73:     }
```

**逐字节拷贝，不是 `memcpy`，且不做 NUL 终止保证。** 消费端必须自己 `strnlen`。

### 3.4 嵌套的 serialize 有两个缺陷

`build/gen/ObstacleList.h:58-62`：

```c
58:     for (size_t i = 0; i < 128; i++) {
60:         size_t sz = 0;
61:         Obstacle_serialize(&src->obstacles[i], buf ? buf + 16 + i * 35 : NULL, &sz);
62:     }
```

- **`:61` 返回值被丢弃**——某个 `Obstacle_serialize` 失败，外层仍返回 0；
- **`:60` 的 `size_t sz` 是写后不读的死变量**，只为满足 `&sz` 参数；
- 步长 `i * 35` 是**线格式下的 `sizeof(Obstacle)`**——序列化成 padded 内存就错位。

### 3.5 注册时机：显式调用，不是 constructor

```c
// src/flow_node_host.c:122  ——  唯一的生产入口
122: adas_msgs_register_all();
```

其他调用点全是显式的：`flowctl.c:192,587`、`flow_launcher.c:825`、`examples/ekf_chapter/ekf_chapter.c:540`、`tests/test_modules.c:95,114`。

> **`modules/adas_nodes/` 下 0 处。** 节点走 `flow_node_host.c` 所以被覆盖，但如果有人直接链接 `flowengine_core` 跑单个节点，**类型表是空的**。

`serializer_register_type`（`serializer.c:101-132`）：

```c
118:     if (g_type_count >= SERIALIZER_MAX_TYPE_ENTRIES) {
120:         fprintf(stderr, "[serializer] ERROR: type table full ...");
122:         return ERR_INVALID_PARAM;              /* ← 应为 ERR_TABLE_FULL */
129:     flow_registry_on_type_registered(entry);   /* 通知 FlowRegistry */
```

**表满时返回 `ERR_INVALID_PARAM(-1)` 而不是 `ERR_TABLE_FULL(-15)`**——而 `error_codes.h:40` 明明定义了后者，**且全仓无人使用它**。

---

## 4. 线格式：两个互不兼容的布局

### 4.1 规范 = 无条件小端

`serializer.c:66-74`：

```c
66: void serializer_store_le(uint8_t* dst, const void* src, size_t n) {
67:     memcpy(dst, src, n);
68:     if (serializer_is_big_endian()) swap_by_size(dst, n);  /* LE 主机：纯 memcpy */
69: }
```

`swap_by_size`（`:57-64`）**1 字节不交换**，`n` 非 2/4/8 不处理。

**`serializer_store_le` 在小端主机上退化为纯 `memcpy`。** 测试对此有显式断言（`tests/test_modules.c:262-265`）：

```c
263:  /* 线格式规范 = 小端：timestamp_us 打包在 offset 28，LSB 必在低地址 */
264:  ASSERT(buf[28] == 0x08, "wire not little-endian (LSB)");
265:  ASSERT(buf[35] == 0x01, "wire not little-endian (MSB)");
```

### 4.2 ⚠️ 「固定大小 struct」只对 8/20 个类型成立

**两个互不兼容的布局。** `ci/gates/msg_layout_check.py:12-15` 讲得最清楚：

```
12: Which one is authoritative depends on how a message is sent:
14:   * `T_serialize()` / `T_deserialize()`  → wire layout  (packed, declared size)
15:   * `msg_cast(T)` / `msg_init_typed(&v, sizeof(v))` → memory layout (padded)
```

| | 生成方式 | 有 padding？ |
|---|---|---|
| **wire** | `msg_codegen.py:341-347` 的 `_struct_size` = 字段大小**直接相加** | 无 |
| **memory** | 编译器自然对齐 | 有 |

**12/20 类型两者不等。**

> [!WARNING]
> **混用会静默错位**——字段落在错误偏移上，**不报任何错**。gate 的头注释（`:20-23`）记着一次真实事故：
>
> > Mixing them fails *silently*: fields land on the wrong offsets, no error is logged. (2026-09-21: `sensor_model` published `LidarPointCloud` as raw struct memory while `perception_node`/`slam_node` deserialized it as wire layout → `count` was read from the upper half of `timestamp_us` → **the point cloud was silently empty**.)
>
> 症状是「点云莫名其妙全空」，而日志里一切正常。

**旧稿完全没提这件事**，而它是本章最该讲的。

### 4.3 字节序标记是死字段

`serializer.c:30-36`，注释直接自认：

```c
30: uint8_t serializer_endian_marker(void) {
31:     /* wire 格式自 PR #72 起无条件小端（msg_codegen 的 serializer_store_le），
32:      * marker 若报主机序，会在假想 BE 主机上产生"wire=LE 但 marker=BE"的矛盾
33:      * （当前所有支持平台均 LE，marker 又是死字段，故纯理论）。统一恒报 LE，
34:      * 与序列化实现一致；未来接 normalize_endian 时 BE 主机据此正确交换。 */
35:     return ENDIAN_MARKER_LE;
36: }
```

**恒返回 LE。** `serializer_normalize_endian`（`:85-93`）**零调用者**，且如 3.1 节所述对 12 个类型会损坏数据。

> ⚠️ **测试与实现矛盾。** `tests/test_modules.c:289` 断言 `marker == (is_be ? BE : LE)`——**这个断言在大端主机上会失败**，因为实现恒返回 LE。**这是个定时炸弹式的测试假设。**

所以旧稿 L129-133 说的「读取端发现 marker 与本地架构相反时自动 bswap」**目前是未实现的**。

### 4.4 版本控制：三层，各自独立

**其一，`schema_version`（uint8）** —— 理论上由 JSON sidecar 自动 bump（`msg_codegen.py:237-254`）：

```python
240:      """Deterministic auto-bump:
241:        - If state[struct].hash doesn't match current layout_hash, bump version + 1.
242:        - If struct missing from state, version = 1 (initial)."""
```

> 🚩 **构建从不传 `--schema-state`**（`CMakeLists.txt:156-162`），`schema_state` 恒为 `{}`（`msg_codegen.py:856`），所有类型 `SCHEMA_VERSION` **恒为 1**。全仓 `**/schema_state*.json` glob 零命中。
>
> **所以旧稿 L143 说的「必须递增 `schema_version`」在当前构建下不可执行。**

**其二，`type_id`（uint32）** —— 隐式版本号（2.2 节）。**这是当前唯一真正起作用的版本机制。**

**其三，bag 文件版本 `BAG_VERSION 3`** —— `src/core/bag.c:33`，record 头格式见 `bag.c:7-11`。

> 🐛 **bag.c 的 writer/reader 不对称——真实 bug。** Writer（`bag.c:340-342`）的索引条目**不写 `schema_hash`**：
> ```c
> 340:  fwrite(&e->type_id,       sizeof(e->type_id),       1, w->fp);
> 341:  fwrite(&e->schema_version, sizeof(e->schema_version), 1, w->fp);
> 343:  uint32_t crc = 0;
> 344:  fwrite(&crc, sizeof(crc), 1, w->fp);
> ```
> 而 Reader（`bag.c:447-453`）在 `version >= 3` 分支**期望 `schema_hash`**：
> ```c
> 447:  if (version >= 3) {
> 448:      /* v3+: schema_hash(4B) 紧跟 type_id 之后 */
> 449:      entry_ok &= (fread(&e->schema_hash, sizeof(e->schema_hash), 1, fp) == 1);
> ```
> **后果：`BAG_VERSION == 3` 的 bag 文件，索引条目比 writer 写的少 4 字节。** 回放时第一个条目就会把 writer 写的 `schema_version`(1B) + `crc`(4B) 的前 4 字节当成 `schema_hash`，然后 `fread` 失败 → 索引全部丢失。**记录数据本身没问题**（record 头确实写了 `schema_hash`，`bag.c:266`），所以是「数据在、索引没了」。
>
> 顺带：**CRC 恒写 0**（`:343-344`），而 `bag.c:11` 的注释宣称有 `crc32(4B)`。**序列化层根本没有 CRC**——旧稿 L137-139 那节「同一份数据算出两个 CRC / memset 修复」描述的是一个**不存在的机制**。

---

## 5. 类型校验：`msg_cast` 与它的现实

### 5.1 真实的实现

`src/core/serializer.c:215-247`：

```c
215: const void* _msg_cast_impl(const Message* msg, uint32_t expected_type_id,
216:                             size_t expected_size, const char* type_name) {
217:     if (!msg) return NULL;
219:     /* 新格式：按 type_id 精确匹配 */
220:     if (msg->type_id != 0 && expected_type_id != 0) {
221:         if (msg->type_id == expected_type_id) {
222:             return msg->data;
223:         }
225:         const TypeRegistryEntry* expected = serializer_lookup_type(expected_type_id);
226:         const TypeRegistryEntry* actual   = serializer_lookup_type(msg->type_id);
227:         fprintf(stderr, "[serializer] WARNING: type mismatch on topic '%s': "
228:                 "expected '%s'(id=0x%08x) but got '%s'(id=0x%08x)\n", ...);
234:         return NULL;
235:     }
237:     /* 旧格式（type_id == 0）或关闭 type_id 检查：回退到 size 检查 */
238:     if (expected_size > 0 && msg->data_size != expected_size) {
239:         fprintf(stderr, "[serializer] WARNING: size mismatch on topic '%s': ...", ...);
243:         return NULL;
244:     }
246:     return msg->data;
247: }
```

**旧稿的四处偏差：**

| 旧稿 | 真实 |
|---|---|
| `if (msg->data_size < sizeof(T))` | `if (msg->data_size != expected_size)`（**语义相反**——拒绝「比期望大」的消息） |
| `LOG_ERROR` | `fprintf(stderr, ... "WARNING: ...")` |
| `TypeTraits<T>::type_id` | `T::TYPE_ID`（traits 类名是 `msg_traits`） |
| 只画了一条校验路径 | **实际有两条**：`type_id` 路径和 legacy `data_size` 路径 |

**C 版宏**（`serializer.h:213-214`）：

```c
213: #define msg_cast(msg, type_id, type_size) \
214:     ((const void*)_msg_cast_impl((msg), (type_id), (type_size), #type_id))
```

> **旧稿写的 `msg_cast_c` 和 `MSG_CAST(Type, msg)` 宏在全仓不存在。** 而且旧稿的示例 `msg_cast(msg, LIDAR_FRAME_TYPE_ID)` 少传了第 3 个参数，**照抄编译不过**。

**C++ 版**（`serializer.h:305-318`）：

```cpp
305: /* C++ template overrides the C macro version */
306: #undef msg_cast
314: template<typename T>
315: inline const T* msg_cast(const Message* msg) {
316:     return static_cast<const T*>(
317:         _msg_cast_impl(msg, T::TYPE_ID, sizeof(T), T::TYPE_NAME));
318: }
```

> 如 3.1 节所述，`T::TYPE_ID` 在生成的 struct 里**不存在**，所以这行模板**实例化即编译失败**。

### 5.2 🚩 校验在生产链路上基本没生效

这是本章最需要读者知道的现实：

| 项 | 状态 |
|---|---|
| `msg_cast` 宏 / 模板 | **零生产调用者**。grep 只命中 `serializer.h` 自身和文档 |
| `_msg_cast_impl` | 有 2 处生产调用：`perception_node.cpp:288`、`fusion_node.cpp:154` |
| 其余所有节点 | **裸调 `*_deserialize`，不做 `type_id` 校验** |

`modules/adas_nodes/flowsim_node.cpp:288-296` 是典型：

```cpp
288:     /* 二进制 ControlCmd 路径 */
290:         ControlCmd bin;
291:         if (ControlCmd_deserialize(&bin, (const uint8_t*)msg->data, msg->data_size) == 0) {
292:             g.ego_throttle.store(bin.throttle, std::memory_order_relaxed);
```

**没有 `msg->type_id == CONTROLCMD_TYPE_ID` 这一步。** `ControlCmd_deserialize` 只查 size（`ControlCmd.h:74` 的 `if (size < 20) return -1`）。

> **所以「把类型错误挡在编译期/运行期」这个卖点，机制是有的，但在实际数据通路上大部分节点并没有走那条路。**

同样地，`message_bus_publish_typed`（`message_bus.c:792`）**零生产调用**——所有节点都走 `transport_publish`，`type_name = NULL`，所以 `type_id` / `schema_hash` **恒为 0**（`message_bus.c:799-814`）。**这意味着绝大多数消息在总线上根本没有类型标识。**

### 5.3 另一条路径：`msg_init_typed`

生产代码里更常见的是先构造一个 `Message` 再发（`sensor_model_node.c:242-249`）：

```c
245:     Message msg;
246:     msg_init_typed(&msg, TOPIC_SENSOR_LIDAR_POINTS, "sensor_model",
247:                    LIDARPOINTCLOUD_TYPE_ID, LIDARPOINTCLOUD_SCHEMA_VERSION,
248:                    wire, wire_len);
249:     transport_publish(g.transport, TOPIC_SENSOR_LIDAR_POINTS, msg.data, msg.data_size);
```

`msg_init_typed`（`serializer.c:251-266`）有两个缺陷：

- **`:263` 的条件不满足时静默丢弃超大 payload**，而 `:259` 已经把 `data_size` 设成了设定值——**静默数据损坏。**
- **不设 `schema_hash`**（对比 `message_bus.c:957` 设了）。

而且 `:246-249` 是**双重无意义拷贝**——`msg_init_typed` 把 `wire` 拷进 `msg.data`，然后 `:249` 又把 `msg.data` 传给 `transport_publish` 二次拷贝。

---

## 6. CI gate：`msg_layout_check.py`

### 6.1 它防的是什么

[`ci/gates/msg_layout_check.py:2-27`](file:///home/caixuf/code/FlowEngine/ci/gates/msg_layout_check.py) 的头注释是全仓最精确的问题陈述之一：

```python
 2: """Guard against the "wire size vs C struct size" trap in msg_codegen output.
 6: `tools/msg_codegen.py` computes a struct's *wire* size as the plain sum of its
 7: field sizes (`_struct_size`), and emits serialize/deserialize functions that
 8: write fields back-to-back at exactly those offsets.  The generated C struct,
 9: however, has **no packing directive**, so the C compiler inserts natural
10: alignment padding — and then `sizeof(T) != wire_size(T)`.
20: Mixing them fails *silently*: fields land on the wrong
21: offsets, no error is logged.  (2026-09-21: `sensor_model` published
22: LidarPointCloud as raw struct memory while `perception_node`/`slam_node`
23: deserialized it as wire layout → `count` was read from the upper half of
25: This gate does not fix that.  It pins the current state so a *new* mismatch
26: (or a changed one) is caught immediately, and so the known list stays honest:
27: a type that stops mismatching must be removed from KNOWN_MISMATCHES.
```

### 6.2 三个方向的校验

`:139-160`：

1. **新 mismatch 报错** —— 必须显式登记 + 写原因；
2. **已知 mismatch 数字变动报错** —— 加字段后必须更新清单；
3. **清单里不再 mismatch 的类型报错** —— 防止清单腐化。

**它复用生成器自己的计算逻辑**，保证永不漂移（`:119-121`）：

```python
119:  # Reuse the generator's own wire-size computation so this gate can never
120:  # drift from what the generated serializer actually emits.
121:  gen = CodeGenerator(parser)
```

CI 接线在 `.github/workflows/ci.yml:89-94`，一个独立的 `msg-layout-gate` job。

> ⚠️ **这个 gate 不在 `needs:` 链上，且直接用 `python3` 调（不经 CMake）**——**它校验的是当前 `msg/adas_msgs.msg`，而非 `build/gen/` 里已提交的生成物**。两者若不同步（因为 CMake 只声明 1 个 OUTPUT），gate 仍会通过。

---

## 7. 测试

### 7.1 C 测试

`tests/test_modules.c`：

| 测试 | 行 | 覆盖 |
|---|---|---|
| `test_fnv1a_hash` | `:51-67` | 已知向量 **`fnv1a("hello") == 0x4f9f2cab`**（`:60`）、确定性 |
| `test_type_registry` | `:69-91` | register / lookup 命中 / 未命中 NULL |
| `test_schema_metadata` | `:94-127` | `field_count`、字段名、`FieldKind` |
| `test_schema_compat` | `:130-163` | 4 个 `serializer_check_compat` 分支 |
| `test_publish_typed_schema_metadata` | `:178-208` | 端到端：`type_id` / `schema_hash` / `schema_version` 落到 `Message` |
| `test_gen_serialize_roundtrip` | `:237-268` | **`GpsData` 真实往返 + 线格式小端断言** |
| `test_msg_cast` | `:270-283` | ⚠️ **只测 legacy 分支**（两个用例都传 `type_id=0`） |
| `test_endian_detection` | `:285-294` | ⚠️ marker 一致性（**大端主机上会失败**） |

> **两个重要空白：**
> 1. `test_msg_cast` 只测 legacy 分支——**`type_id` 匹配/不匹配路径（也就是本章的卖点）零测试覆盖**。
> 2. **没有任何测试校验生成的 type_id 值、字节大小、`sizeof(T) == wire_size`**，或 gate 里 `KNOWN_MISMATCHES` 的数字。

### 7.2 Python 测试

`tests/test_msg_codegen.py` **全文 30 行，一个测试方法**（`:7-26`）：测「2048 个元素的数组不展开成 2048 行代码」。

**没测 FNV-1a、没测 type_id 稳定性、没测 schema_hash、没测 enum/nested 解析、没测解析错误。**

### 7.3 一个自指的不一致

`tests/test_modules.c:210-234` 有一个注释写 `/* Simulate serialize */`、用 `memcpy` 模拟、**没碰任何生成的代码**的函数；而 `:237` 那个 `test_gen_serialize_roundtrip` 是真的。**两个函数同名，一个是假的。**

---

## 8. 死代码清单

| 符号 | 位置 | 状态 |
|---|---|---|
| `ser::fnv1a_const`（2 个重载） | `serializer.h:295,300` | **零调用者**；且 `__builtin_strlen` 是 GCC 专有 |
| `fnv1a_byte` / `fnv1a_update` | `serializer.h:54,59` | **零调用者** |
| `msg_traits<T>` 的 20 个特化 | `build/gen/<N>.h:43-47` | **从不读取**；且 C++ `msg_cast<T>` 因此编译不过 |
| `msg_cast`（C 宏 + C++ 模板） | `serializer.h:213,315` | **零生产调用者** |
| `msg_cast_c` / `MSG_CAST` | — | **不存在**（旧稿虚构） |
| `serializer_normalize_endian` | `serializer.c:85` | 零调用者；**且对 12 个类型会损坏数据** |
| `serializer_ensure_endian` | `serializer.c:76` | 只被上面那个死函数调用；**`size` 参数完全未用** |
| `endian_marker` 的 BE 分支 | `serializer.c:35` | 注释自认「死字段」 |
| `msg_schema_register` / `_check` | `msg_schema.c:20,56` | 仅 `src/bag_demo.c` 一个 demo |
| `flow_registry_register_type` | `flow_registry.c:168` | **零调用者**（第 03 章已记录） |
| `message_bus_publish_typed` | `message_bus.c:792` | **零生产调用者** |
| `proto_support.c` 全文 108 行 | — | `#ifdef FLOWENGINE_USE_PROTOBUF` **永假**（protobuf-c 未找到） |
| `error_codes.h:35-40` 的 6 个错误码 | — | `ERR_TYPE_MISMATCH` … `ERR_TABLE_FULL` **零使用** |
| `_compute_schema_version` 的 bump 路径 | `msg_codegen.py:249-253` | 构建不传 `--schema-state`，恒不触发 |

### 8.1 头文件自身的示例编译不过

`include/serializer.h:14-21` 的「典型用法」注释：

```c
14:  * 典型用法（C）：
15:  *   MSG_REGISTER_TYPE("sensor/lidar", LIDAR_FRAME_TYPE_ID, sizeof(LidarFrame),
16:  *                     lidar_frame_serialize, lidar_frame_deserialize);
17:  *   const LidarFrame* f = msg_cast(msg, LIDAR_FRAME_TYPE_ID);
```

**三处错误**：`MSG_REGISTER_TYPE` 真实是 2 参数版（`msg_schema.h:54-55`）、`lidar_frame_serialize` 符号不存在（真实是 `LidarFrame_serialize`）、`msg_cast` 少传第 3 个参数。

**写书时不要引用这段注释。**

### 8.2 `proto_support.c` 暴露了一个语义裂缝

`proto_support.c:72-93`（整个文件被 `#ifdef` 包住，从不编译）：

```c
75:     uint32_t type_id = fnv1a_hash((const uint8_t*)descriptor->name,
76:                                    strlen(descriptor->name));
```

**这里用的是「纯类型名」算 `type_id`，而 codegen 用的是「字段签名串」。** 同一套 IDL 体系下并存两套 `type_id` 语义——概率极低但**这是真实的设计裂缝**。

---

## 9. 源码与资源对照

| 模块 | 源码文件 | 核心符号 | 职责与要点 |
|---|---|---|---|
| **IDL** | [`msg/adas_msgs.msg`](file:///home/caixuf/code/FlowEngine/msg/adas_msgs.msg) | 20 struct + 3 enum | 294 行。**纯手写行解析器**，前向引用会漏；默认值被丢弃 |
| **生成器** | [`tools/msg_codegen.py`](file:///home/caixuf/code/FlowEngine/tools/msg_codegen.py) | `IDLParser`（`:113`）<br>`_sig_string`（`:283`）<br>`_type_id`（`:299`）<br>`_schema_hash`（`:310`）<br>`generate_split`（`:402`） | `--split` 模式产出 23 个文件。**`type_id` = 字段签名串的 FNV-1a**（递归展开嵌套）。`schema_hash` 不递归 |
| **构建接线** | [`CMakeLists.txt:134-167`](file:///home/caixuf/code/FlowEngine/CMakeLists.txt) | `msg_codegen` 目标<br>`--split --output-dir` | **只声明 1 个 OUTPUT**，其余 22 个对增量构建不可见。**不传 `--schema-state`** ⇒ 版本恒为 1 |
| **序列化器** | [`include/serializer.h`](file:///home/caixuf/code/FlowEngine/include/serializer.h)<br>[`src/core/serializer.c`](file:///home/caixuf/code/FlowEngine/src/core/serializer.c) | `TypeRegistryEntry`（`h:114-125`）<br>`fnv1a_hash`（`c:15`）<br>`serializer_register_type`（`c:101`）<br>`_msg_cast_impl`（`c:215`）<br>`serializer_store_le`（`c:66`） | 128 条固定表。表满返回 `ERR_INVALID_PARAM` 而非 `ERR_TABLE_FULL`。**`msg_cast` 零生产调用** |
| **生成物示例** | [`build/gen/ControlCmd.h`](file:///home/caixuf/code/FlowEngine/build/gen/ControlCmd.h) | `CONTROLCMD_TYPE_ID`（`0xed9c7088`）<br>`ControlCmd_serialize/_deserialize`<br>`ControlCmd_fields[]`<br>`ControlCmd_register_type` | 20 B，wire == padded。`endian_swap` 用 wire 偏移，在 padded 内存上错 |
| **手写路径** | [`src/core/proto_support.c`](file:///home/caixuf/code/FlowEngine/src/core/proto_support.c)<br>[`src/core/msg_schema.c`](file:///home/caixuf/code/FlowEngine/src/core/msg_schema.c) | `proto_register_type`（`proto_support.c:72`）<br>`msg_schema_register` | **前者整文件死代码**（`FLOWENGINE_USE_PROTOBUF` 永假），且**用类型名算 type_id**——与 codegen 语义不同。后者只有 demo 用 |
| **CI gate** | [`ci/gates/msg_layout_check.py`](file:///home/caixuf/code/FlowEngine/ci/gates/msg_layout_check.py) | `KNOWN_MISMATCHES`（`:60-74`）<br>三个方向的校验（`:139-160`） | 复用生成器的计算逻辑保证不漂移。**12/20 类型 wire ≠ padded**。**不在 `needs:` 链上，校验 IDL 而非生成物** |
| **测试** | [`tests/test_modules.c`](file:///home/caixuf/code/FlowEngine/tests/test_modules.c)<br>[`tests/test_msg_codegen.py`](file:///home/caixuf/code/FlowEngine/tests/test_msg_codegen.py) | `test_fnv1a_hash`（`:51`）<br>`test_gen_serialize_roundtrip`（`:237`）<br>`ctest modules_tests` | `test_msg_cast` **只测 legacy 分支**；**无 type_id 路径测试**；Python 侧只有 1 个测试方法 |

---

## 10. 思考题

1. **`type_id` 该不该把字段签名也算进去？**：
   第 2.2 节说 `type_id` 是**字段签名串**的 FNV-1a，所以**改任何字段都会改 `type_id`**，老订阅者被直接拒收。
   (a) 先算一下代价：往 `ControlCmd` 尾部加一个 `uint8 new_field`，会连带影响多少个类型的 `type_id`？（提示：考虑 `ObstacleList` 嵌 `Obstacle` 的递归展开。）
   (b) 对比另一种设计——**只哈希类型名**（`proto_support.c:75` 的做法）。那种设计下「加字段不改 `type_id`」，但也**失去了「布局变了就拒收」的保护**。考虑第 4.2 节那个「wire/padded 混用静默错位」的事故——如果 `type_id` 只看类型名，`Obstacle` 从 35 字节变成 40 字节这件事**根本不会被发现**。
   (c) 提出一个折中：把 `type_id` 拆成两个字段——`type_name_hash`（只看名字，用于路由和诊断）和 `layout_hash`（看字段，用于拒收）。注意 `schema_hash` 已经存在了，它和这个提议的 `layout_hash` 有什么区别？（提示：看 2.3 节那张表。）

2. **让类型校验真正生效**：
   第 5.2 节指出：`msg_cast` 零生产调用，绝大多数节点裸调 `*_deserialize`；而且 `message_bus_publish_typed` 零生产调用，**意味着大多数消息在总线上 `type_id` 恒为 0**。
   (a) 假设要把「类型校验」从「有个机制但没人用」变成「默认生效」。给出完整改动清单：需要改哪些节点、补哪些测试、registry 需要什么前置条件？（提示：第 3.5 节说 `modules/` 下 0 处 `register_type`，而 `flow_node_host.c:122` 是唯一生产入口——这个位置是现成的杠杆点。）
   (b) 有一个更省事的方案：**在 `*_deserialize` 生成的函数里内联 type_id 检查**。生成器已经在发 `ControlCmd_deserialize(msg, buf, size)`，能不能改成 `ControlCmd_deserialize_typed(msg, buf, size)` 并在函数内部检查 `msg->type_id`？评估：(i) 这会让每个调用点都变成「传 `Message*` 而不是 `buf+size`」，对现有 20 处调用点的改动成本；(ii) 对 `msg->data` 直接传参的场景（如第 5.3 节的 `msg_init_typed` 路径）怎么办？
   (c) 最根本的问题：**在一个 16 节点的流水线里，跨模块的「类型契约」应该由谁保证？** 现在的答案是「IDL 文件 + codegen」，但第 1.3 节说 IDL 解析器有前向引用的坑、第 2.4 节说手写 type_id 可以和生成值不一致。请设计一个能捕获「手写 type_id 与生成值不符」的门禁（参考第 2.4 节那个 `0x2D95C6D2` 扩散到 5 处的事故）。

3. **修掉 wire/padded 的双重布局**：
   第 4.2 节说 12/20 类型 `sizeof(T) != wire_size(T)`，混用会静默错位（2026-09-21 有真实事故）。`ci/gates/msg_layout_check.py` 只是「钉住现状」，不修问题。
   (a) 三种修法：(i) 给生成的 struct 加 `#pragma pack(1)` / `__attribute__((packed))`，让 `sizeof == wire_size`；(ii) 统一只走 `*_serialize`/`*_deserialize`，删掉 `msg_init_typed(&v, sizeof(v))` 这种内存布局路径；(iii) 在 IDL 里显式加 `packed` 关键字，让某些类型可以自由选择。评估每个对**性能**（packed 会导致非对齐访问）、**跨平台**（MSVC 的 `#pragma pack` 语法不同）、和**迁移成本**（12 个类型的所有调用点）的影响。
   (b) 假设选了 (i) `#pragma pack`。请检查第 3.1 节提到的 `ControlCmd_endian_swap`——pack 之后 `sizeof == wire_size`，那个函数是不是就变成**正确的**了？再检查 `FieldDesc` 表里的 `offsetof`——packed 之后 `offsetof` 和 `elem_size` 是不是终于一致了？这两处是 (i) 顺带修好的，还是需要额外改？
   (c) 迁移风险：`Obstacle` 从 padded 40 变成 packed 35 会**改变线格式**——已经录在 bag 里的旧数据会解不出来。请设计一个迁移方案：需要 bump `schema_version` 吗？`BAG_VERSION` 要不要升？旧的 bag 文件怎么标记成「需要用旧的解法」？

4. **`schema_version` 自动化没接线**：
   第 4.4 节说 `msg_codegen.py` 实现了基于 JSON sidecar 的自动 bump（`:237-254`），但**构建从不传 `--schema-state`**，所以全部 20 个类型的 `SCHEMA_VERSION` 恒为 1。
   (a) 接线它需要改什么？（提示：`CMakeLists.txt:156-162` 的 `add_custom_command` 加一个参数，然后 sidecar 文件要提交进 git 还是放 build 目录？）
   (b) 假设接线了。`_compute_schema_version` 的逻辑是「hash 变了就 +1」——**这意味着任何一次字段改动都会让版本号 +1**。但第 2.2 节说 `type_id` 也变了（因为它也哈希字段）。那么对一个改过字段的消息，接收端会同时看到 `type_id` 变了**和** `schema_version` 变了——**这不是冗余吗？** 请论证：`schema_version` 存在的意义是什么？如果 `type_id` 已经能拒收一切布局变化，是不是根本不需要 `schema_version`？
   (c) 反过来问：**什么时候 `type_id` 相同但 `schema_version` 应该不同？** 找出一种场景——如果找不出来，说明 `schema_version` 这个字段是纯冗余，应该考虑从 IDL 和 `Message` 里都删掉。删掉的话会影响哪些地方？（提示：`msg/adas_msgs.msg`、`message_bus.h:68`、`ControlCmd.h:25`、bag 格式的 v3 分支……）
