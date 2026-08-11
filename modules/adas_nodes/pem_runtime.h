#ifndef FLOWENGINE_PEM_RUNTIME_H
#define FLOWENGINE_PEM_RUNTIME_H

#include "pem_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PEM runtime is the in-memory half of PEM collection. Producers only update
 * this bounded cache/queue; the designated drain task is the sole caller of
 * the durable writer callback.
 */
#define PEM_RUNTIME_METRIC_CAPACITY 128u
#define PEM_RUNTIME_EVENT_CAPACITY 64u
#define PEM_RUNTIME_TOPIC_CAPACITY 16u
#define PEM_RUNTIME_INPUT_CAPACITY 96u
#define PEM_RUNTIME_INPUT_DATA_SIZE 512u
#define PEM_RUNTIME_FPS_WINDOW 32u
#define PEM_RUNTIME_FAULT_RESERVE 16u
#define PEM_RUNTIME_METRIC_RESERVE 16u

typedef enum {
    PEM_INPUT_FAULT = 0,
    PEM_INPUT_METRIC = 1,
    PEM_INPUT_HEARTBEAT = 2,
    PEM_INPUT_PRIORITY_COUNT = 3,
} PemInputPriority;

typedef enum {
    PEM_PAYLOAD_OPAQUE = 0,
    PEM_PAYLOAD_GPS,
    PEM_PAYLOAD_LOCALIZATION,
    PEM_PAYLOAD_REGION,
    PEM_PAYLOAD_DEGRADE,
    PEM_PAYLOAD_HEARTBEAT,
} PemPayloadKind;

typedef struct {
    char topic[PEM_RECORD_NAME_SIZE];
    uint8_t priority;
    uint8_t kind;
    uint32_t heartbeat_timeout_ms;
} PemRuntimeTopicConfig;

typedef struct {
    uint16_t topic_index;
    uint16_t data_size;
    uint64_t source_timestamp_us;
    uint64_t received_us;
    uint64_t sequence;
    uint8_t data[PEM_RUNTIME_INPUT_DATA_SIZE];
} PemRuntimeInput;

typedef struct {
    uint32_t schema_version;
    uint64_t monotonic_us;
    uint64_t realtime_us;
    char source[PEM_RECORD_NAME_SIZE];
    char code[PEM_RECORD_NAME_SIZE];
    uint8_t severity;
    double values[PEM_RECORD_VALUE_COUNT];
} PemDiagnostic;

typedef struct {
    uint16_t type;
    uint16_t flags;
    uint64_t monotonic_us;
    uint64_t realtime_us;
    uint64_t sequence;
    char name[PEM_RECORD_NAME_SIZE];
    double values[PEM_RECORD_VALUE_COUNT];
    bool critical;
    bool is_event;
} PemRuntimeRecord;

typedef void (*PemRuntimeRuleCallback)(const PemRuntimeRecord* record,
                                       void* user_data);
typedef int (*PemRuntimeWriter)(const PemRuntimeRecord* record,
                                void* user_data);
typedef void (*PemRuntimeDiagnosticCallback)(const PemDiagnostic* diagnostic,
                                             void* user_data);

typedef struct {
    PemRuntimeRecord record;
    bool in_use;
    bool dirty;
} PemRuntimeMetricSlot;

typedef struct {
    uint64_t fps_timestamps[PEM_RUNTIME_FPS_WINDOW];
    uint64_t last_heartbeat_us;
    uint64_t last_message_latency_us;
    uint64_t last_e2e_latency_us;
    uint32_t fps_head;
    uint32_t fps_count;
    bool health_initialized;
    bool healthy;
} PemRuntimeTopicState;

typedef struct {
    PemRuntimeRecord events[PEM_RUNTIME_EVENT_CAPACITY];
    PemRuntimeMetricSlot metrics[PEM_RUNTIME_METRIC_CAPACITY];
    PemRuntimeInput input_pool[PEM_RUNTIME_INPUT_CAPACITY];
    PemRuntimeTopicConfig topics[PEM_RUNTIME_TOPIC_CAPACITY];
    PemRuntimeTopicState topic_state[PEM_RUNTIME_TOPIC_CAPACITY];
    PemLogMutex mutex;
    PemRuntimeRuleCallback rule;
    void* rule_user_data;
    PemRuntimeDiagnosticCallback diagnostic_callback;
    void* diagnostic_user_data;
    uint64_t next_sequence;
    uint64_t watchdog_started_us;
    uint64_t dropped_events;
    uint64_t dropped_metrics;
    uint64_t dropped_inputs[PEM_INPUT_PRIORITY_COUNT];
    uint16_t free_input_indices[PEM_RUNTIME_INPUT_CAPACITY];
    uint16_t input_queues[PEM_INPUT_PRIORITY_COUNT][PEM_RUNTIME_INPUT_CAPACITY];
    uint32_t event_head;
    uint32_t event_count;
    uint32_t next_metric_index;
    uint32_t free_input_count;
    uint32_t input_queue_head[PEM_INPUT_PRIORITY_COUNT];
    uint32_t input_queue_count[PEM_INPUT_PRIORITY_COUNT];
    uint32_t topic_count;
    bool mutex_initialized;
} PemRuntime;

