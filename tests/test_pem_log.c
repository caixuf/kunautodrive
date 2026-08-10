#include "pem_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    const char* base = "/tmp/flowengine_pem_unit";
    PemLog log;
    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024) == 0);
    double values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 1, 123, 456,
                        "unit_event", values, true) == 0);
    char path[sizeof(log.current_path)];
    snprintf(path, sizeof(path), "%s", log.current_path);
    pem_log_close(&log);

    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_size == 168);
    FILE* file = fopen(path, "rb");
    CHECK(file);
    uint8_t record[168];
    CHECK(fread(record, sizeof(record), 1, file) == 1);
    fclose(file);
    CHECK(memcmp(record, "PEM1", 4) == 0);
    CHECK(record[4] == 1 && record[6] == PEM_RECORD_EVENT);
    CHECK(memcmp(record + 40, "unit_event", 10) == 0);
    remove(path);

    CHECK(pem_log_open(&log, base, 300, 1) == 0);
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

    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024) == 0);
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 0, 2000, 3000,
                        "restart", values, false) == 0);
    char restart_a[sizeof(log.current_path)];
    snprintf(restart_a, sizeof(restart_a), "%s", log.current_path);
    pem_log_close(&log);
    CHECK(pem_log_open(&log, base, 300, 100 * 1024 * 1024) == 0);
    CHECK(pem_log_write(&log, PEM_RECORD_EVENT, 0, 2001, 3001,
                        "restart", values, false) == 0);
    CHECK(strcmp(restart_a, log.current_path) != 0);
    char restart_b[sizeof(log.current_path)];
    snprintf(restart_b, sizeof(restart_b), "%s", log.current_path);
    pem_log_close(&log);
    remove(restart_a);
    remove(restart_b);
    puts("PEM log protocol PASS");
    return 0;
}
