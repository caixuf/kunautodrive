/**
 * flowrec_node.c — 配置驱动的 Bag v2 数据采集 NodePlugin。
 *
 * data_recorder_node 继续负责训练 JSONL 样本；flowrec 只做通用 topic
 * 留存，并将事件前后窗口写入可回放的 Bag v2。
 */

#include "node_plugin.h"
#include "flowrec.h"
#include "logger.h"
#include "clock_service.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FLOWREC_STATUS_TYPE_ID 0x46524353u

static struct {
    Transport* transport;
    DiscoveryManager* discovery;
    Scheduler* scheduler;
    TaskBase taskbase;
    FlowrecEngine* engine;
    char input_topics[FLOWREC_MAX_COLLECTORS * FLOWREC_MAX_TOPICS_PER_COLLECTOR]
                     [MSG_BUS_MAX_TOPIC_LEN];
    const char* announced_inputs[FLOWREC_MAX_COLLECTORS * FLOWREC_MAX_TOPICS_PER_COLLECTOR + 1];
    int input_count;
    char subscribed_topics[FLOWREC_MAX_COLLECTORS * FLOWREC_MAX_TOPICS_PER_COLLECTOR]
                          [MSG_BUS_MAX_TOPIC_LEN];
    int subscription_count;
    double status_hz;
    uint64_t last_status_us;
    int task_initialized;
} g;

static NodePlugin s_plugin;
static void flowrec_on_message(const Message* message, void* user_data);

static bool add_input_topic(const char* topic) {
    for (int i = 0; i < g.input_count; ++i) {
        if (strcmp(g.input_topics[i], topic) == 0) return true;
    }
    if (g.input_count >= FLOWREC_MAX_COLLECTORS * FLOWREC_MAX_TOPICS_PER_COLLECTOR)
        return false;
    size_t length = strnlen(topic, MSG_BUS_MAX_TOPIC_LEN);
    if (length == 0 || length >= MSG_BUS_MAX_TOPIC_LEN) return false;
    memcpy(g.input_topics[g.input_count], topic, length);
    g.input_topics[g.input_count][length] = '\0';
    g.announced_inputs[g.input_count] = g.input_topics[g.input_count];
    g.input_count++;
    g.announced_inputs[g.input_count] = NULL;
    return true;
}

static void unsubscribe_all(void) {
    for (int i = 0; i < g.subscription_count; ++i)
        transport_unsubscribe(g.transport, g.subscribed_topics[i], flowrec_on_message);
    g.subscription_count = 0;
}

static void flowrec_on_message(const Message* message, void* user_data) {
    if (!message || !g.engine) return;
    int collector_index = (int)(intptr_t)user_data;
    (void)flowrec_engine_process(g.engine, collector_index, message);
}

static void publish_status(void) {
    char* status = flowrec_engine_status_json(g.engine);
    if (!status) return;
    transport_publish(g.transport, "flowrec/status", status,
                      (uint32_t)strlen(status) + 1);
    free(status);
}

static int flowrec_execute(TaskBase* task) {
    while (!task->should_stop) {
        uint64_t now_us = clock_now_us();
        flowrec_engine_tick(g.engine, now_us);
        if (g.status_hz > 0.0 &&
            (g.last_status_us == 0 ||
             now_us - g.last_status_us >= (uint64_t)(1000000.0 / g.status_hz))) {
            g.last_status_us = now_us;
            publish_status();
        }
        usleep(50000);
    }
    return 0;
}

static const TaskInterface flowrec_vtable = {
    .execute = flowrec_execute,
};

