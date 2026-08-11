/*
 * PEM business collector: topic callbacks stay non-blocking, while one
 * FlowCoro task owns aggregation snapshots and the durable PEM writer.
 */
#include "node_plugin.h"
#include "adas_msgs_gen.h"
#include "clock_service.h"
#include "coroutine_task.h"
#include <pem_log.h>
#include <pem_runtime.h>
#include "platform_paths.h"

#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL
#include "logger.h"

#include <cjson/cJSON.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

constexpr double kEarthRadiusM = 6371000.0;
constexpr double kDefaultMaxStepM = 50.0;
constexpr double kDefaultDrivingSpeedMps = 0.3;
enum DistanceSource {
    DISTANCE_NONE = 0,
    DISTANCE_GPS = 1,
    DISTANCE_LOCALIZATION = 2,
};

struct MetricState {
    double distance_m = 0.0;
    double driving_time_s = 0.0;
    double speed_mps = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double accuracy_m = 0.0;
    uint64_t last_fix_us = 0;
    uint64_t last_gps_us = 0;
    uint64_t last_localization_us = 0;
    double last_latitude = 0.0;
    double last_longitude = 0.0;
    double last_x = 0.0;
    double last_y = 0.0;
    bool have_gps = false;
    bool have_localization = false;
    DistanceSource source = DISTANCE_NONE;
};

struct PemTopicBinding {
    uint16_t index = 0;
};

struct PemTopicManager {
    PemRuntimeTopicConfig configs[PEM_RUNTIME_TOPIC_CAPACITY] {};
    PemTopicBinding bindings[PEM_RUNTIME_TOPIC_CAPACITY] {};
    uint32_t count = 0;
};

struct Collector {
    Transport* transport = nullptr;
    DiscoveryManager* discovery = nullptr;
    Scheduler* scheduler = nullptr;
    PemLog log {};
    PemRuntime runtime {};
    PemTopicManager topic_manager {};
    std::mutex mutex;
    MetricState metrics {};
    char region[32] = "unassigned";
    char pem_path[512] {};
    double emit_hz = 1.0;
    double max_step_m = kDefaultMaxStepM;
    double driving_speed_mps = kDefaultDrivingSpeedMps;
    uint64_t reported_dropped_events = 0;
    uint64_t reported_dropped_inputs = 0;
    bool opened = false;
    struct pem_collector_Wrapper* task_wrapper = nullptr;
} g;

static double deg_to_rad(double degrees) {
    return degrees * (3.14159265358979323846 / 180.0);
}

static double haversine_m(double lat_a, double lon_a, double lat_b, double lon_b) {
    const double dlat = deg_to_rad(lat_b - lat_a);
    const double dlon = deg_to_rad(lon_b - lon_a);
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(deg_to_rad(lat_a)) * std::cos(deg_to_rad(lat_b)) *
                     std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

static bool valid_coordinate(double latitude, double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude) &&
           std::abs(latitude) <= 90.0 && std::abs(longitude) <= 180.0;
}

static int topic_kind_from_json(const char* text, uint8_t* kind) {
    if (!text || !kind) return -1;
    if (std::strcmp(text, "gps") == 0) *kind = PEM_PAYLOAD_GPS;
    else if (std::strcmp(text, "localization") == 0) *kind = PEM_PAYLOAD_LOCALIZATION;
    else if (std::strcmp(text, "region") == 0) *kind = PEM_PAYLOAD_REGION;
    else if (std::strcmp(text, "degrade") == 0) *kind = PEM_PAYLOAD_DEGRADE;
    else if (std::strcmp(text, "heartbeat") == 0) *kind = PEM_PAYLOAD_HEARTBEAT;
    else if (std::strcmp(text, "opaque") == 0) *kind = PEM_PAYLOAD_OPAQUE;
    else return -1;
    return 0;
}

static int topic_priority_from_json(const char* text, uint8_t* priority) {
    if (!text || !priority) return -1;
    if (std::strcmp(text, "fault") == 0) *priority = PEM_INPUT_FAULT;
    else if (std::strcmp(text, "metric") == 0) *priority = PEM_INPUT_METRIC;
    else if (std::strcmp(text, "heartbeat") == 0) *priority = PEM_INPUT_HEARTBEAT;
    else return -1;
    return 0;
}

