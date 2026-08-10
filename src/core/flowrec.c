#include "flowrec.h"

#include "bag.h"
#include "clock_service.h"
#include "error_codes.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define FLOWREC_MIN_BAG_BYTES (256ULL * 1024ULL)
#define FLOWREC_DEFAULT_BAG_BYTES (1024ULL * 1024ULL * 1024ULL)
#define FLOWREC_MAX_BAG_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define FLOWREC_DEFAULT_PREBUFFER_BYTES (32ULL * 1024ULL * 1024ULL)
#define FLOWREC_MIN_PREBUFFER_BYTES (128ULL * 1024ULL)
#define FLOWREC_MAX_PREBUFFER_BYTES (512ULL * 1024ULL * 1024ULL)
#define FLOWREC_BAG_HEADER_BYTES 64ULL
#define FLOWREC_BAG_INDEX_FIXED_BYTES 12ULL
#define FLOWREC_BAG_INDEX_ENTRY_BYTES 93ULL

typedef enum {
    FLOWREC_TRIGGER_ALWAYS_ON,
    FLOWREC_TRIGGER_TOPIC_VALUE,
} FlowrecTriggerType;

typedef enum {
    FLOWREC_EXPECT_BOOL,
    FLOWREC_EXPECT_NUMBER,
    FLOWREC_EXPECT_STRING,
} FlowrecExpectedType;

typedef struct FlowrecBufferedRecord {
    struct FlowrecBufferedRecord* next;
    uint64_t inserted_us;
    uint64_t timestamp_us;
    uint32_t type_id;
    uint8_t schema_version;
    uint8_t endian_marker;
    uint32_t data_size;
    char topic[MSG_BUS_MAX_TOPIC_LEN];
    uint8_t* data;
} FlowrecBufferedRecord;

typedef struct {
    char name[FLOWREC_NAME_LEN];
    char topics[FLOWREC_MAX_TOPICS_PER_COLLECTOR][MSG_BUS_MAX_TOPIC_LEN];
    int topic_count;
    char output_template[FLOWREC_OUTPUT_PATH_LEN];
    uint64_t max_size_bytes;
    uint64_t rotation_us;
    uint64_t pre_buffer_us;
    uint64_t post_buffer_us;
    uint64_t pre_buffer_max_bytes;
    FlowrecTriggerType trigger_type;
    char trigger_topic[MSG_BUS_MAX_TOPIC_LEN];
    char trigger_field[64];
    FlowrecExpectedType expected_type;
    bool expected_bool;
    double expected_number;
    char expected_string[128];

    BagWriter* writer;
    uint64_t opened_us;
    uint64_t estimated_file_bytes;
    uint64_t messages_in_file;
    char indexed_topics[FLOWREC_MAX_TOPICS_PER_COLLECTOR][MSG_BUS_MAX_TOPIC_LEN];
    int indexed_topic_count;
    unsigned int sequence;
    bool active;
    uint64_t post_until_us;

    FlowrecBufferedRecord* pre_head;
    FlowrecBufferedRecord* pre_tail;
    uint64_t pre_bytes;
    uint64_t pre_count;

    uint64_t messages_written;
    uint64_t messages_dropped;
    uint64_t prebuffer_dropped;
    uint64_t triggers;
    uint64_t rotations;
    uint64_t write_errors;
    char last_output[FLOWREC_OUTPUT_PATH_LEN];
    char last_error[FLOWREC_ERROR_LEN];
} FlowrecCollector;

struct FlowrecEngine {
    pthread_mutex_t mutex;
    int collector_count;
    FlowrecCollector collectors[FLOWREC_MAX_COLLECTORS];
};

static void copy_string(char* dest, size_t dest_size, const char* source) {
    if (!dest || dest_size == 0) return;
    snprintf(dest, dest_size, "%s", source ? source : "");
}

static uint64_t buffered_record_bytes(const FlowrecBufferedRecord* record) {
    return record ? (uint64_t)sizeof(*record) + (uint64_t)record->data_size : 0;
}

static void write_error(char* error, size_t error_size, const char* message) {
    if (error && error_size > 0) snprintf(error, error_size, "%s", message);
}

static void set_collector_error(FlowrecCollector* collector, const char* message) {
    if (collector) copy_string(collector->last_error,
                               sizeof(collector->last_error), message);
}

