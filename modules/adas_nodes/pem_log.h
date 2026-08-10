#ifndef FLOWENGINE_PEM_LOG_H
#define FLOWENGINE_PEM_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PEM_RECORD_MAGIC 0x314D4550u
#define PEM_RECORD_VERSION 1u
#define PEM_RECORD_SYSTEM 1u
#define PEM_RECORD_TOPIC 2u
#define PEM_RECORD_HEALTH 3u
#define PEM_RECORD_EVENT 4u

typedef struct {
    FILE* file;
    char base_path[512];
    char current_path[576];
    uint64_t opened_us;
    uint64_t bytes_written;
    uint64_t rotate_us;
    uint64_t rotate_bytes;
    uint32_t sequence;
    uint32_t record_sequence;
    uint32_t pending_records;
} PemLog;

int pem_log_open(PemLog* log, const char* path, uint64_t rotate_sec,
                 uint64_t rotate_bytes);
int pem_log_write(PemLog* log, uint16_t type, uint16_t flags,
                  uint64_t monotonic_us, uint64_t realtime_us,
                  const char* name, const double values[8], bool critical);
void pem_log_close(PemLog* log);

#endif