static int topic_manager_add(PemTopicManager* manager, const char* topic,
                             uint8_t kind, uint8_t priority,
                             uint32_t heartbeat_timeout_ms) {
    if (!manager || !topic || !topic[0] ||
        manager->count >= PEM_RUNTIME_TOPIC_CAPACITY) {
        return -1;
    }
    for (uint32_t i = 0; i < manager->count; ++i) {
        if (std::strcmp(manager->configs[i].topic, topic) == 0) return -1;
    }
    PemRuntimeTopicConfig& config = manager->configs[manager->count];
    std::snprintf(config.topic, sizeof(config.topic), "%s", topic);
    config.kind = kind;
    config.priority = priority;
    config.heartbeat_timeout_ms = heartbeat_timeout_ms;
    manager->bindings[manager->count].index = (uint16_t)manager->count;
    ++manager->count;
    return 0;
}

static int topic_manager_defaults(PemTopicManager* manager) {
    *manager = PemTopicManager {};
    return topic_manager_add(manager, "sensor/gps", PEM_PAYLOAD_GPS,
                             PEM_INPUT_METRIC, 2000) ||
           topic_manager_add(manager, "fusion/localization", PEM_PAYLOAD_LOCALIZATION,
                             PEM_INPUT_METRIC, 2000) ||
           topic_manager_add(manager, "navigation/region", PEM_PAYLOAD_REGION,
                             PEM_INPUT_METRIC, 0) ||
           topic_manager_add(manager, "pem/degrade_event", PEM_PAYLOAD_DEGRADE,
                             PEM_INPUT_FAULT, 0);
}

static int topic_manager_configure(PemTopicManager* manager, const cJSON* params) {
    if (topic_manager_defaults(manager) != 0) return -1;
    const cJSON* runtime = params
        ? cJSON_GetObjectItemCaseSensitive(params, "pem_runtime") : nullptr;
    if (!runtime) return 0;
    if (!cJSON_IsObject(runtime)) return -1;
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(runtime, "schema_version");
    const cJSON* topics = cJSON_GetObjectItemCaseSensitive(runtime, "topics");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsArray(topics))
        return -1;
    *manager = PemTopicManager {};
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, topics) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "topic");
        const cJSON* kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        const cJSON* priority = cJSON_GetObjectItemCaseSensitive(item, "priority");
        const cJSON* heartbeat =
            cJSON_GetObjectItemCaseSensitive(item, "heartbeat_timeout_ms");
        uint8_t parsed_kind = PEM_PAYLOAD_OPAQUE;
        uint8_t parsed_priority = PEM_INPUT_METRIC;
        uint32_t timeout_ms = 0;
        if (!cJSON_IsObject(item) || !cJSON_IsString(name) || !name->valuestring ||
            !cJSON_IsString(kind) || !kind->valuestring ||
            topic_kind_from_json(kind->valuestring, &parsed_kind) != 0) {
            return -1;
        }
        if (cJSON_IsString(priority) && priority->valuestring) {
            if (topic_priority_from_json(priority->valuestring, &parsed_priority) != 0)
                return -1;
        } else if (parsed_kind == PEM_PAYLOAD_DEGRADE) {
            parsed_priority = PEM_INPUT_FAULT;
        } else if (parsed_kind == PEM_PAYLOAD_HEARTBEAT) {
            parsed_priority = PEM_INPUT_HEARTBEAT;
        }
        if (cJSON_IsNumber(heartbeat)) {
            if (heartbeat->valuedouble < 0.0 || heartbeat->valuedouble > 3600000.0)
                return -1;
            timeout_ms = (uint32_t)heartbeat->valuedouble;
        }
        if (topic_manager_add(manager, name->valuestring, parsed_kind, parsed_priority,
                              timeout_ms) != 0) {
            return -1;
        }
    }
    return manager->count > 0 ? 0 : -1;
}

/* TopicManager callback contract: validate bounds then copy into the runtime
 * pool. Parsing, metrics, diagnostics, and file I/O belong to the coroutine. */
static void on_pem_topic(const Message* msg, void* user_data) {
    const PemTopicBinding* binding = static_cast<const PemTopicBinding*>(user_data);
    if (!msg || !binding || msg->data_size == 0) {
        return;
    }
    if (msg->data_size > PEM_RUNTIME_INPUT_DATA_SIZE) {
        pem_runtime_note_input_reject(&g.runtime, binding->index);
        return;
    }
    const uint8_t* data = static_cast<const uint8_t*>(message_bus_message_data(msg));
    if (!data) return;
    (void)pem_runtime_enqueue_input(&g.runtime, binding->index, data,
                                    (uint16_t)msg->data_size, msg->timestamp_us,
                                    clock_now_us());
}

