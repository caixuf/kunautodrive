# 平台抽象层

> 本文件文档化跨平台兼容策略，覆盖 `platform_pal.h`（核心基础层）和
> `platform_compat.h`（编译期兼容宏）。Linux 为主力平台，macOS/Windows
> 通过兼容层实现编译通过与功能降级。

---

## 1. 架构概览

```
┌───────────────────────────────────────────────────────┐
│ platform_compat.h（CMake force-include，仅 APPLE/WIN） │
│   编译期宏替换：线程命名、robust mutex、condvar 时钟、  │
│   CPU pause、accept4、线程栈大小、Win32 POSIX 平替      │
│   作用：调用点零改动，Linux 完全透明                    │
└───────────────────────────────────────────────────────┘
                          ↓
┌───────────────────────────────────────────────────────┐
│ platform_pal.h（显式 #include，PAL 接口）              │
│   运行时抽象：时钟、线程命名、共享内存 IPC、robust mutex │
│   作用：收口核心路径，防止平台分支散落                  │
└───────────────────────────────────────────────────────┘
                          ↓
┌───────────────────────────────────────────────────────┐
│ clock_service / message_bus / ipc_channel / scheduler  │
│   消费 PAL，不直接调用平台 API                         │
└───────────────────────────────────────────────────────┘
```

---

## 2. `platform_compat.h` — 编译期兼容宏

> 由 CMake 在 `APPLE`/`_WIN32` 平台通过 `-include` 全局强制包含，
> 绝大多数源文件无需手动 `#include`。所有内容包在平台守卫内，
> Linux 上完全透明。

### 2.1 macOS 兼容项（`#if defined(__APPLE__)`）

| # | 兼容项 | Linux 原形 | macOS 问题 | 平替方案 | 影响范围 |
|---|--------|-----------|-----------|---------|---------|
| 1 | 线程命名 | `pthread_setname_np(thread, name)`（2 参） | 只有 1 参版本，只能命名当前线程 | 内联包装函数 + 宏重映射（全项目 35 处调用均为 `pthread_self()`，语义一致） | 零影响 |
| 2 | Robust mutex | `pthread_mutexattr_setrobust` / `pthread_mutex_consistent` | macOS pthreads 不支持 | 宏定义为 no-op（返回 0） | 多进程 IPC 崩溃自愈降级；单进程 demo 不触发 |
| 3 | condvar 时钟 | `pthread_condattr_setclock(CLOCK_MONOTONIC)` | 无此 API | 宏定义为 no-op；`ipc_channel.c` 在 `__APPLE__` 下已选用 `CLOCK_REALTIME` | timedwait 语义正确 |
| 4 | CPU pause | `__builtin_ia32_pause()`（x86 专属） | arm64 无此内建 | arm64 定义为 `yield` 指令；x86_64 Mac 保留原生 | FlowCoro 自旋等待 |
| 5 | accept4 | `accept4(fd, addr, len, flags)` | macOS 无 accept4 | `accept()` + `fcntl` 设置 `O_NONBLOCK`/`FD_CLOEXEC` | FlowCoro 网络 |
| 6 | 线程栈大小 | Linux 默认 8MB | macOS 默认 512KB，大数组节点（DBSCAN）栈溢出 | `pthread_create` 包装抬到 8MB 下限 | 调用点零改动 |

### 2.2 Windows 兼容项（`#if defined(_WIN32)`）

| # | 兼容项 | 平替方案 |
|---|--------|---------|
| 1 | `clock_gettime` | `QueryPerformanceCounter`（MONOTONIC）/ `GetSystemTimePreciseAsFileTime`（REALTIME） |
| 2 | POSIX 函数 | `getpid→_getpid`、`mkdir→_mkdir`、`access→_access`、`strdup→_strdup` 等 |
| 3 | `localtime_r` | `localtime_s`（Win32 反转参数顺序） |
| 4 | `setenv` | `_putenv_s` |
| 5 | `nanosleep`/`usleep`/`sleep` | `Sleep()`（毫秒精度） |
| 6 | `posix_memalign` | `_aligned_malloc` |
| 7 | `readlink` | `GetModuleFileNameA` |
| 8 | 编译器内建 | `__builtin_expect`→直通、`__builtin_prefetch`→空、`__builtin_ia32_pause`→`YieldProcessor` |
| 9 | 线程栈大小 | 同 macOS，抬到 8MB |

