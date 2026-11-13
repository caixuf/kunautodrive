#include "inference_latency_guard.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

int main() {
    adas_infer::InferenceLatencyGuard guard;
    for (int i = 0; i < 99; ++i) guard.record(1000);
    guard.record(60000);  // 1% outlier = 60ms

    CHECK(guard.count() == 100);
    CHECK(guard.p99_us() >= 1000);
    CHECK(guard.hard_timeout(50000));
    CHECK(!guard.hard_timeout(100000));

    // Fill with 100ms outliers so P99 clearly exceeds 50ms
    for (int i = 0; i < 10; ++i) guard.record(80000);
    CHECK(guard.p99_us() >= 60000);
    CHECK(guard.p99_exceeds_budget(50000));

    std::printf("test_inference_latency_guard: all checks passed\n");
    return 0;
}
