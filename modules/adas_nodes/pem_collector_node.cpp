/*
 * PEM business collector: topic callbacks stay non-blocking, while one
 * FlowCoro task owns aggregation snapshots and the durable PEM writer.
 */
#include "node_plugin.h"
#include "adas_msgs_gen.h"
#include "clock_service.h"
#include "coroutine_task.h"
#include "pem_log.h"
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
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

constexpr double kEarthRadiusM = 6371000.0;
constexpr double kDefaultMaxStepM = 50.0;
constexpr double kDefaultDrivingSpeedMps = 0.3;
constexpr size_t kEventQueueCapacity = 32;

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

struct PendingEvent {
    char name[PEM_RECORD_NAME_SIZE] {};
    double values[PEM_RECORD_VALUE_COUNT] {};
    bool critical = false;
};

struct Collector {
    Transport* transport = nullptr;
    DiscoveryManager* discovery = nullptr;
    Scheduler* scheduler = nullptr;
    PemLog log {};
    std::mutex mutex;
    MetricState metrics {};
    PendingEvent events[kEventQueueCapacity] {};
    size_t event_head = 0;
    size_t event_size = 0;
    char region[32] = "unassigned";
    char pem_path[512] {};
    double emit_hz = 1.0;
    double max_step_m = kDefaultMaxStepM;
    double driving_speed_mps = kDefaultDrivingSpeedMps;
    uint64_t dropped_events = 0;
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

static void enqueue_event_locked(const char* name, const double values[8], bool critical) {
    if (g.event_size == kEventQueueCapacity) {
        g.event_head = (g.event_head + 1) % kEventQueueCapacity;
        --g.event_size;
        ++g.dropped_events;
    }
    PendingEvent& event = g.events[(g.event_head + g.event_size) % kEventQueueCapacity];
    std::snprintf(event.name, sizeof(event.name), "%s", name);
    std::memcpy(event.values, values, sizeof(event.values));
    event.critical = critical;
    ++g.event_size;
}

static void on_gps(const Message* msg, void*) {
    GpsData gps {};
    if (!msg || GpsData_deserialize(&gps, msg->data, msg->data_size) != 0 ||
        !valid_coordinate(gps.latitude, gps.longitude)) {
        return;
    }
    const uint64_t timestamp_us = gps.timestamp_us ? gps.timestamp_us : msg->timestamp_us;
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

static void on_localization(const Message* msg, void*) {
    Localization localization {};
    if (!msg || Localization_deserialize(&localization, msg->data, msg->data_size) != 0 ||
        !std::isfinite(localization.x) || !std::isfinite(localization.y)) {
        return;
    }
    const uint64_t timestamp_us = msg->timestamp_us;
    std::lock_guard<std::mutex> lock(g.mutex);
    MetricState& state = g.metrics;
    /* GPS is the authoritative odometer whenever a recent fix exists. */
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

static void on_region(const Message* msg, void*) {
    if (!msg || msg->data_size == 0) return;
    cJSON* root = cJSON_ParseWithLength((const char*)msg->data, msg->data_size);
    if (!root) return;
    cJSON* region = cJSON_GetObjectItemCaseSensitive(root, "region");
    if (cJSON_IsString(region) && region->valuestring && region->valuestring[0]) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (std::strncmp(g.region, region->valuestring, sizeof(g.region)) != 0) {
            std::snprintf(g.region, sizeof(g.region), "%s", region->valuestring);
            const double values[8] = {0.0};
            char name[PEM_RECORD_NAME_SIZE];
            std::snprintf(name, sizeof(name), "region_transition:%s", g.region);
            enqueue_event_locked(name, values, false);
        }
    }
    cJSON_Delete(root);
}

static void on_degrade_event(const Message* msg, void*) {
    if (!msg || msg->data_size == 0) return;
    cJSON* root = cJSON_ParseWithLength((const char*)msg->data, msg->data_size);
    if (!root) return;
    const cJSON* level = cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    const cJSON* transition = cJSON_GetObjectItemCaseSensitive(root, "transition_ms");
    if (cJSON_IsNumber(level) && cJSON_IsNumber(reason)) {
        const double values[8] = {
            level->valuedouble, reason->valuedouble,
            cJSON_IsNumber(transition) ? transition->valuedouble : 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0
        };
        std::lock_guard<std::mutex> lock(g.mutex);
        enqueue_event_locked("degrade_transition", values, level->valuedouble > 0.0);
    }
    cJSON_Delete(root);
}

struct PemSubscription {
    const char* topic;
    MessageCallback callback;
};

static const PemSubscription kSubscriptions[] = {
    {"sensor/gps", on_gps},
    {"fusion/localization", on_localization},
    {"navigation/region", on_region},
    {"pem/degrade_event", on_degrade_event},
};

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
        MetricState metrics;
        PendingEvent events[kEventQueueCapacity];
        size_t event_count = 0;
        char region[sizeof(g.region)];
        uint64_t dropped_events = 0;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            metrics = g.metrics;
            std::memcpy(region, g.region, sizeof(region));
            event_count = g.event_size;
            for (size_t i = 0; i < event_count; ++i)
                events[i] = g.events[(g.event_head + i) % kEventQueueCapacity];
            g.event_head = 0;
            g.event_size = 0;
            dropped_events = g.dropped_events;
            g.dropped_events = 0;
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
        if (pem_log_write(&g.log, PEM_RECORD_BUSINESS, 0, now_us, realtime_us,
                          name, values, false) != 0) {
            LOG_ERROR("pem_collector", "business snapshot write failed");
        }
        for (size_t i = 0; i < event_count; ++i) {
            if (pem_log_write(&g.log, PEM_RECORD_EVENT, 1u, now_us, realtime_us,
                              events[i].name, events[i].values,
                              events[i].critical) != 0) {
                LOG_ERROR("pem_collector", "event write failed: %s", events[i].name);
            }
        }
        if (dropped_events) {
            const double dropped[8] = {(double)dropped_events, 0.0, 0.0, 0.0,
                                       0.0, 0.0, 0.0, 0.0};
            pem_log_write(&g.log, PEM_RECORD_EVENT, 1u, now_us, realtime_us,
                          "collector_event_overflow", dropped, true);
        }
    }
};

