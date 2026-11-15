#include "shm_arena.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t arena_id;
    uint32_t _pad0;
    uint64_t data_capacity;
    _Atomic uint64_t head_seq;
    uint8_t  _reserved[32];
} ShmArenaHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ShmArenaHeader) == 64, "ShmArenaHeader must be 64 bytes");
#endif

struct ShmArena {
    char            shm_name[SHM_ARENA_NAME_MAX];
    int             fd;
    void*           map_base;
    size_t          map_size;
    ShmArenaHeader* hdr;
    uint8_t*        data;
    bool            is_writer;
};

static _Atomic uint32_t g_auto_arena_seq = 1;

static void normalize_shm_name(const char* name, char* out, size_t out_sz) {
    if (!name || !name[0]) {
        snprintf(out, out_sz, "/flow_arena_default");
        return;
    }
    if (name[0] == '/') {
        snprintf(out, out_sz, "%s", name);
    } else {
        snprintf(out, out_sz, "/%s", name);
    }
}

static size_t align_up_sz(size_t v, size_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

ShmArena* shm_arena_create(const char* name, uint32_t arena_id, size_t data_capacity) {
    if (data_capacity == 0) data_capacity = SHM_ARENA_DEFAULT_CAPACITY;
    data_capacity = align_up_sz(data_capacity, 4096u);

    ShmArena* a = (ShmArena*)calloc(1, sizeof(ShmArena));
    if (!a) return NULL;
    a->fd = -1;
    a->is_writer = true;
    normalize_shm_name(name, a->shm_name, sizeof(a->shm_name));

    shm_unlink(a->shm_name);
    a->fd = shm_open(a->shm_name, O_CREAT | O_RDWR | O_EXCL, 0660);
    if (a->fd < 0) {
        free(a);
        return NULL;
    }

    a->map_size = sizeof(ShmArenaHeader) + data_capacity;
    if (ftruncate(a->fd, (off_t)a->map_size) != 0) {
        close(a->fd);
        shm_unlink(a->shm_name);
        free(a);
        return NULL;
    }

    a->map_base = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd, 0);
    if (a->map_base == MAP_FAILED) {
        a->map_base = NULL;
        close(a->fd);
        shm_unlink(a->shm_name);
        free(a);
        return NULL;
    }

    a->hdr = (ShmArenaHeader*)a->map_base;
    a->data = (uint8_t*)a->map_base + sizeof(ShmArenaHeader);
    memset(a->hdr, 0, sizeof(ShmArenaHeader));
    a->hdr->magic = SHM_ARENA_MAGIC;
    a->hdr->version = SHM_ARENA_VERSION;
    a->hdr->arena_id = arena_id;
    a->hdr->data_capacity = (uint64_t)data_capacity;
    atomic_init(&a->hdr->head_seq, 0);
    return a;
}

ShmArena* shm_arena_create_auto(uint32_t arena_id, size_t data_capacity) {
    uint32_t idx = atomic_fetch_add_explicit(&g_auto_arena_seq, 1u, memory_order_relaxed);
    char name[SHM_ARENA_NAME_MAX];
    snprintf(name, sizeof(name), "/flow_arena_%ld_%u", (long)getpid(), idx);
    return shm_arena_create(name, arena_id, data_capacity);
}

ShmArena* shm_arena_open(const char* name) {
    if (!name || !name[0]) return NULL;
    ShmArena* a = (ShmArena*)calloc(1, sizeof(ShmArena));
    if (!a) return NULL;
    a->fd = -1;
    a->is_writer = false;
    normalize_shm_name(name, a->shm_name, sizeof(a->shm_name));

    a->fd = shm_open(a->shm_name, O_RDONLY, 0);
    if (a->fd < 0) {
        free(a);
        return NULL;
    }

    struct stat st;
    if (fstat(a->fd, &st) != 0 || (size_t)st.st_size <= sizeof(ShmArenaHeader)) {
        close(a->fd);
        free(a);
        return NULL;
    }

    a->map_size = (size_t)st.st_size;
    a->map_base = mmap(NULL, a->map_size, PROT_READ, MAP_SHARED, a->fd, 0);
    if (a->map_base == MAP_FAILED) {
        a->map_base = NULL;
        close(a->fd);
        free(a);
        return NULL;
    }

    a->hdr = (ShmArenaHeader*)a->map_base;
    if (a->hdr->magic != SHM_ARENA_MAGIC ||
        a->hdr->version != SHM_ARENA_VERSION ||
        sizeof(ShmArenaHeader) + a->hdr->data_capacity > a->map_size) {
        munmap(a->map_base, a->map_size);
        close(a->fd);
        free(a);
        return NULL;
    }

    a->data = (uint8_t*)a->map_base + sizeof(ShmArenaHeader);
    return a;
}

