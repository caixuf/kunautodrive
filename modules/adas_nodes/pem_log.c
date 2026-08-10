#include "pem_log.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define PEM_FSYNC _commit
#define PEM_FILENO _fileno
#else
#include <fcntl.h>
#include <unistd.h>
#define PEM_FSYNC fsync
#define PEM_FILENO fileno
#endif

#define PEM_RECORD_SIZE 168u
#define PEM_CRC_OFFSET 12u
#define PEM_NAME_OFFSET 40u
#define PEM_VALUES_OFFSET 104u

static void put_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t* dst, uint32_t value) {
    for (int i = 0; i < 4; i++) dst[i] = (uint8_t)(value >> (8 * i));
}

static void put_u64_le(uint8_t* dst, uint64_t value) {
    for (int i = 0; i < 8; i++) dst[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t crc32_bytes(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static int open_segment(PemLog* log, uint64_t now_us) {
    time_t now = time(NULL);
    struct tm tm_now;
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    int fd = -1;
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        snprintf(log->current_path, sizeof(log->current_path), "%s_%s_%03u.pem",
                 log->base_path, stamp, log->sequence++);
#if defined(_WIN32)
        fd = _open(log->current_path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
        fd = open(log->current_path, O_CREAT | O_EXCL | O_WRONLY, 0644);
#endif
        if (fd >= 0) break;
        if (errno != EEXIST) return -1;
    }
    if (fd < 0) return -1;
#if defined(_WIN32)
    log->file = _fdopen(fd, "wb");
#else
    log->file = fdopen(fd, "wb");
#endif
    if (!log->file) {
#if defined(_WIN32)
        _close(fd);
#else
        close(fd);
#endif
        return -1;
    }
    log->opened_us = now_us;
    log->bytes_written = 0;
    return 0;
}

static int fail_stream(PemLog* log) {
    if (log->file) {
        fclose(log->file);
        log->file = NULL;
    }
    log->pending_records = 0;
    return -1;
}

static int rotate_if_needed(PemLog* log, uint64_t now_us) {
    if (!log->file) return open_segment(log, now_us);
    bool by_time = log->rotate_us > 0 && now_us - log->opened_us >= log->rotate_us;
    bool by_size = log->rotate_bytes > 0 && log->bytes_written >= log->rotate_bytes;
    if (!by_time && !by_size) return 0;
    fflush(log->file);
    PEM_FSYNC(PEM_FILENO(log->file));
    fclose(log->file);
    log->file = NULL;
    log->pending_records = 0;
    return open_segment(log, now_us);
}

int pem_log_open(PemLog* log, const char* path, uint64_t rotate_sec,
                 uint64_t rotate_bytes) {
    if (!log || !path || !path[0]) return -1;
    memset(log, 0, sizeof(*log));
    snprintf(log->base_path, sizeof(log->base_path), "%s", path);
    log->rotate_us = rotate_sec * 1000000ULL;
    log->rotate_bytes = rotate_bytes;
    return 0;
}

int pem_log_write(PemLog* log, uint16_t type, uint16_t flags,
                  uint64_t monotonic_us, uint64_t realtime_us,
                  const char* name, const double values[8], bool critical) {
    if (!log || !name || !values) return -1;
    if (rotate_if_needed(log, monotonic_us) != 0) return -1;

    uint8_t record[PEM_RECORD_SIZE] = {0};
    put_u32_le(record, PEM_RECORD_MAGIC);
    put_u16_le(record + 4, PEM_RECORD_VERSION);
    put_u16_le(record + 6, type);
    put_u32_le(record + 8, PEM_RECORD_SIZE);
    put_u64_le(record + 16, monotonic_us);
    put_u64_le(record + 24, realtime_us);
    put_u32_le(record + 32, log->record_sequence++);
    put_u16_le(record + 36, flags);
    size_t name_len = strnlen(name, 63);
    memcpy(record + PEM_NAME_OFFSET, name, name_len);
    for (int i = 0; i < 8; i++) {
        uint64_t bits = 0;
        memcpy(&bits, &values[i], sizeof(bits));
        put_u64_le(record + PEM_VALUES_OFFSET + (size_t)i * 8, bits);
    }
    put_u32_le(record + PEM_CRC_OFFSET, crc32_bytes(record, sizeof(record)));

    if (fwrite(record, sizeof(record), 1, log->file) != 1)
        return fail_stream(log);
    log->bytes_written += sizeof(record);
    log->pending_records++;
    if (critical || log->pending_records >= 50) {
        if (fflush(log->file) != 0) return fail_stream(log);
        log->pending_records = 0;
        if (critical && PEM_FSYNC(PEM_FILENO(log->file)) != 0)
            return fail_stream(log);
    }
    return 0;
}

void pem_log_close(PemLog* log) {
    if (!log || !log->file) return;
    fflush(log->file);
    PEM_FSYNC(PEM_FILENO(log->file));
    fclose(log->file);
    log->file = NULL;
}
