# Platform abstraction boundary

`include/platform_pal.h` is a deliberately narrow PAL for core timing,
thread-startup, and shared-memory IPC primitives. It is not a QNX runtime port.

| Primitive | Linux | macOS | QNX foundation |
| --- | --- | --- | --- |
| Realtime / monotonic clocks | supported | supported | compile-guarded |
| Current-thread naming | supported | existing compat shim | unavailable |
| Shared-memory IPC (`shm_open` + process-shared pthread sync) | supported | existing compat fallback | unavailable |
| Robust mutex recovery | supported | unavailable | unavailable |
| CPU affinity | supported | unavailable | unavailable |

When `FLOWENGINE_TARGET_QNX` or `__QNXNTO__` is selected, `ipc_channel_open`
fails with `ENOTSUP` before allocating shared memory. This is intentional: a
QNX target must validate its process-shared synchronization, namespace,
permissions, and lifecycle behavior in a later increment before IPC is
enabled.

The bounded audit moved these core call sites behind the PAL:

- `clock_service.c`: realtime and monotonic clocks.
- `message_bus.c`: realtime timed waits and microsecond backoff.
- `ipc_channel.c`: shared-memory object lifecycle, process-shared pthread
  synchronization, robust-mutex recovery, and condvar clock selection.
- `scheduler.c` and `task_interface.c`: current-thread naming, scheduler
  backoff, and Linux CPU-affinity extension.

Not in this increment: `crash_handler.c`'s Linux `/proc` and `SYS_gettid`
diagnostics, flowcoro networking/event-loop internals, hardware nodes, and
full QNX toolchain/runtime integration. They are outside the message-bus,
IPC, scheduler, and clock foundation and remain unsupported on QNX.
