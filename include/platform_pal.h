#ifndef FLOWENGINE_PLATFORM_PAL_H
#define FLOWENGINE_PLATFORM_PAL_H

/*
 * Narrow POSIX platform-abstraction layer for FlowEngine core.
 *
 * Scope: clock_service, message_bus, ipc_channel, and scheduler/task startup.
 * This is deliberately not a general QNX porting layer.  QNX is compile-guarded
 * so its supported subset is explicit, but no QNX runtime configuration is
 * supported or claimed by this header.
 *
 * Capability boundaries:
 *   - Linux keeps the existing POSIX implementation unchanged: monotonic and
 *     realtime clocks, current-thread names, process-shared SHM IPC with robust
 *     mutex recovery, and CPU affinity are available.
 *   - macOS keeps its established compatibility fallbacks from
 *     platform_compat.h: SHM IPC is retained, robust recovery and monotonic
 *     condvar clocks are not required.
 *   - QNX only has compile-checked clock and scheduler foundations.  Shared
 *     memory IPC, robust recovery, thread naming, and affinity intentionally
 *     report unavailable until a target-runtime validation increment enables
 *     them.
 *   - Native Windows uses its separate IPC implementation in ipc_channel.c.
 *
 * Callers must test capabilities instead of assuming a Linux extension.  The
 * wrappers are header-only to keep Linux linkage and call order unchanged.
 */

#include "platform_compat.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(FLOW_PAL_COMPILE_TEST_QNX)
#define FLOW_PAL_PLATFORM_LINUX 1
#else
#define FLOW_PAL_PLATFORM_LINUX 0
#endif

#if defined(__APPLE__) && !defined(FLOW_PAL_COMPILE_TEST_QNX)
#define FLOW_PAL_PLATFORM_APPLE 1
#else
#define FLOW_PAL_PLATFORM_APPLE 0
#endif

#if defined(__QNXNTO__) || defined(FLOWENGINE_TARGET_QNX) || \
    defined(FLOW_PAL_COMPILE_TEST_QNX)
#define FLOW_PAL_PLATFORM_QNX 1
#else
#define FLOW_PAL_PLATFORM_QNX 0
#endif

#if defined(_WIN32) && !defined(FLOW_PAL_COMPILE_TEST_QNX)
#define FLOW_PAL_PLATFORM_WINDOWS 1
#else
#define FLOW_PAL_PLATFORM_WINDOWS 0
#endif

/*
 * Keep these compile-time constants usable in #if guards as well as in tests.
 * QNX values intentionally describe this foundation's validated boundary, not
 * the full set of APIs an individual QNX release may expose.
 */
#define FLOW_PAL_HAS_MONOTONIC_CLOCK 1
#define FLOW_PAL_HAS_THREAD_NAMING \
    (FLOW_PAL_PLATFORM_LINUX || FLOW_PAL_PLATFORM_APPLE || FLOW_PAL_PLATFORM_WINDOWS)
#define FLOW_PAL_HAS_SHARED_MEMORY_IPC \
    (FLOW_PAL_PLATFORM_LINUX || FLOW_PAL_PLATFORM_APPLE)
#define FLOW_PAL_HAS_ROBUST_MUTEX FLOW_PAL_PLATFORM_LINUX
#define FLOW_PAL_HAS_THREAD_AFFINITY FLOW_PAL_PLATFORM_LINUX
#define FLOW_PAL_HAS_MONOTONIC_COND_CLOCK FLOW_PAL_PLATFORM_LINUX

typedef enum FlowPalCapability {
    FLOW_PAL_CAP_MONOTONIC_CLOCK,
    FLOW_PAL_CAP_THREAD_NAMING,
    FLOW_PAL_CAP_SHARED_MEMORY_IPC,
    FLOW_PAL_CAP_ROBUST_MUTEX,
    FLOW_PAL_CAP_THREAD_AFFINITY,
    FLOW_PAL_CAP_MONOTONIC_COND_CLOCK,
} FlowPalCapability;