typedef struct {
    uint64_t dropped_events;
    uint64_t dropped_metrics;
    uint64_t dropped_inputs[PEM_INPUT_PRIORITY_COUNT];
    uint32_t queued_events;
    uint32_t cached_metrics;
    uint32_t queued_inputs[PEM_INPUT_PRIORITY_COUNT];
    uint32_t configured_topics;
} PemRuntimeStats;

enum {
    PEM_RUNTIME_OK = 0,
    PEM_RUNTIME_INVALID = -1,
    PEM_RUNTIME_FULL = 1,
    PEM_RUNTIME_EMPTY = 2,
};

int pem_runtime_init(PemRuntime* runtime);
void pem_runtime_destroy(PemRuntime* runtime);

/* TopicManager: fixed configuration copied during node initialization. */
int pem_runtime_configure_topics(PemRuntime* runtime,
                                 const PemRuntimeTopicConfig* topics,
                                 uint32_t topic_count);
int pem_runtime_get_topic(PemRuntime* runtime, uint16_t topic_index,
                          PemRuntimeTopicConfig* out_topic);

/*
 * Callback-side MPSC ingress. It copies at most 512 bytes to a preallocated
 * pool and performs no parsing, allocation, rule execution, publication, or
 * durable I/O. The pipeline dequeues fault, metric, then heartbeat inputs.
 */
int pem_runtime_enqueue_input(PemRuntime* runtime, uint16_t topic_index,
                              const uint8_t* data, uint16_t data_size,
                              uint64_t source_timestamp_us,
                              uint64_t received_us);
void pem_runtime_note_input_reject(PemRuntime* runtime, uint16_t topic_index);
int pem_runtime_dequeue_input(PemRuntime* runtime, PemRuntimeInput* out_input);

/*
 * MetricManager's common calculators, invoked by the single pipeline thread:
 * one-second sliding-window fps, delivery latency, end-to-end latency, and
 * pipeline queue latency. The latest result is cached as topic:<topic>.
 */
int pem_runtime_calculate_input_metrics(PemRuntime* runtime,
                                        const PemRuntimeInput* input,
                                        uint64_t pipeline_now_us,
                                        uint64_t realtime_us);

/*
 * WatchdogManager's in-process process/heartbeat check. It writes current
 * health snapshots and creates a fault transition when a configured heartbeat
 * becomes stale or recovers.
 */
int pem_runtime_watchdog_tick(PemRuntime* runtime, uint64_t monotonic_us,
                              uint64_t realtime_us);

/*
 * Replaces the latest value for (type, name). It never queues an unbounded
 * history, so high-rate continuous metrics consume at most one cache slot.
 */
int pem_runtime_update_metric(PemRuntime* runtime, uint16_t type, uint16_t flags,
                              uint64_t monotonic_us, uint64_t realtime_us,
                              const char* name,
                              const double values[PEM_RECORD_VALUE_COUNT],
                              bool critical);

/* Enqueues an immutable transition/event. A full pool drops the new event. */
int pem_runtime_emit_event(PemRuntime* runtime, uint16_t type, uint16_t flags,
                           uint64_t monotonic_us, uint64_t realtime_us,
                           const char* name,
                           const double values[PEM_RECORD_VALUE_COUNT],
                           bool critical);
int pem_runtime_report_fault(PemRuntime* runtime, const char* source,
                             const char* code, uint8_t severity,
                             uint64_t monotonic_us, uint64_t realtime_us,
                             const double values[PEM_RECORD_VALUE_COUNT]);

/* Reads the latest cached metric without changing its pending-drain state. */
int pem_runtime_get_metric(PemRuntime* runtime, uint16_t type, const char* name,
                           PemRuntimeRecord* out_record);

/*
 * Runs only in the designated writer task. Rules run after a successful write
 * and outside the runtime lock, so a rule may enqueue a bounded follow-up event.
 * max_records must be non-zero. A writer failure leaves the current record
 * pending for a later drain and returns PEM_RUNTIME_INVALID.
 */
int pem_runtime_drain(PemRuntime* runtime, PemRuntimeWriter writer,
                      void* user_data, uint32_t max_records);
void pem_runtime_get_stats(PemRuntime* runtime, PemRuntimeStats* out_stats);
void pem_runtime_set_rule(PemRuntime* runtime, PemRuntimeRuleCallback rule,
                          void* user_data);
void pem_runtime_set_diagnostic_callback(PemRuntime* runtime,
                                         PemRuntimeDiagnosticCallback callback,
                                         void* user_data);

#ifdef __cplusplus
}
#endif

#endif