static int write_runtime_record(const PemRuntimeRecord* record, void*) {
    return pem_log_write(&g.log, record->type, record->flags,
                         record->monotonic_us, record->realtime_us,
                         record->name, record->values, record->critical);
}

static void publish_perf_diag(const PemDiagnostic* diagnostic, void*) {
    if (!diagnostic || !g.transport) return;
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "schema_version", diagnostic->schema_version);
    cJSON_AddStringToObject(root, "source", diagnostic->source);
    cJSON_AddStringToObject(root, "code", diagnostic->code);
    cJSON_AddNumberToObject(root, "severity", diagnostic->severity);
    cJSON_AddNumberToObject(root, "monotonic_us", (double)diagnostic->monotonic_us);
    cJSON_AddNumberToObject(root, "realtime_us", (double)diagnostic->realtime_us);
    cJSON* values = cJSON_AddArrayToObject(root, "values");
    for (size_t i = 0; i < PEM_RECORD_VALUE_COUNT; ++i)
        cJSON_AddItemToArray(values, cJSON_CreateNumber(diagnostic->values[i]));
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    transport_publish(g.transport, "perf_diag", (const uint8_t*)json,
                      (uint32_t)std::strlen(json) + 1);
    std::free(json);
}

static void process_gps_input(const PemRuntimeInput& input) {
    GpsData gps {};
    if (GpsData_deserialize(&gps, input.data, input.data_size) != 0 ||
        !valid_coordinate(gps.latitude, gps.longitude)) {
        return;
    }
    const uint64_t timestamp_us = gps.timestamp_us ? gps.timestamp_us :
                                  input.source_timestamp_us;
    if (!timestamp_us) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    MetricState& state = g.metrics;
    if (state.have_gps && timestamp_us > state.last_gps_us) {
        const double step_m = haversine_m(state.last_latitude, state.last_longitude,
                                          gps.latitude, gps.longitude);
        const double dt_s = (double)(timestamp_us - state.last_gps_us) / 1e6;
        if (step_m <= g.max_step_m && dt_s <= 5.0) {
            state.distance_m += step_m;
            state.driving_time_s +=
                (state.speed_mps >= g.driving_speed_mps) ? dt_s : 0.0;
            state.source = DISTANCE_GPS;
        }
    }
    state.latitude = gps.latitude;
    state.longitude = gps.longitude;
    state.accuracy_m = gps.accuracy_m;
    state.speed_mps = gps.speed_mps;
    state.last_latitude = gps.latitude;
    state.last_longitude = gps.longitude;
    state.last_gps_us = timestamp_us;
    state.last_fix_us = timestamp_us;
    state.have_gps = true;
}

static void process_localization_input(const PemRuntimeInput& input) {
    Localization localization {};
    if (Localization_deserialize(&localization, input.data, input.data_size) != 0 ||
        !std::isfinite(localization.x) || !std::isfinite(localization.y)) {
        return;
    }
    const uint64_t timestamp_us = input.source_timestamp_us;
    if (!timestamp_us) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    MetricState& state = g.metrics;
    const bool gps_recent = state.have_gps && timestamp_us >= state.last_gps_us &&
                            timestamp_us - state.last_gps_us <= 2000000ULL;
    if (state.have_localization && timestamp_us > state.last_localization_us) {
        const double dx = (double)localization.x - state.last_x;
        const double dy = (double)localization.y - state.last_y;
        const double step_m = std::hypot(dx, dy);
        const double dt_s = (double)(timestamp_us - state.last_localization_us) / 1e6;
        if (!gps_recent && step_m <= g.max_step_m && dt_s <= 5.0) {
            state.distance_m += step_m;
            state.driving_time_s +=
                (localization.v >= g.driving_speed_mps) ? dt_s : 0.0;
            state.source = DISTANCE_LOCALIZATION;
        }
    }
    state.last_x = localization.x;
    state.last_y = localization.y;
    state.last_localization_us = timestamp_us;
    state.speed_mps = localization.v;
    state.have_localization = true;
}