static int json_string_copy(const cJSON* object, const char* key,
                            char* out, size_t out_size, bool required,
                            char* error, size_t error_size) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring || value->valuestring[0] == '\0') {
        if (required) {
            char detail[FLOWREC_ERROR_LEN];
            snprintf(detail, sizeof(detail), "collector.%s must be a non-empty string", key);
            write_error(error, error_size, detail);
            return -1;
        }
        return 0;
    }
    if (strnlen(value->valuestring, out_size) >= out_size) {
        char detail[FLOWREC_ERROR_LEN];
        snprintf(detail, sizeof(detail), "collector.%s is too long", key);
        write_error(error, error_size, detail);
        return -1;
    }
    copy_string(out, out_size, value->valuestring);
    return 1;
}

static bool topic_is_configured(const FlowrecCollector* collector, const char* topic) {
    if (!collector || !topic) return false;
    for (int i = 0; i < collector->topic_count; ++i) {
        if (strcmp(collector->topics[i], topic) == 0) return true;
    }
    return false;
}

static int parse_topics(FlowrecCollector* collector, const cJSON* object,
                        char* error, size_t error_size) {
    cJSON* topics = cJSON_GetObjectItemCaseSensitive(object, "topics");
    if (!cJSON_IsArray(topics) || cJSON_GetArraySize(topics) <= 0) {
        write_error(error, error_size, "collector.topics must be a non-empty array");
        return -1;
    }
    int count = cJSON_GetArraySize(topics);
    if (count > FLOWREC_MAX_TOPICS_PER_COLLECTOR) {
        write_error(error, error_size, "collector.topics exceeds the supported limit");
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        cJSON* topic = cJSON_GetArrayItem(topics, i);
        if (!cJSON_IsString(topic) || !topic->valuestring || topic->valuestring[0] == '\0' ||
            strnlen(topic->valuestring, MSG_BUS_MAX_TOPIC_LEN) >= MSG_BUS_MAX_TOPIC_LEN) {
            write_error(error, error_size, "collector.topics contains an invalid topic");
            return -1;
        }
        for (int j = 0; j < i; ++j) {
            if (strcmp(collector->topics[j], topic->valuestring) == 0) {
                write_error(error, error_size, "collector.topics contains a duplicate topic");
                return -1;
            }
        }
        copy_string(collector->topics[i], sizeof(collector->topics[i]), topic->valuestring);
    }
    collector->topic_count = count;
    return 0;
}

static int parse_bounded_seconds(const cJSON* object, const char* key,
                                 double default_seconds, double max_seconds,
                                 uint64_t* out, char* error, size_t error_size) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    double seconds = default_seconds;
    if (value) {
        if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
            value->valuedouble > max_seconds) {
            char detail[FLOWREC_ERROR_LEN];
            snprintf(detail, sizeof(detail), "collector.%s must be between 0 and %.0f",
                     key, max_seconds);
            write_error(error, error_size, detail);
            return -1;
        }
        seconds = value->valuedouble;
    }
    *out = (uint64_t)(seconds * 1000000.0);
    return 0;
}

static int parse_max_size(FlowrecCollector* collector, const cJSON* object,
                          char* error, size_t error_size) {
    cJSON* bytes = cJSON_GetObjectItemCaseSensitive(object, "max_size_bytes");
    cJSON* mb = cJSON_GetObjectItemCaseSensitive(object, "max_size_mb");
    double value = (double)FLOWREC_DEFAULT_BAG_BYTES;
    if (bytes && mb) {
        write_error(error, error_size, "collector must set only one of max_size_bytes/max_size_mb");
        return -1;
    }
    if (bytes) {
        if (!cJSON_IsNumber(bytes)) {
            write_error(error, error_size, "collector.max_size_bytes must be numeric");
            return -1;
        }
        value = bytes->valuedouble;
    } else if (mb) {
        if (!cJSON_IsNumber(mb)) {
            write_error(error, error_size, "collector.max_size_mb must be numeric");
            return -1;
        }
        value = mb->valuedouble * 1024.0 * 1024.0;
    }
    if (value < (double)FLOWREC_MIN_BAG_BYTES || value > (double)FLOWREC_MAX_BAG_BYTES) {
        write_error(error, error_size, "collector maximum bag size is outside the supported range");
        return -1;
    }
    collector->max_size_bytes = (uint64_t)value;
    return 0;
}

