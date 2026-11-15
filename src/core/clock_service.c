#include "clock_service.h"
#include "platform_pal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define FLOW_CLOCK_SHM_MAGIC   0x46434C4BU /* "FCLK" */
#define FLOW_CLOCK_SHM_VERSION 2U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t mode;
    uint32_t barrier_slots;
    uint64_t sim_time_us;
    uint64_t step_us;
    uint64_t tick_seq;
    uint64_t random_seed;
    uint64_t slot_done_tick[CLOCK_BARRIER_MAX_SLOTS];
} FlowSimClockShm;

static pthread_mutex_t  g_clock_mutex = PTHREAD_MUTEX_INITIALIZER;
static ClockMode        g_mode        = CLOCK_MODE_SOFT_RT;
static uint64_t         g_sim_time    = 0;
static uint64_t         g_step_us     = 0;
static uint64_t         g_tick_seq    = 0;
static uint64_t         g_seed        = 0;
static uint32_t         g_local_slots = 0;
static uint64_t         g_local_done[CLOCK_BARRIER_MAX_SLOTS] = {0};

static FlowSimClockShm* g_shm         = NULL;
static bool             g_shm_owner   = false;
static char             g_shm_name[128] = {0};

static inline uint32_t shm_load32(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void shm_store32(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline uint64_t shm_load64(const uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void shm_store64(uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

uint64_t clock_now_realtime_us(void) {
    struct timespec ts;
    flow_pal_clock_gettime_realtime(&ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t clock_now_monotonic_wall_us(void) {
    struct timespec ts;
    flow_pal_clock_gettime_monotonic(&ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t clock_now_us(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        ClockMode mode = (ClockMode)shm_load32(&shm->mode);
        if (mode != CLOCK_MODE_SOFT_RT) {
            return shm_load64(&shm->sim_time_us);
        }
        return clock_now_monotonic_wall_us();
    }

    pthread_mutex_lock(&g_clock_mutex);
    ClockMode mode = g_mode;
    uint64_t t = g_sim_time;
    pthread_mutex_unlock(&g_clock_mutex);

    if (mode != CLOCK_MODE_SOFT_RT) {
        return t;
    }
    return clock_now_monotonic_wall_us();
}

void clock_set_mode(ClockMode mode) {
    pthread_mutex_lock(&g_clock_mutex);
    g_mode = mode;
    if (g_shm) {
        shm_store32(&g_shm->mode, (uint32_t)mode);
    }
    pthread_mutex_unlock(&g_clock_mutex);
}

ClockMode clock_get_mode(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        return (ClockMode)shm_load32(&shm->mode);
    }
    pthread_mutex_lock(&g_clock_mutex);
    ClockMode mode = g_mode;
    pthread_mutex_unlock(&g_clock_mutex);
    return mode;
}

const char* clock_mode_name(ClockMode mode) {
    switch (mode) {
        case CLOCK_MODE_SOFT_RT:   return "soft-rt";
        case CLOCK_MODE_FAST_TIME: return "fast-time";
        case CLOCK_MODE_LOCK_STEP: return "lock-step";
        default:                   return "unknown";
    }
}

bool clock_rt_latency_valid(void) {
    return clock_get_mode() == CLOCK_MODE_SOFT_RT;
}

void clock_set_sim_mode(bool enable) {
    clock_set_mode(enable ? CLOCK_MODE_FAST_TIME : CLOCK_MODE_SOFT_RT);
}

void clock_set_sim_time(uint64_t timestamp_us) {
    pthread_mutex_lock(&g_clock_mutex);
    g_sim_time = timestamp_us;
    if (timestamp_us == 0) {
        g_tick_seq = 0;
    }
    if (g_shm) {
        shm_store64(&g_shm->sim_time_us, timestamp_us);
        if (timestamp_us == 0) {
            shm_store64(&g_shm->tick_seq, 0);
        }
    }
    pthread_mutex_unlock(&g_clock_mutex);
}

bool clock_is_sim_mode(void) {
    return clock_get_mode() != CLOCK_MODE_SOFT_RT;
}

void clock_advance_us(uint64_t delta_us) {
    pthread_mutex_lock(&g_clock_mutex);
    if (g_shm && shm_load32(&g_shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        ClockMode mode = (ClockMode)shm_load32(&g_shm->mode);
        if (mode != CLOCK_MODE_SOFT_RT) {
            uint64_t next_t = shm_load64(&g_shm->sim_time_us) + delta_us;
            shm_store64(&g_shm->sim_time_us, next_t);
            __atomic_fetch_add(&g_shm->tick_seq, 1ULL, __ATOMIC_RELEASE);
            g_sim_time = next_t;
            g_tick_seq = shm_load64(&g_shm->tick_seq);
        }
    } else if (g_mode != CLOCK_MODE_SOFT_RT) {
        g_sim_time += delta_us;
        g_tick_seq += 1ULL;
    }
    pthread_mutex_unlock(&g_clock_mutex);
}

void clock_set_step_us(uint64_t step_us) {
    pthread_mutex_lock(&g_clock_mutex);
    g_step_us = step_us;
    if (g_shm) {
        shm_store64(&g_shm->step_us, step_us);
    }
    pthread_mutex_unlock(&g_clock_mutex);
}

uint64_t clock_get_step_us(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        return shm_load64(&shm->step_us);
    }
    pthread_mutex_lock(&g_clock_mutex);
    uint64_t s = g_step_us;
    pthread_mutex_unlock(&g_clock_mutex);
    return s;
}

uint64_t clock_get_tick_seq(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        return shm_load64(&shm->tick_seq);
    }
    pthread_mutex_lock(&g_clock_mutex);
    uint64_t seq = g_tick_seq;
    pthread_mutex_unlock(&g_clock_mutex);
    return seq;
}

void clock_set_seed(uint64_t seed) {
    pthread_mutex_lock(&g_clock_mutex);
    g_seed = seed;
    if (g_shm) {
        shm_store64(&g_shm->random_seed, seed);
    }
    pthread_mutex_unlock(&g_clock_mutex);
}

uint64_t clock_get_seed(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        return shm_load64(&shm->random_seed);
    }
    pthread_mutex_lock(&g_clock_mutex);
    uint64_t s = g_seed;
    pthread_mutex_unlock(&g_clock_mutex);
    return s;
}

int clock_attach_shm(const char* shm_name, bool create) {
#if defined(_WIN32)
    (void)shm_name;
    (void)create;
    return -1;
#else
    if (!shm_name || !shm_name[0]) return -1;

    clock_detach_shm();

    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(shm_name, flags, 0666);
    if (fd < 0) return -1;

    if (create && ftruncate(fd, (off_t)sizeof(FlowSimClockShm)) != 0) {
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    void* ptr = mmap(NULL, sizeof(FlowSimClockShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        if (create) shm_unlink(shm_name);
        return -1;
    }

    FlowSimClockShm* shm = (FlowSimClockShm*)ptr;
    pthread_mutex_lock(&g_clock_mutex);
    if (create) {
        memset(shm, 0, sizeof(*shm));
        shm_store32(&shm->version, FLOW_CLOCK_SHM_VERSION);
        shm_store32(&shm->mode, (uint32_t)g_mode);
        shm_store64(&shm->sim_time_us, g_sim_time);
        shm_store64(&shm->step_us, g_step_us);
        shm_store64(&shm->tick_seq, g_tick_seq);
        shm_store64(&shm->random_seed, g_seed);
        shm_store32(&shm->magic, FLOW_CLOCK_SHM_MAGIC);
    } else if (shm_load32(&shm->magic) != FLOW_CLOCK_SHM_MAGIC ||
               shm_load32(&shm->version) != FLOW_CLOCK_SHM_VERSION) {
        pthread_mutex_unlock(&g_clock_mutex);
        munmap(ptr, sizeof(FlowSimClockShm));
        return -1;
    }
    snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);
    g_shm_owner = create;
    __atomic_store_n(&g_shm, shm, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_clock_mutex);
    return 0;
#endif
}

int clock_attach_env_shm(void) {
    const char* name = getenv("FLOW_CLOCK_SHM");
    if (!name || !name[0]) return -1;
    return clock_attach_shm(name, false);
}

void clock_detach_shm(void) {
#if !defined(_WIN32)
    pthread_mutex_lock(&g_clock_mutex);
    FlowSimClockShm* shm = g_shm;
    bool owner = g_shm_owner;
    char name[128];
    snprintf(name, sizeof(name), "%s", g_shm_name);
    __atomic_store_n(&g_shm, NULL, __ATOMIC_RELEASE);
    g_shm_owner = false;
    g_shm_name[0] = '\0';
    pthread_mutex_unlock(&g_clock_mutex);

    if (shm) {
        munmap(shm, sizeof(FlowSimClockShm));
    }
    if (owner && name[0]) {
        shm_unlink(name);
    }
#endif
}

int clock_barrier_register(void) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        uint32_t slot = __atomic_fetch_add(&shm->barrier_slots, 1U, __ATOMIC_ACQ_REL);
        if (slot >= CLOCK_BARRIER_MAX_SLOTS) {
            return -1;
        }
        shm_store64(&shm->slot_done_tick[slot], shm_load64(&shm->tick_seq));
        return (int)slot;
    }

    pthread_mutex_lock(&g_clock_mutex);
    if (g_local_slots >= CLOCK_BARRIER_MAX_SLOTS) {
        pthread_mutex_unlock(&g_clock_mutex);
        return -1;
    }
    int slot = (int)g_local_slots++;
    g_local_done[slot] = g_tick_seq;
    pthread_mutex_unlock(&g_clock_mutex);
    return slot;
}

void clock_barrier_done_tick(int slot_id, uint64_t tick_seq) {
    if (slot_id < 0 || slot_id >= CLOCK_BARRIER_MAX_SLOTS) return;
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
        shm_store64(&shm->slot_done_tick[slot_id], tick_seq);
        return;
    }
    __atomic_store_n(&g_local_done[slot_id], tick_seq, __ATOMIC_RELEASE);
}

bool clock_barrier_wait_tick(uint64_t expected_tick, uint32_t timeout_ms) {
    uint64_t deadline = clock_now_monotonic_wall_us() + (uint64_t)timeout_ms * 1000ULL;
    for (;;) {
        if (clock_get_mode() != CLOCK_MODE_LOCK_STEP) return false;
        if (clock_get_tick_seq() >= expected_tick) return true;
        if (timeout_ms > 0 && clock_now_monotonic_wall_us() >= deadline) return false;
        flow_pal_sleep_us(50);
    }
}

int clock_barrier_step_all(uint64_t delta_us, uint32_t timeout_ms) {
    FlowSimClockShm* shm = __atomic_load_n(&g_shm, __ATOMIC_ACQUIRE);
    uint64_t cur_tick = clock_get_tick_seq();
    uint64_t deadline = clock_now_monotonic_wall_us() + (uint64_t)timeout_ms * 1000ULL;
    int ready = 0;

    for (;;) {
        uint32_t total = 0;
        ready = 0;
        if (shm && shm_load32(&shm->magic) == FLOW_CLOCK_SHM_MAGIC) {
            total = shm_load32(&shm->barrier_slots);
            if (total > CLOCK_BARRIER_MAX_SLOTS) total = CLOCK_BARRIER_MAX_SLOTS;
            for (uint32_t i = 0; i < total; i++) {
                if (shm_load64(&shm->slot_done_tick[i]) >= cur_tick) {
                    ready++;
                }
            }
        } else {
            pthread_mutex_lock(&g_clock_mutex);
            total = g_local_slots;
            pthread_mutex_unlock(&g_clock_mutex);
            if (total > CLOCK_BARRIER_MAX_SLOTS) total = CLOCK_BARRIER_MAX_SLOTS;
            for (uint32_t i = 0; i < total; i++) {
                if (__atomic_load_n(&g_local_done[i], __ATOMIC_ACQUIRE) >= cur_tick) {
                    ready++;
                }
            }
        }
        if ((uint32_t)ready >= total) break;
        if (timeout_ms > 0 && clock_now_monotonic_wall_us() >= deadline) break;
        flow_pal_sleep_us(50);
    }

    clock_advance_us(delta_us);
    return ready;
}
