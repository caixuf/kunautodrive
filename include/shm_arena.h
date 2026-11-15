#ifndef SHM_ARENA_H
#define SHM_ARENA_H

/**
 * @file shm_arena.h
 * @brief Cross-process lock-free shared-memory ring arena for zero-copy payloads.
 *
 * Payloads larger than MSG_INLINE_MAX_SIZE (4096B) are placed in a ShmArena
 * and referenced by a 32-byte ShmPayloadRef descriptor inside Message/IpcChannel.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHM_ARENA_MAGIC            0x464C4152u  /* 'FLAR' */
#define SHM_ARENA_VERSION          1u
#define SHM_ARENA_DEFAULT_CAPACITY (16u * 1024u * 1024u)  /* 16 MB */
#define SHM_ARENA_NAME_MAX         96

typedef struct ShmPayloadRef {
    uint32_t arena_id;    /**< Logical arena identifier */
    uint32_t generation;  /**< Ring wrap generation */
    uint64_t offset;      /**< Byte offset within arena data region */
    uint32_t size;        /**< Valid payload bytes */
    uint32_t flags;       /**< Reserved / alignment / type flags */
    uint64_t seq_end;     /**< Monotonic byte cursor at end of allocation */
} ShmPayloadRef;

#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ShmPayloadRef) == 32, "ShmPayloadRef must be 32 bytes");
#endif
#endif

typedef struct ShmArena ShmArena;

/** Create or recreate a named POSIX SHM arena (writer). */
ShmArena* shm_arena_create(const char* name, uint32_t arena_id, size_t data_capacity);

/** Create an auto-named POSIX SHM arena unique to this process/instance. */
ShmArena* shm_arena_create_auto(uint32_t arena_id, size_t data_capacity);

/** Open an existing named POSIX SHM arena (reader, read-only mapping). */
ShmArena* shm_arena_open(const char* name);

/** Close mapping and optionally unlink the SHM object. */
void shm_arena_destroy(ShmArena* arena, bool unlink_shm);

/** Return the POSIX SHM object name (e.g. "/flow_arena_..."). */
const char* shm_arena_name(const ShmArena* arena);

/** Return the data region capacity in bytes. */
size_t shm_arena_capacity(const ShmArena* arena);

/** Return the arena_id. */
uint32_t shm_arena_id(const ShmArena* arena);

/**
 * Lock-free contiguous allocation in the SHM ring.
 * @param arena    Writer arena handle
 * @param size     Payload bytes (> 0, <= capacity)
 * @param align    Power-of-two alignment (0 or <8 defaults to 8; e.g. 64 for cacheline/SIMD)
 * @param out_ref  Filled 32B descriptor
 * @param out_ptr  Mapped writable pointer inside SHM
 * @return 0 on success, -1 on invalid params / OOM
 */
int shm_arena_alloc(ShmArena* arena, uint32_t size, uint32_t align,
                    ShmPayloadRef* out_ref, void** out_ptr);

/**
 * Resolve a ShmPayloadRef to a readable pointer in this process's mapping.
 * Returns NULL if ref is invalid, mismatched arena_id, or already overwritten.
 */
const void* shm_arena_resolve(const ShmArena* arena, const ShmPayloadRef* ref);

/** Check whether a previously allocated ShmPayloadRef is still un-overwritten. */
bool shm_arena_is_valid(const ShmArena* arena, const ShmPayloadRef* ref);

#ifdef __cplusplus
}
#endif

#endif /* SHM_ARENA_H */