static int flowrec_init(MessageBus* bus, Transport* transport,
                        DiscoveryManager* discovery, Scheduler* scheduler,
                        const char* params_json) {
    (void)bus;
    if (!transport || !discovery || !scheduler) {
        LOG_ERROR("flowrec", "transport, discovery, and scheduler are required");
        return -1;
    }
    memset(&g, 0, sizeof(g));
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;
    g.status_hz = 1.0;

    if (params_json) {
        cJSON* params = cJSON_Parse(params_json);
        if (!cJSON_IsObject(params)) {
            LOG_ERROR("flowrec", "params_json must be a JSON object");
            cJSON_Delete(params);
            return -1;
        }
        cJSON* status_hz = cJSON_GetObjectItemCaseSensitive(params, "status_hz");
        if (status_hz) {
            if (!cJSON_IsNumber(status_hz) || status_hz->valuedouble < 0.0 ||
                status_hz->valuedouble > 20.0) {
                LOG_ERROR("flowrec", "status_hz must be between 0 and 20");
                cJSON_Delete(params);
                return -1;
            }
            g.status_hz = status_hz->valuedouble;
        }
        cJSON_Delete(params);
    }

    char error[FLOWREC_ERROR_LEN] = "";
    g.engine = flowrec_engine_create_from_json(params_json, error, sizeof(error));
    if (!g.engine) {
        LOG_ERROR("flowrec", "invalid configuration: %s", error);
        return -1;
    }

    for (int collector = 0; collector < flowrec_engine_collector_count(g.engine); ++collector) {
        char topics[FLOWREC_MAX_TOPICS_PER_COLLECTOR][MSG_BUS_MAX_TOPIC_LEN];
        int topic_count = flowrec_engine_get_topics(g.engine, collector, topics,
                                                     FLOWREC_MAX_TOPICS_PER_COLLECTOR);
        if (topic_count <= 0) {
            LOG_ERROR("flowrec", "collector %d has no topics", collector);
            flowrec_engine_destroy(g.engine);
            g.engine = NULL;
            return -1;
        }
        for (int topic = 0; topic < topic_count; ++topic) {
            if (g.subscription_count >= FLOWREC_MAX_COLLECTORS * FLOWREC_MAX_TOPICS_PER_COLLECTOR ||
                !add_input_topic(topics[topic]) ||
                transport_subscribe(g.transport, topics[topic], flowrec_on_message,
                                    (void*)(intptr_t)collector) != 0) {
                LOG_ERROR("flowrec", "cannot subscribe to %s", topics[topic]);
                unsubscribe_all();
                flowrec_engine_destroy(g.engine);
                g.engine = NULL;
                return -1;
            }
            size_t length = strnlen(topics[topic], MSG_BUS_MAX_TOPIC_LEN);
            memcpy(g.subscribed_topics[g.subscription_count], topics[topic], length);
            g.subscribed_topics[g.subscription_count][length] = '\0';
            g.subscription_count++;
        }
    }

    for (int i = 0; i < g.input_count; ++i)
        discovery_advertise(g.discovery, g.input_topics[i], 0, CAP_SUBSCRIBER, 0);
    transport_advertise(g.transport, "flowrec/status", FLOWREC_STATUS_TYPE_ID);
    discovery_advertise(g.discovery, "flowrec/status", FLOWREC_STATUS_TYPE_ID,
                        CAP_PUBLISHER, g.status_hz);

    TaskConfig config;
    memset(&config, 0, sizeof(config));
    snprintf(config.name, sizeof(config.name), "flowrec");
    config.priority = TASK_PRIORITY_LOW;
    config.max_frequency_hz = 20.0;
    config.enable_stats = true;
    if (task_base_init(&g.taskbase, &flowrec_vtable, &config) != 0) {
        LOG_ERROR("flowrec", "task_base_init failed");
        unsubscribe_all();
        flowrec_engine_destroy(g.engine);
        g.engine = NULL;
        return -1;
    }
    g.task_initialized = 1;
    LOG_INFO("flowrec", "initialized with %d collector(s)",
             flowrec_engine_collector_count(g.engine));
    return 0;
}

static int flowrec_start(void) {
    int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) {
        LOG_ERROR("flowrec", "node_start_managed failed: %d", rc);
        return rc;
    }
    publish_status();
    node_announce_self(g.transport, &s_plugin);
    return 0;
}

static void flowrec_stop(void) {
    if (g.task_initialized) task_stop(&g.taskbase);
}

static void flowrec_cleanup(void) {
    if (g.task_initialized) {
        task_stop(&g.taskbase);
        task_base_destroy(&g.taskbase);
        g.task_initialized = 0;
    }
    unsubscribe_all();
    if (g.engine) {
        flowrec_engine_destroy(g.engine);
        g.engine = NULL;
    }
    LOG_INFO("flowrec", "cleanup done");
}

static int flowrec_health(void) {
    return flowrec_engine_health(g.engine);
}

static const char* s_outputs[] = { "flowrec/status", NULL };

static NodePlugin s_plugin = {
    .api_version = NODE_PLUGIN_API_VERSION,
    .name = "flowrec",
    .version = "1.0.0",
    .description = "Configuration-driven Bag v2 topic recorder",
    .input_topics = g.announced_inputs,
    .output_topics = s_outputs,
    .init = flowrec_init,
    .start = flowrec_start,
    .stop = flowrec_stop,
    .cleanup = flowrec_cleanup,
    .health = flowrec_health,
    .taskbase = &g.taskbase,
};

NODE_PLUGIN_EXPORT NodePlugin* node_get_plugin(void) {
    return &s_plugin;
}