static void process_region_input(const PemRuntimeInput& input, uint64_t now_us,
                                 uint64_t realtime_us) {
    cJSON* root = cJSON_ParseWithLength((const char*)input.data, input.data_size);
    if (!root) return;
    const cJSON* region = cJSON_GetObjectItemCaseSensitive(root, "region");
    char event_name[PEM_RECORD_NAME_SIZE] {};
    bool changed = false;
    if (cJSON_IsString(region) && region->valuestring && region->valuestring[0]) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (std::strncmp(g.region, region->valuestring, sizeof(g.region)) != 0) {
            std::snprintf(g.region, sizeof(g.region), "%s", region->valuestring);
            std::snprintf(event_name, sizeof(event_name), "region_transition:%s", g.region);
            changed = true;
        }
    }
    cJSON_Delete(root);
    if (changed) {
        const double values[PEM_RECORD_VALUE_COUNT] = {0.0};
        pem_runtime_emit_event(&g.runtime, PEM_RECORD_EVENT, 1u, now_us, realtime_us,
                               event_name, values, false);
    }
}

static void process_degrade_input(const PemRuntimeInput& input, uint64_t now_us,
                                  uint64_t realtime_us) {
    cJSON* root = cJSON_ParseWithLength((const char*)input.data, input.data_size);
    if (!root) return;
    const cJSON* level = cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    const cJSON* transition =
        cJSON_GetObjectItemCaseSensitive(root, "transition_ms");
    if (cJSON_IsNumber(level) && cJSON_IsNumber(reason)) {
        const double values[PEM_RECORD_VALUE_COUNT] = {
            level->valuedouble, reason->valuedouble,
            cJSON_IsNumber(transition) ? transition->valuedouble : 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0,
        };
        const uint8_t severity = level->valuedouble > 0.0 ? 2u : 0u;
        pem_runtime_report_fault(&g.runtime, "pem_collector", "degrade_transition",
                                 severity, now_us, realtime_us, values);
    }
    cJSON_Delete(root);
}

static void process_runtime_input(const PemRuntimeInput& input) {
    PemRuntimeTopicConfig topic {};
    if (pem_runtime_get_topic(&g.runtime, input.topic_index, &topic) != PEM_RUNTIME_OK)
        return;
    const uint64_t now_us = clock_now_us();
    const uint64_t realtime_us = clock_now_realtime_us();
    pem_runtime_calculate_input_metrics(&g.runtime, &input, now_us, realtime_us);
    switch (topic.kind) {
        case PEM_PAYLOAD_GPS:
            process_gps_input(input);
            break;
        case PEM_PAYLOAD_LOCALIZATION:
            process_localization_input(input);
            break;
        case PEM_PAYLOAD_REGION:
            process_region_input(input, now_us, realtime_us);
            break;
        case PEM_PAYLOAD_DEGRADE:
            process_degrade_input(input, now_us, realtime_us);
            break;
        default:
            break;
    }
}

static void process_runtime_inputs() {
    PemRuntimeInput input {};
    for (uint32_t i = 0; i < PEM_RUNTIME_INPUT_CAPACITY; ++i) {
        if (pem_runtime_dequeue_input(&g.runtime, &input) != PEM_RUNTIME_OK) break;
        process_runtime_input(input);
    }
}

class PemCollectorTask : public CoroutineTask {
public:
    explicit PemCollectorTask(MessageBus* bus) : CoroutineTask(bus) {}