static inline bool flow_pal_has_capability(FlowPalCapability capability) {
    switch (capability) {
        case FLOW_PAL_CAP_MONOTONIC_CLOCK:
            return FLOW_PAL_HAS_MONOTONIC_CLOCK != 0;
        case FLOW_PAL_CAP_THREAD_NAMING:
            return FLOW_PAL_HAS_THREAD_NAMING != 0;
        case FLOW_PAL_CAP_SHARED_MEMORY_IPC:
            return FLOW_PAL_HAS_SHARED_MEMORY_IPC != 0;
        case FLOW_PAL_CAP_ROBUST_MUTEX:
            return FLOW_PAL_HAS_ROBUST_MUTEX != 0;
        case FLOW_PAL_CAP_THREAD_AFFINITY:
            return FLOW_PAL_HAS_THREAD_AFFINITY != 0;
        case FLOW_PAL_CAP_MONOTONIC_COND_CLOCK:
            return FLOW_PAL_HAS_MONOTONIC_COND_CLOCK != 0;
        default:
            return false;
    }
}

static inline int flow_pal_clock_gettime_realtime(struct timespec* out) {
    return clock_gettime(CLOCK_REALTIME, out);
}

static inline int flow_pal_clock_gettime_monotonic(struct timespec* out) {
    return clock_gettime(CLOCK_MONOTONIC, out);
}

static inline int flow_pal_thread_set_current_name(const char* name) {
    if (!name) return EINVAL;
#if FLOW_PAL_PLATFORM_QNX
    (void)name;
    return ENOTSUP;
#elif FLOW_PAL_PLATFORM_APPLE
    /* platform_compat.h maps this established two-argument call on macOS. */
    return pthread_setname_np(pthread_self(), name);
#else
    return pthread_setname_np(pthread_self(), name);
#endif
}

static inline int flow_pal_sleep_us(unsigned int usec) {
    return usleep(usec);
}

#if !defined(_WIN32)
static inline clockid_t flow_pal_ipc_cond_clock(void) {
#if FLOW_PAL_HAS_MONOTONIC_COND_CLOCK
    return CLOCK_MONOTONIC;
#else
    return CLOCK_REALTIME;
#endif
}

static inline int flow_pal_ipc_sync_init(pthread_mutex_t* mutex,
                                         pthread_cond_t* cond) {
    if (!mutex || !cond) return EINVAL;
#if !FLOW_PAL_HAS_SHARED_MEMORY_IPC
    return ENOTSUP;
#else
    pthread_mutexattr_t mattr;
    int rc = pthread_mutexattr_init(&mattr);
    if (rc != 0) return rc;
    rc = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
#if FLOW_PAL_HAS_ROBUST_MUTEX
    if (rc == 0) rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
    if (rc == 0) rc = pthread_mutex_init(mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);
    if (rc != 0) return rc;

    pthread_condattr_t cattr;
    rc = pthread_condattr_init(&cattr);
    if (rc != 0) {
        pthread_mutex_destroy(mutex);
        return rc;
    }
    rc = pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    if (rc == 0)
        rc = pthread_condattr_setclock(&cattr, flow_pal_ipc_cond_clock());
    if (rc == 0) rc = pthread_cond_init(cond, &cattr);
    pthread_condattr_destroy(&cattr);
    if (rc != 0) pthread_mutex_destroy(mutex);
    return rc;
#endif
}

static inline int flow_pal_ipc_mutex_lock(pthread_mutex_t* mutex) {
    int rc = pthread_mutex_lock(mutex);
#if FLOW_PAL_HAS_ROBUST_MUTEX
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(mutex);
        return 0;
    }
#endif
    return rc;
}

static inline int flow_pal_shared_memory_open(const char* name, int flags,
                                              mode_t mode) {
    return shm_open(name, flags, mode);
}

static inline int flow_pal_shared_memory_unlink(const char* name) {
    return shm_unlink(name);
}

static inline int flow_pal_shared_memory_resize(int fd, off_t size) {
    return ftruncate(fd, size);
}

static inline void* flow_pal_shared_memory_map(int fd, size_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

static inline int flow_pal_shared_memory_unmap(void* address, size_t size) {
    return munmap(address, size);
}
#endif /* !_WIN32 */

#if FLOW_PAL_HAS_THREAD_AFFINITY
#include <sched.h>
static inline int flow_pal_thread_attr_set_affinity(pthread_attr_t* attr,
                                                    uint64_t cpu_mask) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int core = 0; core < CPU_SETSIZE && core < 64; ++core) {
        if (cpu_mask & (1ULL << core)) CPU_SET(core, &cpuset);
    }
    return pthread_attr_setaffinity_np(attr, sizeof(cpuset), &cpuset);
}
#else
static inline int flow_pal_thread_attr_set_affinity(pthread_attr_t* attr,
                                                    uint64_t cpu_mask) {
    (void)attr;
    (void)cpu_mask;
    return ENOTSUP;
}
#endif

#endif /* FLOWENGINE_PLATFORM_PAL_H */