---

## 3. `platform_pal.h` — 运行时 PAL 接口

> 显式 `#include` 的平台抽象层，收口核心路径的平台调用。

| 原语 | Linux | macOS | QNX |
|------|-------|-------|-----|
| 实时/单调时钟 | 支持 | [OK] | 编译守卫 |
| 当前线程命名 | 支持 | 支持（compat shim） | 不支持 |
| 共享内存 IPC（shm_open + process-shared pthread sync） | 支持 | 支持（compat fallback） | 不支持 |
| Robust mutex 恢复 | 支持 | 不支持（no-op 降级） | 不支持 |
| CPU 亲和性 | 支持 | 不支持 | 不支持 |

**QNX 行为**：当 `FLOWENGINE_TARGET_QNX` 或 `__QNXNTO__` 选中时，
`ipc_channel_open` 在分配共享内存前以 `ENOTSUP` 失败。这是有意设计——
QNX 目标需先验证进程共享同步、命名空间、权限和生命周期行为。

### PAL 收口的调用点

| 文件 | 使用的 PAL 原语 |
|------|----------------|
| `clock_service.c` | 实时和单调时钟 |
| `message_bus.c` | 实时 timed wait 和微秒级退避 |
| `ipc_channel.c` | 共享内存对象生命周期、process-shared pthread 同步、robust mutex 恢复、condvar 时钟选择 |
| `scheduler.c` / `task_interface.c` | 当前线程命名、调度器退避、Linux CPU 亲和性扩展 |

**不在 PAL 范围内**：`crash_handler.c` 的 Linux `/proc` 和 `SYS_gettid` 诊断、
FlowCoro 网络/事件循环内部、硬件节点、QNX 完整工具链集成。

---

## 4. 构建配置

### CMake 平台分支

```cmake
# CMakeLists.txt 中的关键分支
if(APPLE)
  target_compile_definitions(flow_engine PRIVATE FLOWENGINE_HAVE_CLOCK_GETTIME=0)
  # macOS: 不构建 benchmark（sem_init 已废弃）
  # macOS: 不启用 robust mutex
  # macOS: 强制 include platform_compat.h
  target_compile_options(flow PRIVATE -include ${CMAKE_SOURCE_DIR}/include/platform_compat.h)
endif()

if(WIN32)
  # Windows: MinGW 交叉编译，PE32+ 二进制
  # pipeline_windows.json 配置
endif()
```

### 平台特定配置

| 配置文件 | 平台 | 用途 |
|---------|------|------|
| `config/pipeline.json` | Linux（默认） | 仿真 + 仪表盘 |
| `config/pipeline_car.json` | Linux（真车） | RC 小车硬件 |
| `config/pipeline_windows.json` | Windows（交叉编译） | Win32 PE 二进制 |
| `config/pipeline_manual.json` | Linux | 手动驾驶 |

### macOS 弱化项（不影响默认路径）

| 弱化项 | 原因 | 影响 |
|--------|------|------|
| Robust mutex → no-op | macOS pthreads 不支持 | 多进程 IPC 崩溃自愈不可用；单进程 demo 不触发 |
| Benchmark 不构建 | `sem_init` 在 macOS 已废弃 | 性能基准不可用 |
| CAN/I2C dry-run | 无 Linux 外设 | 硬件节点走 dry_run 模式 |
| 无线程 CPU 亲和性 | macOS 无 `pthread_setaffinity_np` | 调度器不绑核 |

---

## 5. 已知限制与后续方向

1. **QNX 支持**：PAL 层已预留编译守卫，但 IPC 和调度器的 QNX 实现尚未启用。
   需验证进程共享同步、命名空间和生命周期行为后方可打开。

2. **macOS CI**：代码兼容层已就绪，但 `.github/workflows/ci.yml` 未包含
   macOS runner。建议添加 `macos-latest` 到 CI 矩阵以自动验证兼容性。

3. **Windows 完整支持**：MinGW 交叉编译已通过 CI，但 `flow_launcher` 和
   节点插件的 Windows 运行时尚未验证（`dlopen` → `LoadLibrary` 等待适配）。
