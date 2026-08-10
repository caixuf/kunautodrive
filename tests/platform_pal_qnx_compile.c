/*
 * This is a compile-only contract test, not a QNX runtime test.  It forces
 * platform_pal.h through its QNX capability branch with the host compiler so
 * accidental Linux-only declarations in the foundation fail during CI.
 */
#include "platform_pal.h"

#if !FLOW_PAL_PLATFORM_QNX
#error "QNX PAL compile guard did not select the QNX branch"
#endif

#if FLOW_PAL_HAS_SHARED_MEMORY_IPC || FLOW_PAL_HAS_ROBUST_MUTEX || \
    FLOW_PAL_HAS_THREAD_NAMING || FLOW_PAL_HAS_THREAD_AFFINITY
#error "QNX capability boundary unexpectedly widened"
#endif

int flow_pal_qnx_compile_contract(void) {
    struct timespec ts;
    return flow_pal_clock_gettime_monotonic(&ts);
}