EXPORT_COROUTINE_TASK(PemCollectorTask, pem_collector)

static const char* s_inputs[] = {
    "sensor/gps", "fusion/localization", "navigation/region", "pem/degrade_event", nullptr
};
static const char* s_outputs[] = {nullptr};
extern NodePlugin s_plugin;

static int pem_collector_init(MessageBus* bus, Transport* transport,
                              DiscoveryManager* discovery, Scheduler* scheduler,
                              const char* params_json) {
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;
    g.metrics = {};
    g.event_head = g.event_size = g.dropped_events = 0;
    std::snprintf(g.region, sizeof(g.region), "%s", "unassigned");
    g.emit_hz = 1.0;
    g.max_step_m = kDefaultMaxStepM;
    g.driving_speed_mps = kDefaultDrivingSpeedMps;
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
        cJSON_Delete(params);
    }
    if (g.emit_hz > 10.0) g.emit_hz = 10.0;
    if (pem_log_open(&g.log, g.pem_path, rotate_sec, rotate_bytes,
                     retain_segments, retain_bytes) != 0) {
        LOG_ERROR("pem_collector", "cannot open PEM stream at %s", g.pem_path);
        return -1;
    }
    g.opened = true;
    for (const PemSubscription& subscription : kSubscriptions)
        transport_subscribe(transport, subscription.topic, subscription.callback, nullptr);

    TaskConfig config {};
    std::snprintf(config.name, sizeof(config.name), "pem_collector");
    config.priority = TASK_PRIORITY_LOW;
    g.task_wrapper = pem_collector_create(&config, bus);
    if (!g.task_wrapper) {
        pem_log_close(&g.log);
        g.opened = false;
        return -1;
    }
    s_plugin.taskbase = pem_collector_get_base(g.task_wrapper);
    LOG_INFO("pem_collector", "initialized (FlowCoro, %.1f Hz, region=%s, stream=%s)",
             g.emit_hz, g.region, g.pem_path);
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
}

static int pem_collector_health() { return g.opened ? 0 : -1; }

NodePlugin s_plugin = {
    NODE_PLUGIN_API_VERSION,
    "pem_collector",
    "1.0.0",
    "FlowCoro asynchronous PEM business telemetry collector",
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
