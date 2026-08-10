#ifndef FLOWENGINE_PEM_LOG_H
#define FLOWENGINE_PEM_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION PemLogMutex;
#else
#include <pthread.h>
typedef pthread_mutex_t PemLogMutex;
#endif

#define PEM_RECORD_MAGIC 0x314D4550u
#define PEM_RECORD_VERSION 1u
#define PEM_RECORD_NAME_SIZE 64u
#define PEM_RECORD_VALUE_COUNT 8u
#define PEM_RECORD_SYSTEM 1u
#define PEM_RECORD_TOPIC 2u
#define PEM_RECORD_HEALTH 3u
#define PEM_RECORD_EVENT 4u

/* Layout is shared with tools/pem_dump.py through include/pem_log_layout.def. */
typedef struct {
#define PEM_FIELD(name, size) uint8_t name[size];
#include <pem_log_layout.def>
#undef PEM_FIELD
} PemRecordLayout;

#define PEM_RECORD_SIZE ((size_t)sizeof(PemRecordLayout))
#define PEM_RECORD_MAGIC_OFFSET offsetof(PemRecordLayout, magic)
#define PEM_RECORD_VERSION_OFFSET offsetof(PemRecordLayout, version)
#define PEM_RECORD_TYPE_OFFSET offsetof(PemRecordLayout, type)
#define PEM_RECORD_SIZE_OFFSET offsetof(PemRecordLayout, size)
#define PEM_RECORD_CRC_OFFSET offsetof(PemRecordLayout, crc32)
#define PEM_RECORD_MONOTONIC_OFFSET offsetof(PemRecordLayout, monotonic_us)
#define PEM_RECORD_REALTIME_OFFSET offsetof(PemRecordLayout, realtime_us)
#define PEM_RECORD_SEQUENCE_OFFSET offsetof(PemRecordLayout, sequence)
#define PEM_RECORD_FLAGS_OFFSET offsetof(PemRecordLayout, flags)
#define PEM_RECORD_NAME_OFFSET offsetof(PemRecordLayout, name)
#define PEM_RECORD_VALUES_OFFSET offsetof(PemRecordLayout, values)

/*
 * PemLog is safe for concurrent writes, but callers must stop writers before
 * pem_log_close(). Records are serialized through one lock and preserve write
 * order. Prefer one monitor-owned writer so a slow filesystem cannot block
 * unrelated worker threads.
 */
typedef struct {
    FILE* file;
    char base_path[512];
    char current_path[576];
    uint64_t opened_us;
    uint64_t bytes_written;
    uint64_t rotate_us;
    uint64_t rotate_bytes;
    uint64_t max_total_bytes;
    uint64_t last_flush_us;
    uint32_t sequence;
    uint32_t record_sequence;
    uint32_t pending_records;
    uint32_t max_segments;
    PemLogMutex mutex;
    bool mutex_initialized;
} PemLog;

int pem_log_open(PemLog* log, const char* path, uint64_t rotate_sec,
                 uint64_t rotate_bytes, uint32_t max_segments,
                 uint64_t max_total_bytes);
int pem_log_write(PemLog* log, uint16_t type, uint16_t flags,
                  uint64_t monotonic_us, uint64_t realtime_us,
                  const char* name, const double values[8], bool critical);
void pem_log_close(PemLog* log);

#endif
