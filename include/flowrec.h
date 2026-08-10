#ifndef FLOWREC_H
#define FLOWREC_H

/**
 * @file flowrec.h
 * @brief 配置驱动的 Bag v2 采集引擎。
 *
 * flowrec_node 负责 Transport/NodePlugin 生命周期；本模块负责配置校验、
 * 预/后缓冲、触发、轮转和 Bag v2 写入，因此可在不启动节点宿主的条件下测试。
 */

#include "message_bus.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWREC_MAX_COLLECTORS 8
#define FLOWREC_MAX_TOPICS_PER_COLLECTOR 32
#define FLOWREC_NAME_LEN 64
#define FLOWREC_OUTPUT_PATH_LEN 512
#define FLOWREC_ERROR_LEN 160

typedef struct FlowrecEngine FlowrecEngine;

typedef struct {
    char name[FLOWREC_NAME_LEN];
    bool event_triggered;
    bool recording_active;
    uint64_t messages_written;
    uint64_t messages_dropped;
    uint64_t prebuffer_dropped;
    uint64_t triggers;
    uint64_t rotations;
    uint64_t write_errors;
    uint64_t buffered_messages;
    uint64_t buffered_bytes;
    char last_output[FLOWREC_OUTPUT_PATH_LEN];
    char last_error[FLOWREC_ERROR_LEN];
} FlowrecCollectorStatus;

/**
 * 从 pipeline 节点 params JSON 创建采集引擎。
 *
 * 顶层必须包含 collectors 数组。配置错误会返回 NULL，并把简短原因写入 error。
 * 不读取 YAML，也不引入额外运行时依赖。
 */
FlowrecEngine* flowrec_engine_create_from_json(const char* params_json,
                                               char* error, size_t error_size);

/** 关闭所有 Bag、释放预缓冲区和引擎。调用前必须停止消息回调。 */
void flowrec_engine_destroy(FlowrecEngine* engine);

int flowrec_engine_collector_count(const FlowrecEngine* engine);

/** 返回 collector 的精确订阅 topic 数；topics 可为 NULL 以仅查询数量。 */
int flowrec_engine_get_topics(const FlowrecEngine* engine, int collector_index,
                              char topics[][MSG_BUS_MAX_TOPIC_LEN], int max_topics);

/**
 * 处理已由节点按 collector topic 路由的一条消息。
 * 返回 0 表示处理成功；写盘失败会返回非零并反映到 status。
 */
int flowrec_engine_process(FlowrecEngine* engine, int collector_index,
                           const Message* msg);

/** 推进后录制结束与时间轮转。now_us 为 0 时读取 clock_now_us()。 */
void flowrec_engine_tick(FlowrecEngine* engine, uint64_t now_us);

int flowrec_engine_get_status(const FlowrecEngine* engine, int collector_index,
                              FlowrecCollectorStatus* status);

/** 返回 malloc 分配的紧凑 JSON；调用者用 free() 释放。 */
char* flowrec_engine_status_json(const FlowrecEngine* engine);

/** 任何 collector 发生无法打开/写入 Bag 的错误时返回非零。 */
int flowrec_engine_health(const FlowrecEngine* engine);

#ifdef __cplusplus
}
#endif

#endif /* FLOWREC_H */
