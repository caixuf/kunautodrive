#include "bag.h"
#include "clock_service.h"
#include "flowrec.h"

#include <cjson/cJSON.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARTIFACT_DIR "flowrec_test_artifacts"

static int g_failures;

#define CHECK(condition, format, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: " format "\n", ##__VA_ARGS__); \
            g_failures++; \
            return; \
        } \
    } while (0)

static void cleanup_artifacts(void) {
    DIR* directory = opendir(ARTIFACT_DIR);
    if (directory) {
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", ARTIFACT_DIR, entry->d_name);
            unlink(path);
        }
        closedir(directory);
    }
    rmdir(ARTIFACT_DIR);
}

static void make_message(Message* message, const char* topic,
                         const void* payload, uint32_t payload_size) {
    memset(message, 0, sizeof(*message));
    snprintf(message->topic, sizeof(message->topic), "%s", topic);
    message->timestamp_us = clock_now_us();
    message->type_id = 0x12345678u;
    message->schema_version = 1;
    message->endian_marker = 0x12;
    message->data_size = payload_size;
    if (payload_size > 0) memcpy(message->data, payload, payload_size);
}

static void process_bool_message(FlowrecEngine* engine, int collector,
                                 bool emergency_brake) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "emergency_brake", emergency_brake);
    char* json = cJSON_PrintUnformatted(root);
    Message message;
    make_message(&message, "control/cmd", json, (uint32_t)strlen(json) + 1);
    (void)flowrec_engine_process(engine, collector, &message);
    free(json);
    cJSON_Delete(root);
}

static void assert_bag_count(const char* path, uint64_t expected_messages) {
    BagReader* reader = bag_reader_open(path);
    CHECK(reader != NULL, "unable to open %s", path);
    uint64_t message_count = 0;
    CHECK(bag_reader_info(reader, &message_count, NULL) == 0,
          "unable to inspect %s", path);
    bag_reader_close(reader);
    CHECK(message_count == expected_messages, "%s contains %llu messages, expected %llu",
          path, (unsigned long long)message_count,
          (unsigned long long)expected_messages);
}

static void test_always_on_records_bag_v2(void) {
    const char* config =
        "{\"collectors\":[{\"name\":\"always\",\"topics\":[\"sensor/gps\"],"
        "\"output\":\"flowrec_test_artifacts/always.bag\",\"max_size_bytes\":262144,"
        "\"trigger\":\"always_on\"}]}";
    char error[FLOWREC_ERROR_LEN] = "";
    FlowrecEngine* engine = flowrec_engine_create_from_json(config, error, sizeof(error));
    CHECK(engine != NULL, "always_on config rejected: %s", error);

    Message first;
    Message second;
    make_message(&first, "sensor/gps", "first", 6);
    make_message(&second, "sensor/gps", "second", 7);
    CHECK(flowrec_engine_process(engine, 0, &first) == 0, "first record failed");
    CHECK(flowrec_engine_process(engine, 0, &second) == 0, "second record failed");

    FlowrecCollectorStatus status;
    CHECK(flowrec_engine_get_status(engine, 0, &status) == 0, "status unavailable");
    CHECK(status.messages_written == 2, "always_on wrote %llu records",
          (unsigned long long)status.messages_written);
    char output[FLOWREC_OUTPUT_PATH_LEN];
    snprintf(output, sizeof(output), "%s", status.last_output);
    flowrec_engine_destroy(engine);

    assert_bag_count(output, 2);
    printf("PASS: always_on Bag v2 recording\n");
}

static void test_event_pre_and_post_buffer(void) {
    const char* config =
        "{\"collectors\":[{\"name\":\"event\",\"topics\":[\"fusion/state\",\"control/cmd\"],"
        "\"output\":\"flowrec_test_artifacts/event.bag\",\"max_size_bytes\":262144,"
        "\"trigger\":{\"type\":\"topic_value\",\"topic\":\"control/cmd\","
        "\"field\":\"emergency_brake\",\"equals\":true},"
        "\"pre_buffer_sec\":5,\"pre_buffer_mb\":1,\"post_buffer_sec\":0.02}]}";
    char error[FLOWREC_ERROR_LEN] = "";
    FlowrecEngine* engine = flowrec_engine_create_from_json(config, error, sizeof(error));
    CHECK(engine != NULL, "event config rejected: %s", error);

    Message pre;
    Message post;
    make_message(&pre, "fusion/state", "pre", 4);
    make_message(&post, "fusion/state", "post", 5);
    CHECK(flowrec_engine_process(engine, 0, &pre) == 0, "pre-buffer record failed");
    process_bool_message(engine, 0, false);
    process_bool_message(engine, 0, true);
    CHECK(flowrec_engine_process(engine, 0, &post) == 0, "post-trigger record failed");
    process_bool_message(engine, 0, true);
    usleep(30000);
    flowrec_engine_tick(engine, 0);

    FlowrecCollectorStatus status;
    CHECK(flowrec_engine_get_status(engine, 0, &status) == 0, "event status unavailable");
    CHECK(status.triggers == 2, "expected two triggers, got %llu",
          (unsigned long long)status.triggers);
    CHECK(!status.recording_active, "event collector should close after post buffer");
    char output[FLOWREC_OUTPUT_PATH_LEN];
    snprintf(output, sizeof(output), "%s", status.last_output);

    char* status_json = flowrec_engine_status_json(engine);
    cJSON* status_root = cJSON_Parse(status_json);
    CHECK(cJSON_IsObject(status_root), "status must be valid JSON");
    cJSON* healthy = cJSON_GetObjectItemCaseSensitive(status_root, "healthy");
    CHECK(cJSON_IsTrue(healthy), "status should report healthy");
    cJSON_Delete(status_root);
    free(status_json);
    flowrec_engine_destroy(engine);

    assert_bag_count(output, 5);
    printf("PASS: event pre/post buffer recording\n");
}

