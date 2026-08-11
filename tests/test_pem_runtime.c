#include "pem_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    PemRuntimeRecord records[PEM_RUNTIME_EVENT_CAPACITY + PEM_RUNTIME_METRIC_CAPACITY];
    uint32_t count;
    uint32_t rules;
    uint32_t system_rules;
    uint32_t topic_rules;
    uint32_t health_rules;
    uint32_t event_rules;
    uint32_t diagnostics;
    char last_diagnostic[PEM_RECORD_NAME_SIZE];
} TestSink;

static void count_rule(const PemRuntimeRecord* record, void* user_data) {
    TestSink* sink = (TestSink*)user_data;
    ++sink->rules;
    if (record->type == PEM_RECORD_SYSTEM) ++sink->system_rules;
    if (record->type == PEM_RECORD_TOPIC) ++sink->topic_rules;
    if (record->type == PEM_RECORD_HEALTH) ++sink->health_rules;
    if (record->type == PEM_RECORD_EVENT) ++sink->event_rules;
}

static int collect_record(const PemRuntimeRecord* record, void* user_data) {
    TestSink* sink = (TestSink*)user_data;
    if (sink->count >= sizeof(sink->records) / sizeof(sink->records[0])) return -1;
    sink->records[sink->count++] = *record;
    return 0;
}

static int fail_record(const PemRuntimeRecord* record, void* user_data) {
    (void)record;
    (void)user_data;
    return -1;
}

static void collect_diagnostic(const PemDiagnostic* diagnostic, void* user_data) {
    TestSink* sink = (TestSink*)user_data;
    ++sink->diagnostics;
    snprintf(sink->last_diagnostic, sizeof(sink->last_diagnostic), "%s",
             diagnostic->code);
}

