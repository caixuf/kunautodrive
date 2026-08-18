#include <pem_log.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define PEM_FSYNC _commit
#define PEM_FILENO _fileno
#else
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>
#define PEM_FSYNC fsync
#define PEM_FILENO fileno
#endif

#define PEM_BATCH_RECORDS 50u
#define PEM_FLUSH_INTERVAL_US 1000000ULL

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

static int pem_mutex_init(PemLog* log) {
#if defined(_WIN32)
    InitializeCriticalSection(&log->mutex);
    return 0;
#else
    return pthread_mutex_init(&log->mutex, NULL);
#endif
}

static void pem_mutex_destroy(PemLog* log) {
#if defined(_WIN32)
    DeleteCriticalSection(&log->mutex);
#else
    pthread_mutex_destroy(&log->mutex);
#endif
    log->mutex_initialized = false;
}

static void pem_mutex_lock(PemLog* log) {
#if defined(_WIN32)
    EnterCriticalSection(&log->mutex);
#else
    pthread_mutex_lock(&log->mutex);
#endif
}

static void pem_mutex_unlock(PemLog* log) {
#if defined(_WIN32)
    LeaveCriticalSection(&log->mutex);
#else
    pthread_mutex_unlock(&log->mutex);
#endif
}

#if !defined(_WIN32)
static int sync_parent_dir(const char* path) {
    char directory[512];
    if (snprintf(directory, sizeof(directory), "%s", path) >= (int)sizeof(directory))
        return -1;
    char* slash = strrchr(directory, '/');
    if (!slash) return 0;
    if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = open(directory, flags);
    if (fd < 0) return -1;
    int rc = PEM_FSYNC(fd);
    close(fd);
    return rc;
}

typedef struct {
    char* path;
    uint64_t size;
    time_t mtime;
} PemSegment;

static int compare_segments(const void* lhs, const void* rhs) {
    const PemSegment* a = (const PemSegment*)lhs;
    const PemSegment* b = (const PemSegment*)rhs;
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
    return strcmp(a->path, b->path);
}

static void prune_segments(PemLog* log) {
    if (log->max_segments == 0 && log->max_total_bytes == 0) return;
    char pattern[sizeof(log->base_path) + 8];
    if (snprintf(pattern, sizeof(pattern), "%s_*.pem", log->base_path) >=
        (int)sizeof(pattern)) return;

    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    if (glob(pattern, 0, NULL, &matches) != 0) {
        globfree(&matches);
        return;
    }
    PemSegment* segments = calloc(matches.gl_pathc, sizeof(*segments));
    if (!segments) {
        globfree(&matches);
        return;
    }

    size_t count = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        struct stat st;
        if (stat(matches.gl_pathv[i], &st) != 0 || !S_ISREG(st.st_mode)) continue;
        segments[count].path = matches.gl_pathv[i];
        segments[count].size = (uint64_t)st.st_size;
        segments[count].mtime = st.st_mtime;
        total += segments[count].size;
        count++;
    }
    qsort(segments, count, sizeof(*segments), compare_segments);
    /* Reserve one segment's configured budget before writing the new segment.
     * This keeps a quota from being exceeded only after the next rotation. */
    uint64_t quota_before_write = log->max_total_bytes;
    if (quota_before_write > log->rotate_bytes)
        quota_before_write -= log->rotate_bytes;
    for (size_t i = 0; i < count; i++) {
        bool too_many = log->max_segments > 0 && count > log->max_segments;
        bool too_large = log->max_total_bytes > 0 && total > quota_before_write;
        if (!too_many && !too_large) break;
        if (strcmp(segments[i].path, log->current_path) == 0) continue;
        if (unlink(segments[i].path) == 0) {
            total -= segments[i].size;
            count--;
        }
    }
    free(segments);
    globfree(&matches);
}
#else
/* Windows package builds do not include node plugins yet. Keep the API
 * explicit here rather than silently pretending retention is configured. */