    Task run() override {
        const uint64_t period_us = (uint64_t)(1e6 / g.emit_hz);
        while (!should_stop()) {
            co_await sleep_us(period_us);
            if (should_stop()) break;
            flush();
        }
    }

private:
    void flush() {
        /* This coroutine is the only PEM pipeline consumer and durable writer:
         * fault inputs are dequeued before metric and heartbeat inputs. */
        process_runtime_inputs();
        MetricState metrics;
        char region[sizeof(g.region)];
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            metrics = g.metrics;
            std::memcpy(region, g.region, sizeof(region));
        }
        const uint64_t now_us = clock_now_us();
        const uint64_t realtime_us = clock_now_realtime_us();
        const double values[8] = {
            metrics.distance_m, metrics.driving_time_s, metrics.speed_mps,
            metrics.latitude, metrics.longitude, metrics.accuracy_m,
            (double)metrics.source,
            metrics.last_fix_us && now_us >= metrics.last_fix_us
                ? (double)(now_us - metrics.last_fix_us) / 1e6 : -1.0
        };
        char name[PEM_RECORD_NAME_SIZE];
        std::snprintf(name, sizeof(name), "trip:%s", region);
        pem_runtime_update_metric(&g.runtime, PEM_RECORD_BUSINESS, 0, now_us,
                                  realtime_us, name, values, false);
        pem_runtime_watchdog_tick(&g.runtime, now_us, realtime_us);
        PemRuntimeStats stats {};
        pem_runtime_get_stats(&g.runtime, &stats);
        const uint64_t dropped_inputs =
            stats.dropped_inputs[PEM_INPUT_FAULT] +
            stats.dropped_inputs[PEM_INPUT_METRIC] +
            stats.dropped_inputs[PEM_INPUT_HEARTBEAT];
        if (dropped_inputs > g.reported_dropped_inputs) {
            const double dropped[PEM_RECORD_VALUE_COUNT] = {
                (double)(dropped_inputs - g.reported_dropped_inputs),
                (double)stats.dropped_inputs[PEM_INPUT_FAULT],
                (double)stats.dropped_inputs[PEM_INPUT_METRIC],
                (double)stats.dropped_inputs[PEM_INPUT_HEARTBEAT],
                0.0, 0.0, 0.0, 0.0,
            };
            if (pem_runtime_report_fault(&g.runtime, "pem_collector",
                                         "input_pool_overflow", 2u, now_us,
                                         realtime_us, dropped) == PEM_RUNTIME_OK) {
                g.reported_dropped_inputs = dropped_inputs;
            }
        }
        if (stats.dropped_events > g.reported_dropped_events) {
            const double dropped[PEM_RECORD_VALUE_COUNT] = {
                (double)(stats.dropped_events - g.reported_dropped_events),
                0.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0
            };
            if (pem_runtime_emit_event(&g.runtime, PEM_RECORD_EVENT, 1u, now_us,
                                       realtime_us, "collector_event_overflow",
                                       dropped, true) == PEM_RUNTIME_OK)
                g.reported_dropped_events = stats.dropped_events;
        }
        if (pem_runtime_drain(&g.runtime, write_runtime_record, nullptr,
                              PEM_RUNTIME_EVENT_CAPACITY + PEM_RUNTIME_METRIC_CAPACITY) < 0)
            LOG_ERROR("pem_collector", "PEM runtime drain failed");
    }
};

EXPORT_COROUTINE_TASK(PemCollectorTask, pem_collector)

static const char* s_inputs[] = {
    "sensor/gps", "fusion/localization", "navigation/region", "pem/degrade_event", nullptr
};
static const char* s_outputs[] = {"perf_diag", nullptr};
extern NodePlugin s_plugin;

