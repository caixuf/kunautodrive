#include "platform_pal.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            ++failures; \
        } \
    } while (0)

int main(void) {
    CHECK(FLOW_PAL_PLATFORM_LINUX == 1, "Linux build selects Linux PAL");
    CHECK(flow_pal_has_capability(FLOW_PAL_CAP_MONOTONIC_CLOCK),
          "monotonic clock is available");
    CHECK(flow_pal_has_capability(FLOW_PAL_CAP_THREAD_NAMING),
          "current-thread naming is available");
    CHECK(flow_pal_has_capability(FLOW_PAL_CAP_SHARED_MEMORY_IPC),
          "shared-memory IPC is available");
    CHECK(flow_pal_has_capability(FLOW_PAL_CAP_ROBUST_MUTEX),
          "robust mutex recovery is available");
    CHECK(flow_pal_has_capability(FLOW_PAL_CAP_THREAD_AFFINITY),
          "thread affinity is available");
    CHECK(flow_pal_ipc_cond_clock() == CLOCK_MONOTONIC,
          "IPC condvar uses CLOCK_MONOTONIC");

    struct timespec realtime;
    struct timespec monotonic;
    CHECK(flow_pal_clock_gettime_realtime(&realtime) == 0,
          "realtime clock wrapper succeeds");
    CHECK(flow_pal_clock_gettime_monotonic(&monotonic) == 0,
          "monotonic clock wrapper succeeds");
    CHECK(realtime.tv_sec > 0 && monotonic.tv_sec > 0,
          "clock wrappers return populated timestamps");
    CHECK(flow_pal_thread_set_current_name("flow-pal-test") == 0,
          "thread naming wrapper succeeds");

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    CHECK(flow_pal_ipc_sync_init(&mutex, &cond) == 0,
          "process-shared IPC synchronization initializes");
    CHECK(flow_pal_ipc_mutex_lock(&mutex) == 0,
          "IPC mutex wrapper locks");
    CHECK(pthread_mutex_unlock(&mutex) == 0, "IPC mutex unlocks");
    CHECK(pthread_cond_destroy(&cond) == 0, "IPC condvar destroys");
    CHECK(pthread_mutex_destroy(&mutex) == 0, "IPC mutex destroys");

    if (failures != 0) {
        fprintf(stderr, "platform PAL tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("platform PAL tests: PASS\n");
    return 0;
}
