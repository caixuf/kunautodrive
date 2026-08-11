#include "pem_runtime.h"

#include <string.h>

static int runtime_mutex_init(PemRuntime* runtime) {
#if defined(_WIN32)
    InitializeCriticalSection(&runtime->mutex);
    return 0;
#else
    return pthread_mutex_init(&runtime->mutex, NULL);
#endif
}

static void runtime_mutex_destroy(PemRuntime* runtime) {
#if defined(_WIN32)
    DeleteCriticalSection(&runtime->mutex);
#else
    pthread_mutex_destroy(&runtime->mutex);
#endif
    runtime->mutex_initialized = false;
}

static void runtime_mutex_lock(PemRuntime* runtime) {
#if defined(_WIN32)
    EnterCriticalSection(&runtime->mutex);
#else
    pthread_mutex_lock(&runtime->mutex);
#endif
}

static void runtime_mutex_unlock(PemRuntime* runtime) {
#if defined(_WIN32)
    LeaveCriticalSection(&runtime->mutex);
#else
    pthread_mutex_unlock(&runtime->mutex);
#endif
}

static void copy_record(PemRuntimeRecord* record, uint16_t type, uint16_t flags,
                        uint64_t monotonic_us, uint64_t realtime_us,
                        const char* name, const double values[PEM_RECORD_VALUE_COUNT],
                        bool critical, bool is_event, uint64_t sequence) {
    memset(record, 0, sizeof(*record));
    record->type = type;
    record->flags = flags;
    record->monotonic_us = monotonic_us;
    record->realtime_us = realtime_us;
    record->sequence = sequence;
    record->critical = critical;
    record->is_event = is_event;
    size_t name_len = strnlen(name, PEM_RECORD_NAME_SIZE - 1);
    memcpy(record->name, name, name_len);
    memcpy(record->values, values, sizeof(record->values));
}

static void canonical_name(char out_name[PEM_RECORD_NAME_SIZE], const char* name) {
    size_t name_len = strnlen(name, PEM_RECORD_NAME_SIZE - 1);
    memcpy(out_name, name, name_len);
    out_name[name_len] = '\0';
}

static void prefixed_name(char out_name[PEM_RECORD_NAME_SIZE], const char* prefix,
                          const char* name) {
    size_t prefix_len = strnlen(prefix, PEM_RECORD_NAME_SIZE - 1);
    memcpy(out_name, prefix, prefix_len);
    size_t name_len = strnlen(name, PEM_RECORD_NAME_SIZE - prefix_len - 1);
    memcpy(out_name + prefix_len, name, name_len);
    out_name[prefix_len + name_len] = '\0';
}