void shm_arena_destroy(ShmArena* arena, bool unlink_shm) {
    if (!arena) return;
    if (arena->map_base) {
        munmap(arena->map_base, arena->map_size);
    }
    if (arena->fd >= 0) {
        close(arena->fd);
    }
    if (unlink_shm && arena->shm_name[0]) {
        shm_unlink(arena->shm_name);
    }
    free(arena);
}

const char* shm_arena_name(const ShmArena* arena) {
    return arena ? arena->shm_name : "";
}

size_t shm_arena_capacity(const ShmArena* arena) {
    return (arena && arena->hdr) ? (size_t)arena->hdr->data_capacity : 0;
}

uint32_t shm_arena_id(const ShmArena* arena) {
    return (arena && arena->hdr) ? arena->hdr->arena_id : 0;
}

int shm_arena_alloc(ShmArena* arena, uint32_t size, uint32_t align,
                    ShmPayloadRef* out_ref, void** out_ptr) {
    if (!arena || !arena->is_writer || !arena->hdr || !out_ref || !out_ptr || size == 0) {
        return -1;
    }
    if (align < 8u) align = 8u;
    /* Require power of two alignment */
    if ((align & (align - 1u)) != 0u) return -1;

    uint64_t cap = arena->hdr->data_capacity;
    uint64_t aligned_sz = align_up_sz((size_t)size, (size_t)align);
    if (aligned_sz > cap) return -1;

    uint64_t cur = atomic_load_explicit(&arena->hdr->head_seq, memory_order_relaxed);
    for (;;) {
        uint64_t lap = cur / cap;
        uint64_t pos_in_lap = cur % cap;
        uint64_t aligned_pos = (pos_in_lap + (uint64_t)align - 1u) & ~((uint64_t)align - 1u);

        uint64_t start_off;
        uint64_t next_seq;
        uint32_t gen;
        if (aligned_pos + aligned_sz <= cap) {
            start_off = aligned_pos;
            next_seq = lap * cap + aligned_pos + aligned_sz;
            gen = (uint32_t)lap;
        } else {
            /* Wrap cleanly to start of next lap so payload is contiguous */
            start_off = 0u;
            next_seq = (lap + 1u) * cap + aligned_sz;
            gen = (uint32_t)(lap + 1u);
        }

        if (atomic_compare_exchange_weak_explicit(
                &arena->hdr->head_seq, &cur, next_seq,
                memory_order_acq_rel, memory_order_relaxed)) {
            out_ref->arena_id = arena->hdr->arena_id;
            out_ref->generation = gen;
            out_ref->offset = start_off;
            out_ref->size = size;
            out_ref->flags = 0u;
            out_ref->seq_end = next_seq;
            *out_ptr = arena->data + start_off;
            return 0;
        }
    }
}

bool shm_arena_is_valid(const ShmArena* arena, const ShmPayloadRef* ref) {
    if (!arena || !arena->hdr || !ref) return false;
    if (ref->arena_id != arena->hdr->arena_id) return false;
    uint64_t cap = arena->hdr->data_capacity;
    if (ref->size == 0 || ref->offset + (uint64_t)ref->size > cap) return false;

    uint64_t head = atomic_load_explicit(&arena->hdr->head_seq, memory_order_acquire);
    /* Allocation starts at (seq_end - size) or earlier; if head moved > cap past start, overwritten */
    uint64_t alloc_start_seq = (uint64_t)ref->generation * cap + ref->offset;
    if (head < ref->seq_end) return false;
    if (head - alloc_start_seq > cap) return false;
    return true;
}

const void* shm_arena_resolve(const ShmArena* arena, const ShmPayloadRef* ref) {
    if (!shm_arena_is_valid(arena, ref)) return NULL;
    return (const void*)(arena->data + ref->offset);
}
