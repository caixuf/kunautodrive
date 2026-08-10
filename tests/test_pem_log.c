#include "pem_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glob.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    PemLog* log;
    uint64_t first_timestamp;
    int failed;
} Writer;

static void* write_records(void* opaque) {
    Writer* writer = (Writer*)opaque;
    const double values[8] = {0};
    for (int i = 0; i < 32; i++) {
        if (pem_log_write(writer->log, PEM_RECORD_TOPIC, 0,
                          writer->first_timestamp + (uint64_t)i,
                          writer->first_timestamp + (uint64_t)i,
                          "concurrent", values, false) != 0) {
            writer->failed = 1;
            break;
        }
    }
    return NULL;
}

int main(void) {
    char base[128];
    snprintf(base, sizeof(base), "/tmp/kunautodrive_pem_unit_%ld", (long)getpid());
    PemLog log;
    CHECK(PEM_RECORD_SIZE == 168);
    CHECK(PEM_RECORD_CRC_OFFSET == 12);
    CHECK(PEM_RECORD_NAME_OFFSET == 40);
    CHECK(PEM_RECORD_VALUES_OFFSET == 104);
    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024, 8,
                       1024ULL * 1024 * 1024) == 0);
    double values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 1, 123, 456,
                        "unit_event", values, true) == 0);
    char path[sizeof(log.current_path)];
    snprintf(path, sizeof(path), "%s", log.current_path);
    pem_log_close(&log);

    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_size == (off_t)PEM_RECORD_SIZE);
    FILE* file = fopen(path, "rb");
    CHECK(file);
    uint8_t record[PEM_RECORD_SIZE];
    CHECK(fread(record, sizeof(record), 1, file) == 1);
    fclose(file);
    CHECK(memcmp(record, "PEM1", 4) == 0);
    CHECK(record[4] == 1 && record[6] == PEM_RECORD_EVENT);
    CHECK(memcmp(record + 40, "unit_event", 10) == 0);
    remove(path);

    CHECK(pem_log_open(&log, base, 300, 1, 8,
                       1024ULL * 1024 * 1024) == 0);
    CHECK(pem_log_write(&log, PEM_RECORD_SYSTEM, 0, 1000, 2000,
                        "first", values, false) == 0);
    char first_path[sizeof(log.current_path)];
    snprintf(first_path, sizeof(first_path), "%s", log.current_path);
    CHECK(pem_log_write(&log, PEM_RECORD_SYSTEM, 0, 1001, 2001,
                        "second", values, false) == 0);
    CHECK(strcmp(first_path, log.current_path) != 0);
    char second_path[sizeof(log.current_path)];
    snprintf(second_path, sizeof(second_path), "%s", log.current_path);
    pem_log_close(&log);
    remove(first_path);
    remove(second_path);

    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024, 8,
                       1024ULL * 1024 * 1024) == 0);
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 0, 2000, 3000,
                        "restart", values, false) == 0);
    char restart_a[sizeof(log.current_path)];
    snprintf(restart_a, sizeof(restart_a), "%s", log.current_path);
    pem_log_close(&log);
    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024, 8,
                       1024ULL * 1024 * 1024) == 0);
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 0, 2001, 3001,
                        "restart", values, false) == 0);
    CHECK(strcmp(restart_a, log.current_path) != 0);
    char restart_b[sizeof(log.current_path)];
    snprintf(restart_b, sizeof(restart_b), "%s", log.current_path);
    pem_log_close(&log);
    remove(restart_a);
    remove(restart_b);

    /* A retention limit removes oldest closed segments before storage grows
     * without bound. The current segment is always retained. */
    CHECK(pem_log_open(&log, base, 300, 1, 2, 1024 * 1024) == 0);
    for (int i = 0; i < 4; i++) {
        CHECK(pem_log_write(&log, PEM_RECORD_SYSTEM, 0, 3000 + (uint64_t)i,
                            4000 + (uint64_t)i, "retention", values, false) == 0);
    }
    pem_log_close(&log);
    char pattern[sizeof(base) + 8];
    snprintf(pattern, sizeof(pattern), "%s_*.pem", base);
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    CHECK(glob(pattern, 0, NULL, &matches) == 0);
    CHECK(matches.gl_pathc == 2);
    for (size_t i = 0; i < matches.gl_pathc; i++) remove(matches.gl_pathv[i]);
    globfree(&matches);

    /* Directory quota is enforced alongside segment count. */
    CHECK(pem_log_open(&log, base, 300, 1, 8, PEM_RECORD_SIZE * 2) == 0);
    for (int i = 0; i < 4; i++) {
        CHECK(pem_log_write(&log, PEM_RECORD_TOPIC, 0, 5000 + (uint64_t)i,
                            6000 + (uint64_t)i, "quota", values, false) == 0);
    }
    pem_log_close(&log);
    memset(&matches, 0, sizeof(matches));
    CHECK(glob(pattern, 0, NULL, &matches) == 0);
    uint64_t retained_bytes = 0;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        CHECK(stat(matches.gl_pathv[i], &st) == 0);
        retained_bytes += (uint64_t)st.st_size;
        remove(matches.gl_pathv[i]);
    }
    CHECK(retained_bytes <= PEM_RECORD_SIZE * 2);
    globfree(&matches);

    /* Concurrent producers serialize through PemLog's internal writer lock. */
    CHECK(pem_log_open(&log, base, 300, 1024 * 1024, 8,
                       1024ULL * 1024 * 1024) == 0);
    Writer writers[2] = {
        {.log = &log, .first_timestamp = 7000, .failed = 0},
        {.log = &log, .first_timestamp = 8000, .failed = 0},
    };
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, write_records, &writers[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, write_records, &writers[1]) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(pthread_join(threads[1], NULL) == 0);
    CHECK(!writers[0].failed && !writers[1].failed);
    char concurrent_path[sizeof(log.current_path)];
    snprintf(concurrent_path, sizeof(concurrent_path), "%s", log.current_path);
    pem_log_close(&log);
    CHECK(stat(concurrent_path, &st) == 0);
    CHECK(st.st_size == (off_t)(64 * PEM_RECORD_SIZE));
    file = fopen(concurrent_path, "rb");
    CHECK(file);
    bool seen_sequence[64] = {false};
    for (int i = 0; i < 64; i++) {
        CHECK(fread(record, sizeof(record), 1, file) == 1);
        uint32_t sequence = (uint32_t)record[PEM_RECORD_SEQUENCE_OFFSET] |
                            ((uint32_t)record[PEM_RECORD_SEQUENCE_OFFSET + 1] << 8) |
                            ((uint32_t)record[PEM_RECORD_SEQUENCE_OFFSET + 2] << 16) |
                            ((uint32_t)record[PEM_RECORD_SEQUENCE_OFFSET + 3] << 24);
        CHECK(sequence < 64 && !seen_sequence[sequence]);
        seen_sequence[sequence] = true;
    }
    fclose(file);
    remove(concurrent_path);

    puts("PEM log protocol PASS");
    return 0;
}