static int find_metric_slot(const PemRuntime* runtime, uint16_t type,
                            const char* name) {
    for (uint32_t i = 0; i < PEM_RUNTIME_METRIC_CAPACITY; ++i) {
        const PemRuntimeMetricSlot* slot = &runtime->metrics[i];
        if (slot->in_use && slot->record.type == type &&
            strcmp(slot->record.name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int find_free_metric_slot(const PemRuntime* runtime) {
    for (uint32_t i = 0; i < PEM_RUNTIME_METRIC_CAPACITY; ++i) {
        if (!runtime->metrics[i].in_use) return (int)i;
    }
    return -1;
}

int pem_runtime_init(PemRuntime* runtime) {
    if (!runtime) return PEM_RUNTIME_INVALID;
    memset(runtime, 0, sizeof(*runtime));
    if (runtime_mutex_init(runtime) != 0) return PEM_RUNTIME_INVALID;
    runtime->mutex_initialized = true;
    runtime->free_input_count = PEM_RUNTIME_INPUT_CAPACITY;
    for (uint16_t i = 0; i < PEM_RUNTIME_INPUT_CAPACITY; ++i)
        runtime->free_input_indices[i] = i;
    return PEM_RUNTIME_OK;
}

void pem_runtime_destroy(PemRuntime* runtime) {
    if (!runtime || !runtime->mutex_initialized) return;
    runtime_mutex_destroy(runtime);
}

int pem_runtime_configure_topics(PemRuntime* runtime,
                                 const PemRuntimeTopicConfig* topics,
                                 uint32_t topic_count) {
    if (!runtime || !runtime->mutex_initialized || !topics || topic_count == 0 ||
        topic_count > PEM_RUNTIME_TOPIC_CAPACITY)
        return PEM_RUNTIME_INVALID;
    PemRuntimeTopicConfig copied[PEM_RUNTIME_TOPIC_CAPACITY];
    memset(copied, 0, sizeof(copied));
    for (uint32_t i = 0; i < topic_count; ++i) {
        if (!topics[i].topic[0] || topics[i].priority >= PEM_INPUT_PRIORITY_COUNT ||
            topics[i].kind > PEM_PAYLOAD_HEARTBEAT)
            return PEM_RUNTIME_INVALID;
        canonical_name(copied[i].topic, topics[i].topic);
        copied[i].priority = topics[i].priority;
        copied[i].kind = topics[i].kind;
        copied[i].heartbeat_timeout_ms = topics[i].heartbeat_timeout_ms;
    }
    runtime_mutex_lock(runtime);
    for (uint32_t priority = 0; priority < PEM_INPUT_PRIORITY_COUNT; ++priority) {
        if (runtime->input_queue_count[priority] != 0) {
            runtime_mutex_unlock(runtime);
            return PEM_RUNTIME_INVALID;
        }
    }
    memcpy(runtime->topics, copied, sizeof(copied));
    memset(runtime->topic_state, 0, sizeof(runtime->topic_state));
    runtime->topic_count = topic_count;
    runtime->watchdog_started_us = 0;
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_OK;
}

int pem_runtime_get_topic(PemRuntime* runtime, uint16_t topic_index,
                          PemRuntimeTopicConfig* out_topic) {
    if (!runtime || !runtime->mutex_initialized || !out_topic) return PEM_RUNTIME_INVALID;
    runtime_mutex_lock(runtime);
    if (topic_index >= runtime->topic_count) {
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_INVALID;
    }
    *out_topic = runtime->topics[topic_index];
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_OK;
}

int pem_runtime_enqueue_input(PemRuntime* runtime, uint16_t topic_index,
                              const uint8_t* data, uint16_t data_size,
                              uint64_t source_timestamp_us,
                              uint64_t received_us) {
    if (!runtime || !runtime->mutex_initialized || !data || data_size == 0 ||
        data_size > PEM_RUNTIME_INPUT_DATA_SIZE)
        return PEM_RUNTIME_INVALID;
    runtime_mutex_lock(runtime);
    if (topic_index >= runtime->topic_count) {
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_INVALID;
    }
    uint32_t priority = runtime->topics[topic_index].priority;
    uint32_t reserve = priority == PEM_INPUT_HEARTBEAT
                           ? PEM_RUNTIME_FAULT_RESERVE + PEM_RUNTIME_METRIC_RESERVE
                           : (priority == PEM_INPUT_METRIC
                                  ? PEM_RUNTIME_FAULT_RESERVE : 0);
    if (runtime->free_input_count <= reserve) {
        ++runtime->dropped_inputs[priority];
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_FULL;
    }
    uint16_t pool_index = runtime->free_input_indices[--runtime->free_input_count];
    PemRuntimeInput* input = &runtime->input_pool[pool_index];
    input->topic_index = topic_index;
    input->data_size = data_size;
    input->source_timestamp_us = source_timestamp_us;
    input->received_us = received_us;
    input->sequence = ++runtime->next_sequence;
    memcpy(input->data, data, data_size);
    uint32_t tail = (runtime->input_queue_head[priority] +
                     runtime->input_queue_count[priority]) % PEM_RUNTIME_INPUT_CAPACITY;
    runtime->input_queues[priority][tail] = pool_index;
    ++runtime->input_queue_count[priority];
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_OK;
}

void pem_runtime_note_input_reject(PemRuntime* runtime, uint16_t topic_index) {
    if (!runtime || !runtime->mutex_initialized) return;
    runtime_mutex_lock(runtime);
    if (topic_index < runtime->topic_count) {
        uint32_t priority = runtime->topics[topic_index].priority;
        ++runtime->dropped_inputs[priority];
    }
    runtime_mutex_unlock(runtime);
}

int pem_runtime_dequeue_input(PemRuntime* runtime, PemRuntimeInput* out_input) {
    if (!runtime || !runtime->mutex_initialized || !out_input) return PEM_RUNTIME_INVALID;
    runtime_mutex_lock(runtime);
    for (uint32_t priority = 0; priority < PEM_INPUT_PRIORITY_COUNT; ++priority) {
        if (runtime->input_queue_count[priority] == 0) continue;
        uint32_t head = runtime->input_queue_head[priority];
        uint16_t pool_index = runtime->input_queues[priority][head];
        *out_input = runtime->input_pool[pool_index];
        runtime->input_queue_head[priority] = (head + 1) % PEM_RUNTIME_INPUT_CAPACITY;
        --runtime->input_queue_count[priority];
        runtime->free_input_indices[runtime->free_input_count++] = pool_index;
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_OK;
    }
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_EMPTY;
}

int pem_runtime_calculate_input_metrics(PemRuntime* runtime,
                                        const PemRuntimeInput* input,
                                        uint64_t pipeline_now_us,
                                        uint64_t realtime_us) {
    if (!runtime || !runtime->mutex_initialized || !input) return PEM_RUNTIME_INVALID;
    PemRuntimeTopicConfig topic;
    uint32_t fps = 0;
    uint64_t message_latency_us = 0;
    uint64_t e2e_latency_us = 0;
    uint64_t queue_latency_us = 0;
    bool have_source_timestamp = false;

    runtime_mutex_lock(runtime);
    if (input->topic_index >= runtime->topic_count) {
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_INVALID;
    }
    topic = runtime->topics[input->topic_index];
    PemRuntimeTopicState* state = &runtime->topic_state[input->topic_index];
    uint64_t sample_us = input->received_us ? input->received_us : pipeline_now_us;
    while (state->fps_count > 0) {
        uint64_t oldest = state->fps_timestamps[state->fps_head];
        if (sample_us < oldest || sample_us - oldest < 1000000ULL) break;
        state->fps_head = (state->fps_head + 1) % PEM_RUNTIME_FPS_WINDOW;
        --state->fps_count;
    }
    if (state->fps_count == PEM_RUNTIME_FPS_WINDOW) {
        state->fps_head = (state->fps_head + 1) % PEM_RUNTIME_FPS_WINDOW;
        --state->fps_count;
    }
    uint32_t tail = (state->fps_head + state->fps_count) % PEM_RUNTIME_FPS_WINDOW;
    state->fps_timestamps[tail] = sample_us;
    ++state->fps_count;
    state->last_heartbeat_us = sample_us;
    fps = state->fps_count;
    if (input->source_timestamp_us && sample_us >= input->source_timestamp_us) {
        have_source_timestamp = true;
        message_latency_us = sample_us - input->source_timestamp_us;
        e2e_latency_us = pipeline_now_us >= input->source_timestamp_us
                             ? pipeline_now_us - input->source_timestamp_us : 0;
        state->last_message_latency_us = message_latency_us;
        state->last_e2e_latency_us = e2e_latency_us;
    }
    queue_latency_us = pipeline_now_us >= sample_us ? pipeline_now_us - sample_us : 0;
    runtime_mutex_unlock(runtime);

    char name[PEM_RECORD_NAME_SIZE];
    prefixed_name(name, "topic:", topic.topic);
    const double values[PEM_RECORD_VALUE_COUNT] = {
        (double)fps,
        have_source_timestamp ? (double)message_latency_us : -1.0,
        have_source_timestamp ? (double)e2e_latency_us : -1.0,
        (double)queue_latency_us,
        (double)input->data_size,
        (double)topic.priority,
        0.0,
        0.0,
    };
    return pem_runtime_update_metric(runtime, PEM_RECORD_TOPIC, 0,
                                     pipeline_now_us, realtime_us, name,
                                     values, false);
}

int pem_runtime_update_metric(PemRuntime* runtime, uint16_t type, uint16_t flags,
                              uint64_t monotonic_us, uint64_t realtime_us,
                              const char* name,
                              const double values[PEM_RECORD_VALUE_COUNT],
                              bool critical) {
    if (!runtime || !runtime->mutex_initialized || !name || !name[0] || !values)
        return PEM_RUNTIME_INVALID;
    char key[PEM_RECORD_NAME_SIZE];
    canonical_name(key, name);
    runtime_mutex_lock(runtime);
    int index = find_metric_slot(runtime, type, key);
    if (index < 0) index = find_free_metric_slot(runtime);
    if (index < 0) {
        ++runtime->dropped_metrics;
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_FULL;
    }
    PemRuntimeMetricSlot* slot = &runtime->metrics[index];
    copy_record(&slot->record, type, flags, monotonic_us, realtime_us, key,
                values, critical, false, ++runtime->next_sequence);
    slot->in_use = true;
    slot->dirty = true;
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_OK;
}

int pem_runtime_emit_event(PemRuntime* runtime, uint16_t type, uint16_t flags,
                           uint64_t monotonic_us, uint64_t realtime_us,
                           const char* name,
                           const double values[PEM_RECORD_VALUE_COUNT],
                           bool critical) {
    if (!runtime || !runtime->mutex_initialized || !name || !name[0] || !values)
        return PEM_RUNTIME_INVALID;
    runtime_mutex_lock(runtime);
    if (runtime->event_count == PEM_RUNTIME_EVENT_CAPACITY) {
        ++runtime->dropped_events;
        runtime_mutex_unlock(runtime);
        return PEM_RUNTIME_FULL;
    }
    uint32_t index = (runtime->event_head + runtime->event_count) %
                     PEM_RUNTIME_EVENT_CAPACITY;
    copy_record(&runtime->events[index], type, flags, monotonic_us, realtime_us,
                name, values, critical, true, ++runtime->next_sequence);
    ++runtime->event_count;
    runtime_mutex_unlock(runtime);
    return PEM_RUNTIME_OK;
}

int pem_runtime_report_fault(PemRuntime* runtime, const char* source,
                             const char* code, uint8_t severity,
                             uint64_t monotonic_us, uint64_t realtime_us,
                             const double values[PEM_RECORD_VALUE_COUNT]) {
    if (!runtime || !runtime->mutex_initialized || !source || !source[0] ||
        !code || !code[0] || !values)
        return PEM_RUNTIME_INVALID;
    int rc = pem_runtime_emit_event(runtime, PEM_RECORD_EVENT, 1u, monotonic_us,
                                    realtime_us, code, values, severity >= 2);

    PemRuntimeDiagnosticCallback callback;
    void* callback_user_data;
    runtime_mutex_lock(runtime);
    callback = runtime->diagnostic_callback;
    callback_user_data = runtime->diagnostic_user_data;
    runtime_mutex_unlock(runtime);
    if (callback) {
        PemDiagnostic diagnostic;
        memset(&diagnostic, 0, sizeof(diagnostic));
        diagnostic.schema_version = 1;
        diagnostic.monotonic_us = monotonic_us;
        diagnostic.realtime_us = realtime_us;
        diagnostic.severity = severity;
        canonical_name(diagnostic.source, source);
        canonical_name(diagnostic.code, code);
        memcpy(diagnostic.values, values, sizeof(diagnostic.values));
        callback(&diagnostic, callback_user_data);
    }
    return rc;
}

int pem_runtime_watchdog_tick(PemRuntime* runtime, uint64_t monotonic_us,
                              uint64_t realtime_us) {
    if (!runtime || !runtime->mutex_initialized) return PEM_RUNTIME_INVALID;
    typedef struct {
        PemRuntimeTopicConfig topic;
        bool healthy;
        bool transition;
        uint64_t age_us;
        uint64_t timeout_us;
    } WatchdogSample;
    WatchdogSample samples[PEM_RUNTIME_TOPIC_CAPACITY];
    uint32_t sample_count = 0;
    uint32_t queued_inputs = 0;
    uint64_t dropped_inputs = 0;
    uint32_t queued_by_priority[PEM_INPUT_PRIORITY_COUNT] = {0};
    uint64_t dropped_by_priority[PEM_INPUT_PRIORITY_COUNT] = {0};

    runtime_mutex_lock(runtime);
    if (runtime->watchdog_started_us == 0)
        runtime->watchdog_started_us = monotonic_us;
    for (uint32_t priority = 0; priority < PEM_INPUT_PRIORITY_COUNT; ++priority) {
        queued_by_priority[priority] = runtime->input_queue_count[priority];
        dropped_by_priority[priority] = runtime->dropped_inputs[priority];
        queued_inputs += queued_by_priority[priority];
        dropped_inputs += dropped_by_priority[priority];
    }
    for (uint32_t i = 0; i < runtime->topic_count; ++i) {
        PemRuntimeTopicConfig* topic = &runtime->topics[i];
        if (topic->heartbeat_timeout_ms == 0) continue;
        PemRuntimeTopicState* state = &runtime->topic_state[i];
        uint64_t reference_us = state->last_heartbeat_us
                                    ? state->last_heartbeat_us
                                    : runtime->watchdog_started_us;
        uint64_t age_us = monotonic_us >= reference_us
                              ? monotonic_us - reference_us : 0;
        uint64_t timeout_us = (uint64_t)topic->heartbeat_timeout_ms * 1000ULL;
        bool healthy = age_us <= timeout_us;
        bool transition = state->health_initialized && state->healthy != healthy;
        state->health_initialized = true;
        state->healthy = healthy;
        samples[sample_count++] = (WatchdogSample) {
            .topic = *topic,
            .healthy = healthy,
            .transition = transition,
            .age_us = age_us,
            .timeout_us = timeout_us,
        };
    }
    runtime_mutex_unlock(runtime);

    const double process_values[PEM_RECORD_VALUE_COUNT] = {
        1.0, (double)sample_count, (double)queued_inputs, (double)dropped_inputs,
        0.0, 0.0, 0.0, 0.0,
    };
    pem_runtime_update_metric(runtime, PEM_RECORD_HEALTH, 0, monotonic_us,
                              realtime_us, "watchdog:process", process_values, false);
    const double load_values[PEM_RECORD_VALUE_COUNT] = {
        (double)queued_inputs / (double)PEM_RUNTIME_INPUT_CAPACITY,
        (double)queued_by_priority[PEM_INPUT_FAULT],
        (double)queued_by_priority[PEM_INPUT_METRIC],
        (double)queued_by_priority[PEM_INPUT_HEARTBEAT],
        (double)dropped_by_priority[PEM_INPUT_FAULT],
        (double)dropped_by_priority[PEM_INPUT_METRIC],
        (double)dropped_by_priority[PEM_INPUT_HEARTBEAT],
        0.0,
    };
    pem_runtime_update_metric(runtime, PEM_RECORD_SYSTEM, 0, monotonic_us,
                              realtime_us, "runtime:load", load_values, false);
    for (uint32_t i = 0; i < sample_count; ++i) {
        WatchdogSample* sample = &samples[i];
        char name[PEM_RECORD_NAME_SIZE];
        prefixed_name(name, "watchdog:", sample->topic.topic);
        const double values[PEM_RECORD_VALUE_COUNT] = {
            1.0, sample->healthy ? 1.0 : 0.0,
            (double)sample->age_us / 1000.0,
            (double)sample->timeout_us / 1000.0,
            (double)queued_inputs, (double)dropped_inputs, 0.0, 0.0,
        };
        pem_runtime_update_metric(runtime, PEM_RECORD_HEALTH,
                                  sample->healthy ? 0u : 1u, monotonic_us,
                                  realtime_us, name, values, !sample->healthy);
        if (sample->transition) {
            pem_runtime_report_fault(runtime, sample->topic.topic,
                                     sample->healthy ? "heartbeat_recovered"
                                                     : "heartbeat_timeout",
                                     sample->healthy ? 0u : 2u, monotonic_us,
                                     realtime_us, values);
        }
    }
    return PEM_RUNTIME_OK;
}

int pem_runtime_get_metric(PemRuntime* runtime, uint16_t type, const char* name,
                           PemRuntimeRecord* out_record) {
    if (!runtime || !runtime->mutex_initialized || !name || !out_record)
        return PEM_RUNTIME_INVALID;
    char key[PEM_RECORD_NAME_SIZE];
    canonical_name(key, name);
    runtime_mutex_lock(runtime);
    int index = find_metric_slot(runtime, type, key);
    if (index >= 0) *out_record = runtime->metrics[index].record;
    runtime_mutex_unlock(runtime);
    return index >= 0 ? PEM_RUNTIME_OK : PEM_RUNTIME_INVALID;
}

static int take_pending_event(PemRuntime* runtime, PemRuntimeRecord* record) {
    runtime_mutex_lock(runtime);
    if (runtime->event_count == 0) {
        runtime_mutex_unlock(runtime);
        return 0;
    }
    *record = runtime->events[runtime->event_head];
    runtime_mutex_unlock(runtime);
    return 1;
}

static int take_dirty_metric(PemRuntime* runtime, PemRuntimeRecord* record) {
    runtime_mutex_lock(runtime);
    for (uint32_t offset = 0; offset < PEM_RUNTIME_METRIC_CAPACITY; ++offset) {
        uint32_t index = (runtime->next_metric_index + offset) %
                         PEM_RUNTIME_METRIC_CAPACITY;
        PemRuntimeMetricSlot* slot = &runtime->metrics[index];
        if (!slot->in_use || !slot->dirty) continue;
        *record = slot->record;
        runtime->next_metric_index = (index + 1) % PEM_RUNTIME_METRIC_CAPACITY;
        runtime_mutex_unlock(runtime);
        return 1;
    }
    runtime_mutex_unlock(runtime);
    return 0;
}

static void complete_record(PemRuntime* runtime, const PemRuntimeRecord* record) {
    runtime_mutex_lock(runtime);
    if (record->is_event) {
        if (runtime->event_count > 0 &&
            runtime->events[runtime->event_head].sequence == record->sequence) {
            runtime->event_head = (runtime->event_head + 1) % PEM_RUNTIME_EVENT_CAPACITY;
            --runtime->event_count;
        }
    } else {
        int index = find_metric_slot(runtime, record->type, record->name);
        if (index >= 0 && runtime->metrics[index].record.sequence == record->sequence)
            runtime->metrics[index].dirty = false;
    }
    runtime_mutex_unlock(runtime);
}

int pem_runtime_drain(PemRuntime* runtime, PemRuntimeWriter writer,
                      void* user_data, uint32_t max_records) {
    if (!runtime || !runtime->mutex_initialized || !writer || max_records == 0)
        return PEM_RUNTIME_INVALID;
    int drained = 0;
    while ((uint32_t)drained < max_records) {
        PemRuntimeRecord record;
        int found = take_pending_event(runtime, &record);
        if (!found) found = take_dirty_metric(runtime, &record);
        if (!found) break;

        PemRuntimeRuleCallback rule;
        void* rule_user_data;
        runtime_mutex_lock(runtime);
        rule = runtime->rule;
        rule_user_data = runtime->rule_user_data;
        runtime_mutex_unlock(runtime);
        if (writer(&record, user_data) != 0) return PEM_RUNTIME_INVALID;
        complete_record(runtime, &record);
        if (rule) rule(&record, rule_user_data);
        ++drained;
    }
    return drained;
}

void pem_runtime_get_stats(PemRuntime* runtime, PemRuntimeStats* out_stats) {
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!runtime || !runtime->mutex_initialized) return;
    runtime_mutex_lock(runtime);
    out_stats->dropped_events = runtime->dropped_events;
    out_stats->dropped_metrics = runtime->dropped_metrics;
    out_stats->queued_events = runtime->event_count;
    out_stats->configured_topics = runtime->topic_count;
    for (uint32_t priority = 0; priority < PEM_INPUT_PRIORITY_COUNT; ++priority) {
        out_stats->dropped_inputs[priority] = runtime->dropped_inputs[priority];
        out_stats->queued_inputs[priority] = runtime->input_queue_count[priority];
    }
    for (uint32_t i = 0; i < PEM_RUNTIME_METRIC_CAPACITY; ++i)
        out_stats->cached_metrics += runtime->metrics[i].in_use ? 1u : 0u;
    runtime_mutex_unlock(runtime);
}

void pem_runtime_set_rule(PemRuntime* runtime, PemRuntimeRuleCallback rule,
                          void* user_data) {
    if (!runtime || !runtime->mutex_initialized) return;
    runtime_mutex_lock(runtime);
    runtime->rule = rule;
    runtime->rule_user_data = user_data;
    runtime_mutex_unlock(runtime);
}

void pem_runtime_set_diagnostic_callback(PemRuntime* runtime,
                                         PemRuntimeDiagnosticCallback callback,
                                         void* user_data) {
    if (!runtime || !runtime->mutex_initialized) return;
    runtime_mutex_lock(runtime);
    runtime->diagnostic_callback = callback;
    runtime->diagnostic_user_data = user_data;
    runtime_mutex_unlock(runtime);
}
