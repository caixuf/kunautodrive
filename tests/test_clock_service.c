#include "clock_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_clock_modes(void) {
    clock_detach_shm();
    clock_set_mode(CLOCK_MODE_SOFT_RT);
    assert(clock_get_mode() == CLOCK_MODE_SOFT_RT);
    assert(!clock_is_sim_mode());
    assert(clock_rt_latency_valid());
    assert(strcmp(clock_mode_name(CLOCK_MODE_SOFT_RT), "soft-rt") == 0);

    uint64_t t_wall = clock_now_us();
    assert(t_wall > 0);

    clock_set_mode(CLOCK_MODE_FAST_TIME);
    clock_set_sim_time(1000000ULL);
    clock_set_step_us(50000ULL);
    assert(clock_is_sim_mode());
    assert(!clock_rt_latency_valid());
    assert(clock_now_us() == 1000000ULL);
    assert(clock_get_step_us() == 50000ULL);

    uint64_t seq0 = clock_get_tick_seq();
    clock_advance_us(50000ULL);
    assert(clock_now_us() == 1050000ULL);
    assert(clock_get_tick_seq() == seq0 + 1ULL);

    clock_set_mode(CLOCK_MODE_LOCK_STEP);
    assert(clock_get_mode() == CLOCK_MODE_LOCK_STEP);
    assert(clock_is_sim_mode());
    assert(!clock_rt_latency_valid());
    assert(strcmp(clock_mode_name(CLOCK_MODE_LOCK_STEP), "lock-step") == 0);
    clock_advance_us(20000ULL);
    assert(clock_now_us() == 1070000ULL);

    clock_set_mode(CLOCK_MODE_SOFT_RT);
    assert(clock_rt_latency_valid());
}

static void test_clock_shm_and_seed(void) {
    char name[64];
    snprintf(name, sizeof(name), "/flowengine_clock_test_%d", (int)getpid());

    assert(clock_attach_shm(name, true) == 0);
    clock_set_mode(CLOCK_MODE_LOCK_STEP);
    clock_set_step_us(10000ULL);
    clock_set_sim_time(500000ULL);
    clock_set_seed(424242ULL);

    assert(clock_now_us() == 500000ULL);
    assert(clock_get_step_us() == 10000ULL);
    assert(clock_get_seed() == 424242ULL);

    uint64_t seq_before = clock_get_tick_seq();
    clock_advance_us(10000ULL);
    assert(clock_now_us() == 510000ULL);
    assert(clock_get_tick_seq() == seq_before + 1ULL);

    clock_detach_shm();
    clock_set_mode(CLOCK_MODE_SOFT_RT);
}

int main(void) {
    test_clock_modes();
    test_clock_shm_and_seed();
    printf("test_clock_service: ALL PASSED\n");
    return 0;
}