static int parse_rotation(FlowrecCollector* collector, const cJSON* object,
                          char* error, size_t error_size) {
    cJSON* seconds = cJSON_GetObjectItemCaseSensitive(object, "rotation_sec");
    cJSON* named = cJSON_GetObjectItemCaseSensitive(object, "rotation");
    if (seconds && named) {
        write_error(error, error_size, "collector must set only one of rotation_sec/rotation");
        return -1;
    }
    if (seconds) {
        return parse_bounded_seconds(object, "rotation_sec", 0.0, 604800.0,
                                     &collector->rotation_us, error, error_size);
    }
    if (!named) {
        collector->rotation_us = 0;
        return 0;
    }
    if (!cJSON_IsString(named) || !named->valuestring) {
        write_error(error, error_size, "collector.rotation must be a string");
        return -1;
    }
    if (strcmp(named->valuestring, "none") == 0) collector->rotation_us = 0;
    else if (strcmp(named->valuestring, "hourly") == 0) collector->rotation_us = 3600ULL * 1000000ULL;
    else if (strcmp(named->valuestring, "daily") == 0) collector->rotation_us = 86400ULL * 1000000ULL;
    else {
        write_error(error, error_size, "collector.rotation must be none, hourly, or daily");
        return -1;
    }
    return 0;
}

static int parse_expected_value(FlowrecCollector* collector, const cJSON* trigger,
                                char* error, size_t error_size) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(trigger, "equals");
    if (cJSON_IsBool(value)) {
        collector->expected_type = FLOWREC_EXPECT_BOOL;
        collector->expected_bool = cJSON_IsTrue(value);
        return 0;
    }
    if (cJSON_IsNumber(value)) {
        collector->expected_type = FLOWREC_EXPECT_NUMBER;
        collector->expected_number = value->valuedouble;
        return 0;
    }
    if (cJSON_IsString(value) && value->valuestring &&
        strnlen(value->valuestring, sizeof(collector->expected_string)) <
            sizeof(collector->expected_string)) {
        collector->expected_type = FLOWREC_EXPECT_STRING;
        copy_string(collector->expected_string, sizeof(collector->expected_string),
                    value->valuestring);
        return 0;
    }
    write_error(error, error_size,
                "collector.trigger.equals must be a boolean, number, or short string");
    return -1;
}

static int parse_trigger(FlowrecCollector* collector, const cJSON* object,
                         char* error, size_t error_size) {
    cJSON* trigger = cJSON_GetObjectItemCaseSensitive(object, "trigger");
    if (cJSON_IsString(trigger) && trigger->valuestring &&
        strcmp(trigger->valuestring, "always_on") == 0) {
        collector->trigger_type = FLOWREC_TRIGGER_ALWAYS_ON;
        return 0;
    }
    if (!cJSON_IsObject(trigger)) {
        write_error(error, error_size,
                    "collector.trigger must be \"always_on\" or a topic_value object");
        return -1;
    }

    char type[32];
    if (json_string_copy(trigger, "type", type, sizeof(type), true, error, error_size) < 0 ||
        strcmp(type, "topic_value") != 0) {
        if (error && error[0] == '\0')
            write_error(error, error_size, "collector.trigger.type must be topic_value");
        return -1;
    }
    collector->trigger_type = FLOWREC_TRIGGER_TOPIC_VALUE;
    if (json_string_copy(trigger, "topic", collector->trigger_topic,
                         sizeof(collector->trigger_topic), true, error, error_size) < 0 ||
        json_string_copy(trigger, "field", collector->trigger_field,
                         sizeof(collector->trigger_field), true, error, error_size) < 0 ||
        parse_expected_value(collector, trigger, error, error_size) != 0) {
        return -1;
    }
    if (!topic_is_configured(collector, collector->trigger_topic)) {
        write_error(error, error_size,
                    "collector.trigger.topic must also appear in collector.topics");
        return -1;
    }
    return 0;
}

static int parse_prebuffer(FlowrecCollector* collector, const cJSON* object,
                           char* error, size_t error_size) {
    if (collector->trigger_type == FLOWREC_TRIGGER_ALWAYS_ON) {
        collector->pre_buffer_us = 0;
        collector->post_buffer_us = 0;
        collector->pre_buffer_max_bytes = 0;
        return 0;
    }
    if (parse_bounded_seconds(object, "pre_buffer_sec", 0.0, 300.0,
                              &collector->pre_buffer_us, error, error_size) != 0 ||
        parse_bounded_seconds(object, "post_buffer_sec", 0.0, 600.0,
                              &collector->post_buffer_us, error, error_size) != 0) {
        return -1;
    }
    cJSON* mb = cJSON_GetObjectItemCaseSensitive(object, "pre_buffer_mb");
    double bytes = (double)FLOWREC_DEFAULT_PREBUFFER_BYTES;
    if (mb) {
        if (!cJSON_IsNumber(mb)) {
            write_error(error, error_size, "collector.pre_buffer_mb must be numeric");
            return -1;
        }
        bytes = mb->valuedouble * 1024.0 * 1024.0;
    }
    if (collector->pre_buffer_us == 0) {
        collector->pre_buffer_max_bytes = 0;
        return 0;
    }
    if (bytes < (double)FLOWREC_MIN_PREBUFFER_BYTES ||
        bytes > (double)FLOWREC_MAX_PREBUFFER_BYTES) {
        write_error(error, error_size, "collector.pre_buffer_mb is outside the supported range");
        return -1;
    }
    collector->pre_buffer_max_bytes = (uint64_t)bytes;
    return 0;
}

