#include "bag.h"
#include "clock_service.h"
#include "error_codes.h"
#include "message_bus.h"
#include "transport.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <unistd.h>

#ifndef FLOWCTL_TEST_BIN
#define FLOWCTL_TEST_BIN "flowctl"
#endif

#define ARTIFACT_DIR "bag_replay_test_artifacts"

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    do { printf("  %-50s ", name); fflush(stdout); } while (0)

#define PASS() \
    do { printf("\xE2\x9C\x85 PASS\n"); g_passed++; } while (0)

#define FAIL(fmt, ...) \
    do { printf("\xE2\x9D\x8C FAIL: " fmt "\n", ##__VA_ARGS__); g_failed++; } while (0)

#define ASSERT(cond, fmt, ...) \
    do { if (!(cond)) { FAIL(fmt, ##__VA_ARGS__); return; } } while (0)

typedef struct {
    int values[16];
    int count;
} IntCollector;

typedef struct {
    pthread_mutex_t mutex;
    int count;
    int last_value;
} Counter;

typedef struct {
    volatile int* delivered_count;
    int stop_after;
} LoopStopCtx;

typedef struct {
    Transport* transport;
} ReplayTransportCtx;

static void cleanup_artifacts(void) {
    unlink(ARTIFACT_DIR "/window.bag");
    unlink(ARTIFACT_DIR "/loop.bag");
    unlink(ARTIFACT_DIR "/timing.bag");
    unlink(ARTIFACT_DIR "/ipc.bag");
    rmdir(ARTIFACT_DIR);
}

static void ensure_artifact_dir(void) {
    mkdir(ARTIFACT_DIR, 0755);
}

static void write_int_record(BagWriter* writer, const char* topic,
                             uint64_t ts_us, int value) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
    snprintf(msg.sender, sizeof(msg.sender), "test_writer");
    msg.timestamp_us = ts_us;
    msg.type = MSG_TYPE_PUBLISH;
    msg.type_id = 0x12345678u;
    msg.schema_version = 1;
    msg.endian_marker = 0x12;
    msg.data_size = sizeof(value);
    memcpy(msg.data, &value, sizeof(value));
    (void)bag_writer_write(writer, &msg);
}

static int collect_int_cb(const Message* msg, void* user_data) {
    IntCollector* collector = (IntCollector*)user_data;
    int value = 0;
    if (!msg || !collector || msg->data_size < sizeof(value)) return ERR_INVALID_PARAM;
    memcpy(&value, message_bus_message_data(msg), sizeof(value));
    if (collector->count < (int)(sizeof(collector->values) / sizeof(collector->values[0])))
        collector->values[collector->count] = value;
    collector->count++;
    return ERR_OK;
}

static int noop_publish_cb(const Message* msg, void* user_data) {
    (void)msg;
    (void)user_data;
    return ERR_OK;
}

static bool loop_stop_cb(void* user_data) {
    LoopStopCtx* stop = (LoopStopCtx*)user_data;
    return stop && stop->delivered_count && *stop->delivered_count >= stop->stop_after;
}

static void counter_cb(const Message* msg, void* user_data) {
    Counter* counter = (Counter*)user_data;
    int value = 0;
    if (msg->data_size >= sizeof(value))
        memcpy(&value, message_bus_message_data(msg), sizeof(value));
    pthread_mutex_lock(&counter->mutex);
    counter->count++;
    counter->last_value = value;
    pthread_mutex_unlock(&counter->mutex);
}

static int transport_publish_cb(const Message* msg, void* user_data) {
    ReplayTransportCtx* ctx = (ReplayTransportCtx*)user_data;
    if (!ctx || !ctx->transport) return ERR_INVALID_PARAM;
    return transport_publish(ctx->transport, msg->topic,
                             message_bus_message_data(msg), msg->data_size);
}

#if defined(_WIN32)
static int run_flowctl_capture(char* const argv[], char* output, size_t output_size) {
    char command[4096];
    size_t used = 0;
    command[0] = '\0';
    for (size_t i = 0; argv[i]; i++) {
        const char* arg = argv[i];
        size_t len = strlen(arg);
        if (used + len * 2 + 4 >= sizeof(command)) return -1;
        command[used++] = '"';
        memcpy(command + used, arg, len);
        used += len;
        command[used++] = '"';
        command[used++] = ' ';
    }
    if (used + 5 >= sizeof(command)) return -1;
    memcpy(command + used, "2>&1", 5);

    FILE* stream = _popen(command, "r");
    if (!stream) return -1;
    size_t read_total = 0;
    while (read_total + 1 < output_size) {
        size_t nread = fread(output + read_total, 1,
                             output_size - read_total - 1, stream);
        read_total += nread;
        if (nread == 0) break;
    }
    output[read_total] = '\0';
    return _pclose(stream);
}
#else
static int run_flowctl_capture(char* const argv[], char* output, size_t output_size) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(FLOWCTL_TEST_BIN, argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t used = 0;
    ssize_t nread = 0;
    while ((nread = read(pipefd[0], output + used,
                         output_size > used + 1 ? output_size - used - 1 : 0)) > 0) {
        used += (size_t)nread;
        if (used + 1 >= output_size) break;
    }
    close(pipefd[0]);
    output[used] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}
#endif

static void test_replay_window_and_filter(void) {
    TEST("bag replay window + topic filter");
    ensure_artifact_dir();
    const char* path = ARTIFACT_DIR "/window.bag";
    BagWriter* writer = bag_writer_open(path);
    ASSERT(writer != NULL, "open failed");
    write_int_record(writer, "t/keep", 1000000ULL, 1);
    write_int_record(writer, "t/skip", 1200000ULL, 2);
    write_int_record(writer, "t/keep", 1400000ULL, 3);
    write_int_record(writer, "t/keep", 2100000ULL, 4);
    bag_writer_close(writer);

    BagReader* reader = bag_reader_open(path);
    ASSERT(reader != NULL, "reader open failed");

    IntCollector collector = {0};
    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.publish_fn = collect_int_cb;
    options.publish_user_data = &collector;
    options.topic_filter = "t/keep";
    options.start_offset_us = 300000ULL;
    options.end_offset_us = 900000ULL;

    uint64_t played = 0;
    ASSERT(bag_reader_play_with_options(reader, &options, &played) == ERR_OK,
           "advanced replay failed");
    ASSERT(played == 1, "expected 1 replayed message, got %llu",
           (unsigned long long)played);
    ASSERT(collector.count == 1 && collector.values[0] == 3,
           "unexpected replay payload sequence");
    bag_reader_close(reader);
    PASS();
}

static void test_replay_loop_order(void) {
    TEST("bag replay loop preserves equal-ts order");
    ensure_artifact_dir();
    const char* path = ARTIFACT_DIR "/loop.bag";
    BagWriter* writer = bag_writer_open(path);
    ASSERT(writer != NULL, "open failed");
    write_int_record(writer, "t/order", 1000000ULL, 10);
    write_int_record(writer, "t/order", 1000000ULL, 11);
    write_int_record(writer, "t/order", 2000000ULL, 12);
    bag_writer_close(writer);

    BagReader* reader = bag_reader_open(path);
    ASSERT(reader != NULL, "reader open failed");

    IntCollector collector = {0};
    LoopStopCtx stop = { .delivered_count = &collector.count, .stop_after = 5 };
    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.publish_fn = collect_int_cb;
    options.publish_user_data = &collector;
    options.loop = true;
    options.should_stop = loop_stop_cb;
    options.should_stop_user_data = &stop;

    uint64_t played = 0;
    int rc = bag_reader_play_with_options(reader, &options, &played);
    ASSERT(rc == ERR_OK, "loop replay failed rc=%d", rc);
    ASSERT(played == 5, "expected 5 looped messages, got %llu",
           (unsigned long long)played);
    ASSERT(collector.values[0] == 10 && collector.values[1] == 11 &&
           collector.values[2] == 12 && collector.values[3] == 10 &&
           collector.values[4] == 11,
           "equal-timestamp order was not deterministic");
    bag_reader_close(reader);
    PASS();
}

static void test_replay_rate_timing(void) {
    TEST("bag replay rate scales wall timing");
    ensure_artifact_dir();
    const char* path = ARTIFACT_DIR "/timing.bag";
    BagWriter* writer = bag_writer_open(path);
    ASSERT(writer != NULL, "open failed");
    write_int_record(writer, "t/timing", 1000000ULL, 1);
    write_int_record(writer, "t/timing", 1160000ULL, 2);
    bag_writer_close(writer);

    BagReader* reader = bag_reader_open(path);
    ASSERT(reader != NULL, "reader open failed");

    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.publish_fn = noop_publish_cb;
    options.speed = 2.0;

    uint64_t start = clock_now_monotonic_wall_us();
    uint64_t played = 0;
    int rc = bag_reader_play_with_options(reader, &options, &played);
    ASSERT(rc == ERR_OK, "timed replay failed rc=%d", rc);
    uint64_t elapsed = clock_now_monotonic_wall_us() - start;
    ASSERT(played == 2, "expected 2 messages, got %llu", (unsigned long long)played);
    ASSERT(elapsed >= 50000ULL && elapsed <= 220000ULL,
           "expected ~80ms/2 replay delay, got %llu us",
           (unsigned long long)elapsed);
    bag_reader_close(reader);
    PASS();
}

static void test_replay_ipc_transport_boundary(void) {
    TEST("bag replay injects through transport IPC");
    ensure_artifact_dir();
    const char* path = ARTIFACT_DIR "/ipc.bag";
    BagWriter* writer = bag_writer_open(path);
    ASSERT(writer != NULL, "open failed");
    write_int_record(writer, "ipc/replay", 1000000ULL, 7);
    write_int_record(writer, "ipc/replay", 1100000ULL, 8);
    bag_writer_close(writer);

    MessageBus* pub_bus = message_bus_create("bag_replay_pub");
    ASSERT(pub_bus != NULL, "pub bus create failed");
    Transport* pub_transport = transport_create(pub_bus, NULL, TRANSPORT_IPC);
    ASSERT(pub_transport != NULL, "pub transport create failed");
    ASSERT(transport_start(pub_transport) == 0, "pub transport start failed");
    ASSERT(transport_advertise(pub_transport, "ipc/replay", 0) == 0,
           "pub advertise failed");

#if defined(_WIN32)
    MessageBus* sub_bus = message_bus_create("bag_replay_sub");
    Transport* sub_transport = sub_bus
        ? transport_create(sub_bus, NULL, TRANSPORT_IPC) : NULL;
    Counter counter = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    ASSERT(sub_bus && sub_transport, "subscriber transport create failed");
    ASSERT(transport_start(sub_transport) == 0, "subscriber transport start failed");
    ASSERT(transport_subscribe(sub_transport, "ipc/replay", counter_cb, &counter) == 0,
           "subscriber transport subscribe failed");

    BagReader* reader = bag_reader_open(path);
    ASSERT(reader != NULL, "reader open failed");
    ReplayTransportCtx ctx = { .transport = pub_transport };
    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.publish_fn = transport_publish_cb;
    options.publish_user_data = &ctx;

    uint64_t played = 0;
    int rc = bag_reader_play_with_options(reader, &options, &played);
    ASSERT(rc == ERR_OK, "IPC replay failed rc=%d", rc);
    ASSERT(played == 2, "expected 2 IPC replayed messages, got %llu",
           (unsigned long long)played);
    bag_reader_close(reader);

    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&counter.mutex);
        int delivered = counter.count;
        pthread_mutex_unlock(&counter.mutex);
        if (delivered >= 2) break;
        usleep(50000);
    }
    pthread_mutex_lock(&counter.mutex);
    int delivered = counter.count;
    int last_value = counter.last_value;
    pthread_mutex_unlock(&counter.mutex);
    ASSERT(delivered == 2,
           "subscriber saw count=%d last=%d", delivered, last_value);
    transport_destroy(sub_transport);
    message_bus_destroy(sub_bus);
#else
    int ready_pipe[2];
    int result_pipe[2];
    ASSERT(pipe(ready_pipe) == 0, "ready pipe failed");
    ASSERT(pipe(result_pipe) == 0, "result pipe failed");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork failed");
    if (pid == 0) {
        close(ready_pipe[0]);
        close(result_pipe[0]);

        MessageBus* sub_bus = message_bus_create("bag_replay_sub");
        Transport* sub_transport = sub_bus ? transport_create(sub_bus, NULL, TRANSPORT_IPC) : NULL;
        Counter counter = { .mutex = PTHREAD_MUTEX_INITIALIZER };
        if (!sub_bus || !sub_transport || transport_start(sub_transport) != 0 ||
            transport_subscribe(sub_transport, "ipc/replay", counter_cb, &counter) != 0) {
            int fail = -1;
            write(result_pipe[1], &fail, sizeof(fail));
            _exit(2);
        }

        char ready = 1;
        write(ready_pipe[1], &ready, sizeof(ready));
        close(ready_pipe[1]);

        for (int i = 0; i < 20; i++) {
            usleep(50000);
            pthread_mutex_lock(&counter.mutex);
            int done = counter.count;
            pthread_mutex_unlock(&counter.mutex);
            if (done >= 2) break;
        }

        int result[2];
        pthread_mutex_lock(&counter.mutex);
        result[0] = counter.count;
        result[1] = counter.last_value;
        pthread_mutex_unlock(&counter.mutex);
        write(result_pipe[1], result, sizeof(result));
        close(result_pipe[1]);
        transport_destroy(sub_transport);
        message_bus_destroy(sub_bus);
        _exit(0);
    }

    close(ready_pipe[1]);
    close(result_pipe[1]);
    char ready = 0;
    ASSERT(read(ready_pipe[0], &ready, sizeof(ready)) == (ssize_t)sizeof(ready) && ready == 1,
           "child subscriber not ready");
    close(ready_pipe[0]);

    BagReader* reader = bag_reader_open(path);
    ASSERT(reader != NULL, "reader open failed");
    ReplayTransportCtx ctx = { .transport = pub_transport };
    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.publish_fn = transport_publish_cb;
    options.publish_user_data = &ctx;

    uint64_t played = 0;
    int rc = bag_reader_play_with_options(reader, &options, &played);
    ASSERT(rc == ERR_OK, "IPC replay failed rc=%d", rc);
    ASSERT(played == 2, "expected 2 IPC replayed messages, got %llu",
           (unsigned long long)played);
    bag_reader_close(reader);

    int result[2] = {0, 0};
    ASSERT(read(result_pipe[0], result, sizeof(result)) == (ssize_t)sizeof(result),
           "failed to read child result");
    close(result_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child subscriber exit=%d", WEXITSTATUS(status));
    ASSERT(result[0] == 2,
           "subscriber saw count=%d last=%d", result[0], result[1]);
#endif

    transport_destroy(pub_transport);
    message_bus_destroy(pub_bus);
    PASS();
}

static void test_flowctl_play_validation(void) {
    TEST("flowctl bag play validates arguments");
    ensure_artifact_dir();
    const char* path = ARTIFACT_DIR "/window.bag";
    char output[2048];

    char* bad_window_argv[] = {
        FLOWCTL_TEST_BIN,
        "bag",
        "play",
        (char*)path,
        "--start", "2.0",
        "--end", "1.0",
        NULL
    };
    int rc = run_flowctl_capture(bad_window_argv, output, sizeof(output));
    ASSERT(rc != 0, "expected flowctl validation failure");
    ASSERT(strstr(output, "--end must be >= --start") != NULL,
           "missing end/start validation message: %s", output);

    char* attach_argv[] = {
        FLOWCTL_TEST_BIN,
        "bag",
        "play",
        (char*)path,
        "--attach",
        NULL
    };
    rc = run_flowctl_capture(attach_argv, output, sizeof(output));
    ASSERT(rc != 0, "expected attach mode to fail");
    ASSERT(strstr(output, "already-running pipeline is unsupported") != NULL,
           "missing unsupported attach message: %s", output);
    PASS();
}

int main(void) {
    cleanup_artifacts();
    ensure_artifact_dir();
    test_replay_window_and_filter();
    test_replay_loop_order();
    test_replay_rate_timing();
    test_replay_ipc_transport_boundary();
    test_flowctl_play_validation();
    cleanup_artifacts();

    if (g_failed != 0) {
        fprintf(stderr, "bag replay tests failed: %d\n", g_failed);
        return 1;
    }
    printf("bag replay tests passed: %d\n", g_passed);
    return 0;
}