static void prune_segments(PemLog* log) {
    (void)log;
}
#endif

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
        fd = _open(log->current_path, O_CREAT | O_EXCL | O_WRONLY | O_BINARY,
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
    log->last_flush_us = now_us;
#if !defined(_WIN32)
    /* fsync the directory after O_EXCL creation so a critical record's file
     * name survives an abrupt power loss as well as its contents. */
    if (sync_parent_dir(log->current_path) != 0) {
        fclose(log->file);
        log->file = NULL;
        unlink(log->current_path);
        return -1;
    }
#endif
    prune_segments(log);
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
    if (fflush(log->file) != 0 || PEM_FSYNC(PEM_FILENO(log->file)) != 0)
        return fail_stream(log);
    fclose(log->file);
    log->file = NULL;
    log->pending_records = 0;
    return open_segment(log, now_us);
}

int pem_log_open(PemLog* log, const char* path, uint64_t rotate_sec,
                 uint64_t rotate_bytes, uint32_t max_segments,
                 uint64_t max_total_bytes) {
    if (!log || !path || !path[0]) return -1;
    memset(log, 0, sizeof(*log));
    if (pem_mutex_init(log) != 0) return -1;
    log->mutex_initialized = true;
    snprintf(log->base_path, sizeof(log->base_path), "%s", path);
    log->rotate_us = rotate_sec * 1000000ULL;
    log->rotate_bytes = rotate_bytes;
    log->max_segments = max_segments;
    log->max_total_bytes = max_total_bytes;
    return 0;
}

int pem_log_write(PemLog* log, uint16_t type, uint16_t flags,
                  uint64_t monotonic_us, uint64_t realtime_us,
                  const char* name, const double values[8], bool critical) {
    if (!log || !log->mutex_initialized || !name || !values) return -1;
    pem_mutex_lock(log);
    int rc = -1;
    if (rotate_if_needed(log, monotonic_us) != 0) goto done;

    uint8_t record[PEM_RECORD_SIZE] = {0};
    put_u32_le(record + PEM_RECORD_MAGIC_OFFSET, PEM_RECORD_MAGIC);
    put_u16_le(record + PEM_RECORD_VERSION_OFFSET, PEM_RECORD_VERSION);
    put_u16_le(record + PEM_RECORD_TYPE_OFFSET, type);
    put_u32_le(record + PEM_RECORD_SIZE_OFFSET, (uint32_t)PEM_RECORD_SIZE);
    put_u64_le(record + PEM_RECORD_MONOTONIC_OFFSET, monotonic_us);
    put_u64_le(record + PEM_RECORD_REALTIME_OFFSET, realtime_us);
    put_u32_le(record + PEM_RECORD_SEQUENCE_OFFSET, log->record_sequence++);
    put_u16_le(record + PEM_RECORD_FLAGS_OFFSET, flags);
    size_t name_len = strnlen(name, PEM_RECORD_NAME_SIZE - 1);
    memcpy(record + PEM_RECORD_NAME_OFFSET, name, name_len);
    for (size_t i = 0; i < PEM_RECORD_VALUE_COUNT; i++) {
        uint64_t bits = 0;
        memcpy(&bits, &values[i], sizeof(bits));
        put_u64_le(record + PEM_RECORD_VALUES_OFFSET + (size_t)i * sizeof(bits), bits);
    }
    put_u32_le(record + PEM_RECORD_CRC_OFFSET, crc32_bytes(record, sizeof(record)));

    if (fwrite(record, sizeof(record), 1, log->file) != 1)
        goto failed;
    log->bytes_written += sizeof(record);
    log->pending_records++;
    if (critical || log->pending_records >= PEM_BATCH_RECORDS ||
        monotonic_us - log->last_flush_us >= PEM_FLUSH_INTERVAL_US) {
        if (fflush(log->file) != 0) goto failed;
        log->pending_records = 0;
        log->last_flush_us = monotonic_us;
        if (critical && PEM_FSYNC(PEM_FILENO(log->file)) != 0) goto failed;
    }
    rc = 0;
    goto done;

failed:
    fail_stream(log);
done:
    pem_mutex_unlock(log);
    return rc;
}

void pem_log_close(PemLog* log) {
    if (!log || !log->mutex_initialized) return;
    pem_mutex_lock(log);
    if (log->file) {
        fflush(log->file);
        PEM_FSYNC(PEM_FILENO(log->file));
        fclose(log->file);
        log->file = NULL;
    }
    pem_mutex_unlock(log);
    pem_mutex_destroy(log);
}