static void test_size_rotation(void) {
    const char* config =
        "{\"collectors\":[{\"name\":\"rotate\",\"topics\":[\"sensor/lidar\"],"
        "\"output\":\"flowrec_test_artifacts/rotate.bag\",\"max_size_bytes\":300000,"
        "\"trigger\":\"always_on\"}]}";
    char error[FLOWREC_ERROR_LEN] = "";
    FlowrecEngine* engine = flowrec_engine_create_from_json(config, error, sizeof(error));
    CHECK(engine != NULL, "rotation config rejected: %s", error);

    uint8_t payload[MSG_BUS_MAX_DATA_SIZE];
    memset(payload, 0xA5, sizeof(payload));
    for (int i = 0; i < 8; ++i) {
        Message message;
        make_message(&message, "sensor/lidar", payload, sizeof(payload));
        CHECK(flowrec_engine_process(engine, 0, &message) == 0,
              "rotation record %d failed", i);
    }
    FlowrecCollectorStatus status;
    CHECK(flowrec_engine_get_status(engine, 0, &status) == 0, "rotation status unavailable");
    CHECK(status.rotations >= 1, "size threshold did not rotate");
    flowrec_engine_destroy(engine);

    DIR* directory = opendir(ARTIFACT_DIR);
    CHECK(directory != NULL, "artifact directory missing");
    int bag_count = 0;
    uint64_t total_messages = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "rotate_", 7) != 0) continue;
        char path[FLOWREC_OUTPUT_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", ARTIFACT_DIR, entry->d_name);
        BagReader* reader = bag_reader_open(path);
        CHECK(reader != NULL, "unable to open rotated bag %s", path);
        uint64_t message_count = 0;
        CHECK(bag_reader_info(reader, &message_count, NULL) == 0,
              "unable to inspect rotated bag");
        bag_reader_close(reader);
        total_messages += message_count;
        bag_count++;
    }
    closedir(directory);
    CHECK(bag_count >= 2, "expected at least two rotated bags, got %d", bag_count);
    CHECK(total_messages == 8, "rotated bags contain %llu records",
          (unsigned long long)total_messages);
    printf("PASS: size-based rotation\n");
}

static void test_multiple_collectors_and_ring_wrap(void) {
    const char* config =
        "{\"collectors\":["
        "{\"name\":\"left\",\"topics\":[\"sensor/left\"],"
        "\"output\":\"flowrec_test_artifacts/left.bag\",\"max_size_bytes\":2097152,"
        "\"trigger\":\"always_on\"},"
        "{\"name\":\"right\",\"topics\":[\"sensor/right\"],"
        "\"output\":\"flowrec_test_artifacts/right.bag\",\"max_size_bytes\":262144,"
        "\"trigger\":\"always_on\"}]}";
    char error[FLOWREC_ERROR_LEN] = "";
    FlowrecEngine* engine = flowrec_engine_create_from_json(config, error, sizeof(error));
    CHECK(engine != NULL, "multi-collector config rejected: %s", error);

    uint8_t payload[MSG_BUS_MAX_DATA_SIZE];
    memset(payload, 0x5A, sizeof(payload));
    for (int i = 0; i < 18; ++i) {
        Message message;
        make_message(&message, "sensor/left", payload, sizeof(payload));
        CHECK(flowrec_engine_process(engine, 0, &message) == 0,
              "wrapped-ring record %d failed", i);
    }
    Message right;
    make_message(&right, "sensor/right", "right", 6);
    CHECK(flowrec_engine_process(engine, 1, &right) == 0, "second writer record failed");

    FlowrecCollectorStatus left;
    FlowrecCollectorStatus right_status;
    CHECK(flowrec_engine_get_status(engine, 0, &left) == 0, "left status unavailable");
    CHECK(flowrec_engine_get_status(engine, 1, &right_status) == 0, "right status unavailable");
    char left_output[FLOWREC_OUTPUT_PATH_LEN];
    char right_output[FLOWREC_OUTPUT_PATH_LEN];
    snprintf(left_output, sizeof(left_output), "%s", left.last_output);
    snprintf(right_output, sizeof(right_output), "%s", right_status.last_output);
    flowrec_engine_destroy(engine);

    assert_bag_count(left_output, 18);
    assert_bag_count(right_output, 1);
    printf("PASS: concurrent collectors and Bag ring wrap\n");
}

static void test_rejects_unsafe_trigger(void) {
    const char* config =
        "{\"collectors\":[{\"name\":\"invalid\",\"topics\":[\"fusion/state\"],"
        "\"output\":\"flowrec_test_artifacts/invalid.bag\",\"max_size_bytes\":262144,"
        "\"trigger\":{\"type\":\"topic_value\",\"topic\":\"control/cmd\","
        "\"field\":\"emergency_brake\",\"equals\":true}}]}";
    char error[FLOWREC_ERROR_LEN] = "";
    FlowrecEngine* engine = flowrec_engine_create_from_json(config, error, sizeof(error));
    CHECK(engine == NULL, "trigger topic outside configured topics was accepted");
    CHECK(error[0] != '\0', "rejected configuration should explain the error");
    printf("PASS: config validation\n");
}

int main(void) {
    cleanup_artifacts();
    test_always_on_records_bag_v2();
    test_event_pre_and_post_buffer();
    test_size_rotation();
    test_multiple_collectors_and_ring_wrap();
    test_rejects_unsafe_trigger();
    cleanup_artifacts();
    if (g_failures != 0) {
        fprintf(stderr, "flowrec tests failed: %d\n", g_failures);
        return 1;
    }
    printf("flowrec tests passed\n");
    return 0;
}