int main(void) {
    PemRuntime runtime;
    TestSink sink = {0};
    const double first[PEM_RECORD_VALUE_COUNT] = {1.0};
    const double latest[PEM_RECORD_VALUE_COUNT] = {2.0, 3.0, 4.0};

    CHECK(pem_runtime_init(&runtime) == PEM_RUNTIME_OK);
    const PemRuntimeTopicConfig topics[] = {
        {.topic = "fault", .priority = PEM_INPUT_FAULT, .kind = PEM_PAYLOAD_DEGRADE},
        {.topic = "metric", .priority = PEM_INPUT_METRIC, .kind = PEM_PAYLOAD_OPAQUE,
         .heartbeat_timeout_ms = 1},
        {.topic = "heartbeat", .priority = PEM_INPUT_HEARTBEAT,
         .kind = PEM_PAYLOAD_HEARTBEAT},
    };
    const uint8_t byte = 7;
    CHECK(pem_runtime_configure_topics(&runtime, topics, 3) == PEM_RUNTIME_OK);
    pem_runtime_set_diagnostic_callback(&runtime, collect_diagnostic, &sink);
    CHECK(pem_runtime_enqueue_input(&runtime, 2, &byte, 1, 100, 1000) ==
          PEM_RUNTIME_OK);
    CHECK(pem_runtime_enqueue_input(&runtime, 1, &byte, 1, 900, 1000) ==
          PEM_RUNTIME_OK);
    CHECK(pem_runtime_enqueue_input(&runtime, 0, &byte, 1, 800, 1000) ==
          PEM_RUNTIME_OK);
    PemRuntimeInput input;
    CHECK(pem_runtime_dequeue_input(&runtime, &input) == PEM_RUNTIME_OK);
    CHECK(input.topic_index == 0);
    CHECK(pem_runtime_dequeue_input(&runtime, &input) == PEM_RUNTIME_OK);
    CHECK(input.topic_index == 1);
    CHECK(pem_runtime_dequeue_input(&runtime, &input) == PEM_RUNTIME_OK);
    CHECK(input.topic_index == 2);
    CHECK(pem_runtime_dequeue_input(&runtime, &input) == PEM_RUNTIME_EMPTY);
    const PemRuntimeInput metric_input = {
        .topic_index = 1, .data_size = 1, .source_timestamp_us = 900,
        .received_us = 1000, .data = {7},
    };
    CHECK(pem_runtime_calculate_input_metrics(&runtime, &metric_input, 1100, 2100) ==
          PEM_RUNTIME_OK);
    PemRuntimeRecord snapshot;
    CHECK(pem_runtime_get_metric(&runtime, PEM_RECORD_TOPIC, "topic:metric",
                                 &snapshot) == PEM_RUNTIME_OK);
    CHECK(snapshot.values[0] == 1.0 && snapshot.values[1] == 100.0 &&
          snapshot.values[2] == 200.0 && snapshot.values[3] == 100.0);
    CHECK(pem_runtime_watchdog_tick(&runtime, 1050, 2050) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_watchdog_tick(&runtime, 2200, 3200) == PEM_RUNTIME_OK);
    CHECK(sink.diagnostics == 1 &&
          strcmp(sink.last_diagnostic, "heartbeat_timeout") == 0);
    const uint32_t metric_input_limit =
        PEM_RUNTIME_INPUT_CAPACITY - PEM_RUNTIME_FAULT_RESERVE;
    for (uint32_t i = 0; i < metric_input_limit; ++i) {
        CHECK(pem_runtime_enqueue_input(&runtime, 1, &byte, 1, i, i) ==
              PEM_RUNTIME_OK);
    }
    CHECK(pem_runtime_enqueue_input(&runtime, 1, &byte, 1, 999, 999) ==
          PEM_RUNTIME_FULL);
    PemRuntimeStats ingress_stats;
    pem_runtime_get_stats(&runtime, &ingress_stats);
    CHECK(ingress_stats.queued_inputs[PEM_INPUT_METRIC] ==
          metric_input_limit);
    CHECK(ingress_stats.dropped_inputs[PEM_INPUT_METRIC] == 1);
    pem_runtime_destroy(&runtime);
    memset(&sink, 0, sizeof(sink));

    CHECK(pem_runtime_init(&runtime) == PEM_RUNTIME_OK);
    pem_runtime_set_rule(&runtime, count_rule, &sink);

    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_SYSTEM, 0, 10, 20,
                                    "system", first, false) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_SYSTEM, 1, 11, 21,
                                    "system", latest, true) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_TOPIC, 0, 12, 22,
                                    "topic:control", latest, false) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_TOPIC, 0, 13, 23,
                                    "latency:fusion", latest, false) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_HEALTH, 0, 14, 24,
                                    "control", latest, false) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_emit_event(&runtime, PEM_RECORD_EVENT, 1, 15, 25,
                                 "degrade_transition", latest, false) == PEM_RUNTIME_OK);

    CHECK(pem_runtime_get_metric(&runtime, PEM_RECORD_SYSTEM, "system", &snapshot) ==
          PEM_RUNTIME_OK);
    CHECK(snapshot.values[0] == 2.0 && snapshot.flags == 1 && snapshot.critical);

    CHECK(pem_runtime_drain(&runtime, collect_record, &sink, 16) == 5);
    CHECK(sink.count == 5 && sink.rules == 5);
    CHECK(sink.system_rules == 1 && sink.topic_rules == 2 &&
          sink.health_rules == 1 && sink.event_rules == 1);
    CHECK(sink.records[0].is_event);
    CHECK(strcmp(sink.records[0].name, "degrade_transition") == 0);
    int seen_latency = 0;
    for (uint32_t i = 0; i < sink.count; ++i)
        seen_latency |= strcmp(sink.records[i].name, "latency:fusion") == 0;
    CHECK(seen_latency);

    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_TOPIC, 0, 16, 26,
                                    "retry_metric", first, false) == PEM_RUNTIME_OK);
    CHECK(pem_runtime_drain(&runtime, fail_record, NULL, 1) == PEM_RUNTIME_INVALID);
    sink.count = 0;
    CHECK(pem_runtime_drain(&runtime, collect_record, &sink, 1) == 1);
    CHECK(sink.count == 1 && strcmp(sink.records[0].name, "retry_metric") == 0);

    for (uint32_t i = 0; i < PEM_RUNTIME_EVENT_CAPACITY; ++i) {
        char name[PEM_RECORD_NAME_SIZE];
        snprintf(name, sizeof(name), "event_%u", i);
        CHECK(pem_runtime_emit_event(&runtime, PEM_RECORD_EVENT, 0, 100 + i, 200 + i,
                                     name, first, false) == PEM_RUNTIME_OK);
    }
    CHECK(pem_runtime_emit_event(&runtime, PEM_RECORD_EVENT, 0, 999, 999,
                                 "overflow", first, false) == PEM_RUNTIME_FULL);
    PemRuntimeStats stats;
    pem_runtime_get_stats(&runtime, &stats);
    CHECK(stats.queued_events == PEM_RUNTIME_EVENT_CAPACITY);
    CHECK(stats.dropped_events == 1);

    sink.count = 0;
    sink.rules = 0;
    CHECK(pem_runtime_drain(&runtime, collect_record, &sink,
                            PEM_RUNTIME_EVENT_CAPACITY) ==
          (int)PEM_RUNTIME_EVENT_CAPACITY);
    CHECK(sink.count == PEM_RUNTIME_EVENT_CAPACITY);
    pem_runtime_get_stats(&runtime, &stats);
    CHECK(stats.queued_events == 0);

    pem_runtime_destroy(&runtime);
    CHECK(pem_runtime_init(&runtime) == PEM_RUNTIME_OK);
    for (uint32_t i = 0; i < PEM_RUNTIME_METRIC_CAPACITY; ++i) {
        char name[PEM_RECORD_NAME_SIZE];
        snprintf(name, sizeof(name), "metric_%u", i);
        CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_TOPIC, 0, i, i,
                                        name, first, false) == PEM_RUNTIME_OK);
    }
    CHECK(pem_runtime_update_metric(&runtime, PEM_RECORD_TOPIC, 0, 999, 999,
                                    "metric_overflow", first, false) ==
          PEM_RUNTIME_FULL);
    pem_runtime_get_stats(&runtime, &stats);
    CHECK(stats.cached_metrics == PEM_RUNTIME_METRIC_CAPACITY);
    CHECK(stats.dropped_metrics == 1);
    puts("PEM runtime bounded cache/queue PASS");
    pem_runtime_destroy(&runtime);
    return 0;
}