static int parse_collector(FlowrecCollector* collector, const cJSON* object,
                           int index, char* error, size_t error_size) {
    if (!cJSON_IsObject(object)) {
        write_error(error, error_size, "collectors entries must be objects");
        return -1;
    }
    if (json_string_copy(object, "name", collector->name, sizeof(collector->name),
                         false, error, error_size) < 0) {
        return -1;
    }
    if (collector->name[0] == '\0')
        snprintf(collector->name, sizeof(collector->name), "collector_%d", index);
    if (parse_topics(collector, object, error, error_size) != 0 ||
        json_string_copy(object, "output", collector->output_template,
                         sizeof(collector->output_template), true, error, error_size) < 0 ||
        parse_max_size(collector, object, error, error_size) != 0 ||
        parse_rotation(collector, object, error, error_size) != 0 ||
        parse_trigger(collector, object, error, error_size) != 0 ||
        parse_prebuffer(collector, object, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

FlowrecEngine* flowrec_engine_create_from_json(const char* params_json,
                                               char* error, size_t error_size) {
    write_error(error, error_size, "");
    if (!params_json) {
        write_error(error, error_size, "flowrec params JSON is required");
        return NULL;
    }
    cJSON* root = cJSON_Parse(params_json);
    if (!cJSON_IsObject(root)) {
        write_error(error, error_size, "flowrec params must be a JSON object");
        cJSON_Delete(root);
        return NULL;
    }
    cJSON* collectors = cJSON_GetObjectItemCaseSensitive(root, "collectors");
    int count = cJSON_IsArray(collectors) ? cJSON_GetArraySize(collectors) : 0;
    if (count <= 0 || count > FLOWREC_MAX_COLLECTORS) {
        write_error(error, error_size, "collectors must contain 1 to 8 entries");
        cJSON_Delete(root);
        return NULL;
    }

    FlowrecEngine* engine = (FlowrecEngine*)calloc(1, sizeof(*engine));
    if (!engine) {
        write_error(error, error_size, "flowrec allocation failed");
        cJSON_Delete(root);
        return NULL;
    }
    pthread_mutex_init(&engine->mutex, NULL);
    engine->collector_count = count;
    for (int i = 0; i < count; ++i) {
        if (parse_collector(&engine->collectors[i], cJSON_GetArrayItem(collectors, i),
                            i, error, error_size) != 0) {
            cJSON_Delete(root);
            flowrec_engine_destroy(engine);
            return NULL;
        }
        for (int j = 0; j < i; ++j) {
            if (strcmp(engine->collectors[i].name, engine->collectors[j].name) == 0) {
                write_error(error, error_size, "collector names must be unique");
                cJSON_Delete(root);
                flowrec_engine_destroy(engine);
                return NULL;
            }
        }
    }
    cJSON_Delete(root);
    return engine;
}

static void free_prebuffer(FlowrecCollector* collector) {
    FlowrecBufferedRecord* record = collector->pre_head;
    while (record) {
        FlowrecBufferedRecord* next = record->next;
        free(record->data);
        free(record);
        record = next;
    }
    collector->pre_head = NULL;
    collector->pre_tail = NULL;
    collector->pre_bytes = 0;
    collector->pre_count = 0;
}

static void close_writer(FlowrecCollector* collector) {
    if (!collector->writer) return;
    bag_writer_close(collector->writer);
    collector->writer = NULL;
    collector->opened_us = 0;
    collector->estimated_file_bytes = 0;
    collector->messages_in_file = 0;
    collector->indexed_topic_count = 0;
}

void flowrec_engine_destroy(FlowrecEngine* engine) {
    if (!engine) return;
    pthread_mutex_lock(&engine->mutex);
    for (int i = 0; i < engine->collector_count; ++i) {
        close_writer(&engine->collectors[i]);
        free_prebuffer(&engine->collectors[i]);
    }
    pthread_mutex_unlock(&engine->mutex);
    pthread_mutex_destroy(&engine->mutex);
    free(engine);
}

int flowrec_engine_collector_count(const FlowrecEngine* engine) {
    return engine ? engine->collector_count : 0;
}

int flowrec_engine_get_topics(const FlowrecEngine* engine, int collector_index,
                              char topics[][MSG_BUS_MAX_TOPIC_LEN], int max_topics) {
    if (!engine || collector_index < 0 || collector_index >= engine->collector_count)
        return ERR_INVALID_PARAM;
    const FlowrecCollector* collector = &engine->collectors[collector_index];
    if (topics && max_topics > 0) {
        int copied = collector->topic_count < max_topics ? collector->topic_count : max_topics;
        for (int i = 0; i < copied; ++i)
            copy_string(topics[i], MSG_BUS_MAX_TOPIC_LEN, collector->topics[i]);
    }
    return collector->topic_count;
}

static int ensure_directory(const char* path) {
    char directory[FLOWREC_OUTPUT_PATH_LEN];
    copy_string(directory, sizeof(directory), path);
    char* slash = strrchr(directory, '/');
    if (!slash) return 0;
    if (slash == directory) {
        directory[1] = '\0';
        return 0;
    }
    *slash = '\0';
    if (directory[0] == '\0') return 0;

    for (char* p = directory + 1; ; ++p) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        if (directory[0] != '\0' && mkdir(directory, 0750) != 0 && errno != EEXIST)
            return -1;
        if (saved == '\0') break;
        *p = saved;
    }
    struct stat st;
    return stat(directory, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int format_output_path(const FlowrecCollector* collector, unsigned int sequence,
                              char* path, size_t path_size) {
    time_t seconds = (time_t)(clock_now_realtime_us() / 1000000ULL);
    struct tm tm_now;
    if (!localtime_r(&seconds, &tm_now)) return -1;

    char expanded[FLOWREC_OUTPUT_PATH_LEN];
    if (strftime(expanded, sizeof(expanded), collector->output_template, &tm_now) == 0)
        return -1;
    const char* filename = strrchr(expanded, '/');
    filename = filename ? filename + 1 : expanded;
    const char* extension = strrchr(filename, '.');
    int written;
    if (extension) {
        size_t prefix = (size_t)(extension - expanded);
        written = snprintf(path, path_size, "%.*s_%06u%s", (int)prefix, expanded,
                           sequence, extension);
    } else {
        written = snprintf(path, path_size, "%s_%06u.bag", expanded, sequence);
    }
    return written > 0 && (size_t)written < path_size ? 0 : -1;
}

static int open_writer(FlowrecCollector* collector, uint64_t now_us) {
    char path[FLOWREC_OUTPUT_PATH_LEN];
    if (format_output_path(collector, ++collector->sequence, path, sizeof(path)) != 0) {
        set_collector_error(collector, "could not expand output path");
        collector->write_errors++;
        return ERR_IO;
    }
    if (ensure_directory(path) != 0) {
        set_collector_error(collector, "could not create output directory");
        collector->write_errors++;
        return ERR_IO;
    }
    collector->writer = bag_writer_open(path);
    if (!collector->writer) {
        set_collector_error(collector, "could not open Bag v2 output");
        collector->write_errors++;
        return ERR_IO;
    }
    collector->opened_us = now_us;
    collector->estimated_file_bytes = FLOWREC_BAG_HEADER_BYTES;
    collector->messages_in_file = 0;
    collector->indexed_topic_count = 0;
    copy_string(collector->last_output, sizeof(collector->last_output), path);
    collector->last_error[0] = '\0';
    return 0;
}

static bool indexed_topic_exists(const FlowrecCollector* collector, const char* topic) {
    for (int i = 0; i < collector->indexed_topic_count; ++i) {
        if (strcmp(collector->indexed_topics[i], topic) == 0) return true;
    }
    return false;
}

static uint64_t record_size(const Message* message) {
    size_t topic_len = strnlen(message->topic, MSG_BUS_MAX_TOPIC_LEN - 1);
    return 4ULL + 1ULL + 1ULL + 8ULL + 1ULL + (uint64_t)topic_len + 4ULL +
           (uint64_t)message->data_size;
}

static bool should_rotate(const FlowrecCollector* collector, const Message* message,
                          uint64_t now_us) {
    if (!collector->writer || collector->messages_in_file == 0) return false;
    if (collector->rotation_us > 0 && now_us >= collector->opened_us &&
        now_us - collector->opened_us >= collector->rotation_us)
        return true;
    int prospective_topics = collector->indexed_topic_count +
        (indexed_topic_exists(collector, message->topic) ? 0 : 1);
    uint64_t projected = collector->estimated_file_bytes + record_size(message) +
        FLOWREC_BAG_INDEX_FIXED_BYTES +
        (uint64_t)prospective_topics * FLOWREC_BAG_INDEX_ENTRY_BYTES;
    return projected > collector->max_size_bytes;
}

static int write_message(FlowrecCollector* collector, const Message* message,
                         uint64_t now_us) {
    if (!message || message->data_size > MSG_BUS_MAX_DATA_SIZE) return ERR_INVALID_PARAM;
    if (should_rotate(collector, message, now_us)) {
        close_writer(collector);
        collector->rotations++;
    }
    if (!collector->writer && open_writer(collector, now_us) != 0) {
        collector->messages_dropped++;
        return ERR_IO;
    }
    if (bag_writer_write(collector->writer, message) != 0) {
        collector->messages_dropped++;
        collector->write_errors++;
        set_collector_error(collector, "Bag v2 writer dropped a record");
        return ERR_IO;
    }
    if (!indexed_topic_exists(collector, message->topic) &&
        collector->indexed_topic_count < FLOWREC_MAX_TOPICS_PER_COLLECTOR) {
        copy_string(collector->indexed_topics[collector->indexed_topic_count++],
                    MSG_BUS_MAX_TOPIC_LEN, message->topic);
    }
    collector->estimated_file_bytes += record_size(message);
    collector->messages_in_file++;
    collector->messages_written++;
    return 0;
}

static void prebuffer_remove_head(FlowrecCollector* collector) {
    FlowrecBufferedRecord* record = collector->pre_head;
    if (!record) return;
    collector->pre_head = record->next;
    if (!collector->pre_head) collector->pre_tail = NULL;
    collector->pre_bytes -= buffered_record_bytes(record);
    collector->pre_count--;
    free(record->data);
    free(record);
}

static void prebuffer_prune(FlowrecCollector* collector, uint64_t now_us) {
    while (collector->pre_head && now_us >= collector->pre_head->inserted_us &&
           now_us - collector->pre_head->inserted_us > collector->pre_buffer_us) {
        prebuffer_remove_head(collector);
    }
    while (collector->pre_head && collector->pre_bytes > collector->pre_buffer_max_bytes)
        prebuffer_remove_head(collector);
}

static void prebuffer_add(FlowrecCollector* collector, const Message* message,
                          uint64_t now_us) {
    if (collector->pre_buffer_us == 0 || collector->pre_buffer_max_bytes == 0) return;
    const uint8_t* data = (const uint8_t*)message_bus_message_data(message);
    uint64_t storage_bytes = (uint64_t)sizeof(FlowrecBufferedRecord) +
                             (uint64_t)message->data_size;
    if ((message->data_size > 0 && !data) ||
        storage_bytes > collector->pre_buffer_max_bytes) {
        collector->prebuffer_dropped++;
        return;
    }
    FlowrecBufferedRecord* record =
        (FlowrecBufferedRecord*)calloc(1, sizeof(FlowrecBufferedRecord));
    if (!record) {
        collector->prebuffer_dropped++;
        return;
    }
    if (message->data_size > 0) {
        record->data = (uint8_t*)malloc(message->data_size);
        if (!record->data) {
            free(record);
            collector->prebuffer_dropped++;
            return;
        }
        memcpy(record->data, data, message->data_size);
    }
    record->inserted_us = now_us;
    record->timestamp_us = message->timestamp_us;
    record->type_id = message->type_id;
    record->schema_version = message->schema_version;
    record->endian_marker = message->endian_marker;
    record->data_size = message->data_size;
    copy_string(record->topic, sizeof(record->topic), message->topic);
    if (collector->pre_tail) collector->pre_tail->next = record;
    else collector->pre_head = record;
    collector->pre_tail = record;
    collector->pre_bytes += buffered_record_bytes(record);
    collector->pre_count++;
    prebuffer_prune(collector, now_us);
}

static int write_buffered_record(FlowrecCollector* collector,
                                 const FlowrecBufferedRecord* record,
                                 uint64_t now_us) {
    Message message;
    memset(&message, 0, sizeof(message));
    copy_string(message.topic, sizeof(message.topic), record->topic);
    message.timestamp_us = record->timestamp_us;
    message.type_id = record->type_id;
    message.schema_version = record->schema_version;
    message.endian_marker = record->endian_marker;
    message.data_size = record->data_size;
    message._loaned_data = record->data;
    return write_message(collector, &message, now_us);
}

static int flush_prebuffer(FlowrecCollector* collector, uint64_t now_us) {
    int result = 0;
    for (FlowrecBufferedRecord* record = collector->pre_head; record; record = record->next) {
        if (write_buffered_record(collector, record, now_us) != 0) result = ERR_IO;
    }
    return result;
}

static bool trigger_matches(const FlowrecCollector* collector, const Message* message) {
    if (collector->trigger_type != FLOWREC_TRIGGER_TOPIC_VALUE ||
        strcmp(message->topic, collector->trigger_topic) != 0 ||
        message->data_size == 0 || message->data_size > MSG_BUS_MAX_DATA_SIZE)
        return false;

    const uint8_t* data = (const uint8_t*)message_bus_message_data(message);
    if (!data) return false;
    char* json_text = (char*)malloc((size_t)message->data_size + 1);
    if (!json_text) return false;
    memcpy(json_text, data, message->data_size);
    json_text[message->data_size] = '\0';
    cJSON* root = cJSON_Parse(json_text);
    free(json_text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, collector->trigger_field);
    bool matches = false;
    if (collector->expected_type == FLOWREC_EXPECT_BOOL)
        matches = cJSON_IsBool(value) && cJSON_IsTrue(value) == collector->expected_bool;
    else if (collector->expected_type == FLOWREC_EXPECT_NUMBER)
        matches = cJSON_IsNumber(value) && value->valuedouble == collector->expected_number;
    else if (collector->expected_type == FLOWREC_EXPECT_STRING)
        matches = cJSON_IsString(value) && value->valuestring &&
                  strcmp(value->valuestring, collector->expected_string) == 0;
    cJSON_Delete(root);
    return matches;
}

static void finish_event_if_due(FlowrecCollector* collector, uint64_t now_us) {
    if (collector->trigger_type == FLOWREC_TRIGGER_TOPIC_VALUE && collector->active &&
        now_us >= collector->post_until_us) {
        close_writer(collector);
        collector->active = false;
    }
}

int flowrec_engine_process(FlowrecEngine* engine, int collector_index,
                           const Message* message) {
    if (!engine || !message || collector_index < 0 ||
        collector_index >= engine->collector_count)
        return ERR_INVALID_PARAM;
    if (strnlen(message->topic, MSG_BUS_MAX_TOPIC_LEN) >= MSG_BUS_MAX_TOPIC_LEN)
        return ERR_INVALID_PARAM;
    if (!topic_is_configured(&engine->collectors[collector_index], message->topic))
        return ERR_INVALID_PARAM;

    uint64_t now_us = clock_now_us();
    pthread_mutex_lock(&engine->mutex);
    FlowrecCollector* collector = &engine->collectors[collector_index];
    finish_event_if_due(collector, now_us);
    int result = 0;

    if (collector->trigger_type == FLOWREC_TRIGGER_ALWAYS_ON) {
        result = write_message(collector, message, now_us);
    } else {
        prebuffer_prune(collector, now_us);
        prebuffer_add(collector, message, now_us);
        if (trigger_matches(collector, message)) {
            collector->triggers++;
            if (!collector->active) {
                collector->active = true;
                result = collector->pre_buffer_us > 0
                    ? flush_prebuffer(collector, now_us)
                    : write_message(collector, message, now_us);
            } else {
                result = write_message(collector, message, now_us);
            }
            collector->post_until_us = now_us + collector->post_buffer_us;
            if (collector->post_buffer_us == 0) {
                close_writer(collector);
                collector->active = false;
            }
        } else if (collector->active) {
            result = write_message(collector, message, now_us);
        }
    }
    pthread_mutex_unlock(&engine->mutex);
    return result;
}

void flowrec_engine_tick(FlowrecEngine* engine, uint64_t now_us) {
    if (!engine) return;
    if (now_us == 0) now_us = clock_now_us();
    pthread_mutex_lock(&engine->mutex);
    for (int i = 0; i < engine->collector_count; ++i) {
        FlowrecCollector* collector = &engine->collectors[i];
        prebuffer_prune(collector, now_us);
        if (collector->trigger_type == FLOWREC_TRIGGER_TOPIC_VALUE) {
            finish_event_if_due(collector, now_us);
        } else if (collector->writer && collector->messages_in_file > 0 &&
                   collector->rotation_us > 0 && now_us >= collector->opened_us &&
                   now_us - collector->opened_us >= collector->rotation_us) {
            close_writer(collector);
            collector->rotations++;
        }
    }
    pthread_mutex_unlock(&engine->mutex);
}

int flowrec_engine_get_status(const FlowrecEngine* engine, int collector_index,
                              FlowrecCollectorStatus* status) {
    if (!engine || !status || collector_index < 0 ||
        collector_index >= engine->collector_count)
        return ERR_INVALID_PARAM;
    FlowrecEngine* mutable_engine = (FlowrecEngine*)engine;
    pthread_mutex_lock(&mutable_engine->mutex);
    const FlowrecCollector* collector = &engine->collectors[collector_index];
    memset(status, 0, sizeof(*status));
    copy_string(status->name, sizeof(status->name), collector->name);
    status->event_triggered = collector->trigger_type == FLOWREC_TRIGGER_TOPIC_VALUE;
    status->recording_active = collector->trigger_type == FLOWREC_TRIGGER_ALWAYS_ON
        ? collector->writer != NULL : collector->active;
    status->messages_written = collector->messages_written;
    status->messages_dropped = collector->messages_dropped;
    status->prebuffer_dropped = collector->prebuffer_dropped;
    status->triggers = collector->triggers;
    status->rotations = collector->rotations;
    status->write_errors = collector->write_errors;
    status->buffered_messages = collector->pre_count;
    status->buffered_bytes = collector->pre_bytes;
    copy_string(status->last_output, sizeof(status->last_output), collector->last_output);
    copy_string(status->last_error, sizeof(status->last_error), collector->last_error);
    pthread_mutex_unlock(&mutable_engine->mutex);
    return 0;
}

char* flowrec_engine_status_json(const FlowrecEngine* engine) {
    if (!engine) return NULL;
    FlowrecEngine* mutable_engine = (FlowrecEngine*)engine;
    pthread_mutex_lock(&mutable_engine->mutex);
    cJSON* root = cJSON_CreateObject();
    cJSON* collectors = cJSON_AddArrayToObject(root, "collectors");
    if (!root || !collectors) {
        cJSON_Delete(root);
        pthread_mutex_unlock(&mutable_engine->mutex);
        return NULL;
    }
    cJSON_AddStringToObject(root, "type", "flowrec_status");
    cJSON_AddNumberToObject(root, "timestamp_us", (double)clock_now_realtime_us());
    bool healthy = true;
    for (int i = 0; i < engine->collector_count; ++i) {
        const FlowrecCollector* collector = &engine->collectors[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", collector->name);
        cJSON_AddStringToObject(item, "mode",
                                collector->trigger_type == FLOWREC_TRIGGER_ALWAYS_ON
                                    ? "always_on" : "topic_value");
        cJSON_AddBoolToObject(item, "active",
                              collector->trigger_type == FLOWREC_TRIGGER_ALWAYS_ON
                                  ? collector->writer != NULL : collector->active);
        cJSON_AddNumberToObject(item, "messages_written",
                                (double)collector->messages_written);
        cJSON_AddNumberToObject(item, "messages_dropped",
                                (double)collector->messages_dropped);
        cJSON_AddNumberToObject(item, "prebuffer_messages",
                                (double)collector->pre_count);
        cJSON_AddNumberToObject(item, "prebuffer_bytes",
                                (double)collector->pre_bytes);
        cJSON_AddNumberToObject(item, "prebuffer_dropped",
                                (double)collector->prebuffer_dropped);
        cJSON_AddNumberToObject(item, "triggers", (double)collector->triggers);
        cJSON_AddNumberToObject(item, "rotations", (double)collector->rotations);
        cJSON_AddNumberToObject(item, "write_errors", (double)collector->write_errors);
        cJSON_AddStringToObject(item, "last_output", collector->last_output);
        cJSON_AddStringToObject(item, "last_error", collector->last_error);
        cJSON_AddItemToArray(collectors, item);
        if (collector->write_errors > 0) healthy = false;
    }
    cJSON_AddBoolToObject(root, "healthy", healthy);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&mutable_engine->mutex);
    return json;
}

int flowrec_engine_health(const FlowrecEngine* engine) {
    if (!engine) return ERR_INVALID_PARAM;
    FlowrecEngine* mutable_engine = (FlowrecEngine*)engine;
    pthread_mutex_lock(&mutable_engine->mutex);
    int unhealthy = 0;
    for (int i = 0; i < engine->collector_count; ++i) {
        if (engine->collectors[i].write_errors > 0) {
            unhealthy = ERR_IO;
            break;
        }
    }
    pthread_mutex_unlock(&mutable_engine->mutex);
    return unhealthy;
}