static int pem_collector_init(MessageBus* bus, Transport* transport,
                              DiscoveryManager* discovery, Scheduler* scheduler,
                              const char* params_json) {
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;
    g.metrics = {};
    std::snprintf(g.region, sizeof(g.region), "%s", "unassigned");
    g.emit_hz = 1.0;
    g.max_step_m = kDefaultMaxStepM;
    g.driving_speed_mps = kDefaultDrivingSpeedMps;
    g.reported_dropped_events = 0;
    g.reported_dropped_inputs = 0;
    uint64_t rotate_sec = 300;
    uint64_t rotate_bytes = 100ULL * 1024 * 1024;
    uint32_t retain_segments = 96;
    uint64_t retain_bytes = 1024ULL * 1024 * 1024;
    flow_temp_path(g.pem_path, sizeof(g.pem_path), "kunautodrive_pem_business");

    cJSON* params = params_json ? cJSON_Parse(params_json) : nullptr;
    if (params) {
        const cJSON* item;
#define PEM_NUMBER_PARAM(key, target) \
        item = cJSON_GetObjectItemCaseSensitive(params, key); \
        if (cJSON_IsNumber(item) && item->valuedouble > 0.0) target = item->valuedouble
        PEM_NUMBER_PARAM("emit_hz", g.emit_hz);
        PEM_NUMBER_PARAM("max_step_m", g.max_step_m);
        PEM_NUMBER_PARAM("driving_speed_mps", g.driving_speed_mps);
        PEM_NUMBER_PARAM("rotate_sec", rotate_sec);
        item = cJSON_GetObjectItemCaseSensitive(params, "rotate_mb");
        if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
            rotate_bytes = (uint64_t)(item->valuedouble * 1024.0 * 1024.0);
        item = cJSON_GetObjectItemCaseSensitive(params, "retain_segments");
        if (cJSON_IsNumber(item) && item->valuedouble >= 2.0)
            retain_segments = (uint32_t)item->valuedouble;
        item = cJSON_GetObjectItemCaseSensitive(params, "retain_mb");
        if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
            retain_bytes = (uint64_t)(item->valuedouble * 1024.0 * 1024.0);
#undef PEM_NUMBER_PARAM
        item = cJSON_GetObjectItemCaseSensitive(params, "region");
        if (cJSON_IsString(item) && item->valuestring)
            std::snprintf(g.region, sizeof(g.region), "%s", item->valuestring);
        item = cJSON_GetObjectItemCaseSensitive(params, "pem_log_path");
        if (cJSON_IsString(item) && item->valuestring)
            std::snprintf(g.pem_path, sizeof(g.pem_path), "%s", item->valuestring);
    }
    const int topic_config_rc = topic_manager_configure(&g.topic_manager, params);
    if (params) cJSON_Delete(params);
    if (topic_config_rc != 0) {
        LOG_ERROR("pem_collector", "invalid pem_runtime topic configuration (schema v1)");
        return -1;
    }
    if (g.emit_hz > 10.0) g.emit_hz = 10.0;
    if (pem_runtime_init(&g.runtime) != PEM_RUNTIME_OK) {
        LOG_ERROR("pem_collector", "cannot initialize bounded PEM runtime");
        return -1;
    }
    if (pem_runtime_configure_topics(&g.runtime, g.topic_manager.configs,
                                     g.topic_manager.count) != PEM_RUNTIME_OK) {
        pem_runtime_destroy(&g.runtime);
        LOG_ERROR("pem_collector", "cannot initialize PEM TopicManager");
        return -1;
    }
    pem_runtime_set_diagnostic_callback(&g.runtime, publish_perf_diag, nullptr);
    if (pem_log_open(&g.log, g.pem_path, rotate_sec, rotate_bytes,
                     retain_segments, retain_bytes) != 0) {
        pem_runtime_destroy(&g.runtime);
        LOG_ERROR("pem_collector", "cannot open PEM stream at %s", g.pem_path);
        return -1;
    }
    g.opened = true;
    transport_advertise(transport, "perf_diag", 0u);
    for (uint32_t i = 0; i < g.topic_manager.count; ++i) {
        transport_subscribe(transport, g.topic_manager.configs[i].topic,
                            on_pem_topic, &g.topic_manager.bindings[i]);
    }

    TaskConfig config {};
    std::snprintf(config.name, sizeof(config.name), "pem_collector");
    config.priority = TASK_PRIORITY_LOW;
    g.task_wrapper = pem_collector_create(&config, bus);
    if (!g.task_wrapper) {
        pem_log_close(&g.log);
        pem_runtime_destroy(&g.runtime);
        g.opened = false;
        return -1;
    }
    s_plugin.taskbase = pem_collector_get_base(g.task_wrapper);
    LOG_INFO("pem_collector", "initialized (PEM pipeline, %.1f Hz, topics=%u, region=%s, stream=%s)",
             g.emit_hz, g.topic_manager.count, g.region, g.pem_path);
    return 0;
}

static int pem_collector_start() {
    if (!g.task_wrapper) return -1;
    const int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) LOG_WARN("pem_collector", "node_start_managed failed: %d", rc);
    node_announce_self(g.transport, &s_plugin);
    return rc;
}

static void pem_collector_stop() {
    if (g.task_wrapper) pem_collector_stop(&g.task_wrapper->base);
}

static void pem_collector_cleanup() {
    if (g.task_wrapper) {
        pem_collector_destroy(g.task_wrapper);
        g.task_wrapper = nullptr;
    }
    s_plugin.taskbase = nullptr;
    if (g.opened) {
        pem_log_close(&g.log);
        g.opened = false;
    }
    pem_runtime_destroy(&g.runtime);
}

static int pem_collector_health() { return g.opened ? 0 : -1; }

NodePlugin s_plugin = {
    NODE_PLUGIN_API_VERSION,
    "pem_collector",
    "1.0.0",
    "Bounded priority PEM metric/event pipeline",
    s_inputs,
    s_outputs,
    pem_collector_init,
    pem_collector_start,
    pem_collector_stop,
    pem_collector_cleanup,
    pem_collector_health,
    nullptr,
};

}  // namespace

extern "C" NodePlugin* node_get_plugin(void) { return &s_plugin; }
