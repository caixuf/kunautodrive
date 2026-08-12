/**
 * flow_launcher.c — FlowEngine 纯配置驱动启动器
 *
 * 设计意图: 这是整个项目的「生产入口」。
 *
 * 核心原则:
 *   - 启动器不含任何业务逻辑，只读配置文件
 *   - 节点是独立 .so 插件，通过 dlopen + NodePlugin 接口加载
 *   - pipeline 拓扑、启动顺序、参数全部在 config/pipeline.json 里声明
 *
 * 对比 CyberRT / ROS2:
 *   CyberRT: mainboard + dag 配置文件 → dlopen 加载 Component
 *   FlowEngine: flow_launcher + pipeline.json → dlopen 加载 NodePlugin
 *
 * 用法:
 *   ./build/bin/flow_launcher config/pipeline.json [--duration 30]
 *   ./build/bin/flow_launcher config/pipeline.json --multi [--duration 30]
 *
 * 目录结构:
 *   config/pipeline.json        — pipeline 声明
 *   modules/adas_nodes 下的 .so — 节点插件（感知/融合/规划/控制/监控）
 *   include/node_plugin.h       — NodePlugin 接口定义
 */

#include "fp_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <inttypes.h>

#include "logger.h"
#include "node_plugin.h"
#include "message_bus.h"
#include "transport.h"
#include "discovery.h"
#include "scheduler.h"
#include "flow_registry.h"
#include "config_manager.h"
#include "bag.h"
#include "adas_msgs_gen.h"
#include "crash_handler.h"
#include "param_bridge.h"
#include "param_registry.h"   /* param_count() — 启动日志报告已注册参数数 */
#include "clock_service.h"
#include "error_codes.h"

/* ── 节点描述 ──────────────────────────────────────────────── */

#define MAX_NODES 32
#define MAX_TOPICS_PER_NODE 16
typedef struct {
    char name[64];
    char library[256];
    char params_json[1024];
    int  stagger_after_ms;
    /* 输入/输出 topics（从 pipeline.json 解析）*/
    char inputs[MAX_TOPICS_PER_NODE][64];
    char outputs[MAX_TOPICS_PER_NODE][64];
    int  input_count;
    int  output_count;
    /* dlopen 模式 */
    void*       lib_handle;
    NodePlugin* plugin;
    /* 多进程模式 */
    pid_t       pid;
    bool        replay_disabled;
} NodeDesc;

static volatile int g_running = 1;
static NodeDesc g_nodes[MAX_NODES];
static int      g_node_count = 0;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

typedef struct {
    const char* bag_path;
    const char* topic_filter;
    double      speed;
    uint64_t    start_offset_us;
    uint64_t    end_offset_us;
    bool        loop;
    bool        enabled;
    char        selected_topics[64][64];
    int         selected_topic_count;
} ReplayConfig;

typedef struct {
    bool        use_ipc;
    MessageBus* bus;
    Transport*  transport;
} ReplayPublishCtx;

typedef struct {
    bool     use_deadline;
    uint64_t deadline_wall_us;
} ReplayStopCtx;

/* ── 加载配置 (统一使用 config_manager) ──────────────────────── */

static int parse_pipeline(const char* path, int* stagger_ms_out) {
    LauncherConfig* cfg = config_load(path);
    if (!cfg) {
        LOG_WARN("launcher", "failed to load config: %s", path);
        return -1;
    }

    *stagger_ms_out = 300;  /* default stagger */

    g_node_count = 0;
    for (int i = 0; i < cfg->process_count; i++) {
        if (g_node_count >= MAX_NODES) {
            LOG_WARN("launcher", "node limit reached (%d); skipping %d remaining process(es)",
                     MAX_NODES, cfg->process_count - i);
            break;
        }
        ProcessConfig* pc = &cfg->processes[i];
        NodeDesc* nd = &g_nodes[g_node_count];
        memset(nd, 0, sizeof(*nd));
        nd->stagger_after_ms = *stagger_ms_out;
        nd->pid = -1;

        snprintf(nd->name, sizeof(nd->name), "%s", pc->name);
        snprintf(nd->library, sizeof(nd->library), "%s", pc->library_path);

        /* Map publish → outputs, subscribe → inputs (from node's perspective) */
        for (int k = 0; k < pc->subscribe_count; k++) {
            if (nd->input_count >= MAX_TOPICS_PER_NODE) {
                LOG_WARN("launcher", "node '%s': input topic limit (%d) reached; skipping remaining",
                         nd->name, MAX_TOPICS_PER_NODE);
                break;
            }
            snprintf(nd->inputs[nd->input_count++], 64, "%s", pc->subscribe[k].topic);
        }
        for (int k = 0; k < pc->publish_count; k++) {
            if (nd->output_count >= MAX_TOPICS_PER_NODE) {
                LOG_WARN("launcher", "node '%s': output topic limit (%d) reached; skipping remaining",
                         nd->name, MAX_TOPICS_PER_NODE);
                break;
            }
            snprintf(nd->outputs[nd->output_count++], 64, "%s", pc->publish[k].topic);
        }

        /* Copy params */
        if (pc->params[0])
            snprintf(nd->params_json, sizeof(nd->params_json), "%s", pc->params);

        if (nd->name[0]) g_node_count++;
    }

    LOG_INFO("launcher", "config loaded: %d nodes (from %d processes)",
             g_node_count, cfg->process_count);
    config_free(cfg);
    return g_node_count;
}

static bool replay_stop_requested(void* user_data) {
    ReplayStopCtx* stop = (ReplayStopCtx*)user_data;
    if (!g_running) return true;
    if (stop && stop->use_deadline &&
        clock_now_monotonic_wall_us() >= stop->deadline_wall_us)
        return true;
    return false;
}

static int replay_publish_cb(const Message* msg, void* user_data) {
    ReplayPublishCtx* ctx = (ReplayPublishCtx*)user_data;
    if (!ctx || !msg) return ERR_INVALID_PARAM;
    if (ctx->use_ipc) {
        return transport_publish(ctx->transport, msg->topic,
                                 message_bus_message_data(msg), msg->data_size);
    }
    return message_bus_publish(ctx->bus, msg->topic, msg->sender,
                               message_bus_message_data(msg), msg->data_size);
}

static bool topic_equals(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

static bool replay_topic_selected(const ReplayConfig* replay, const char* topic) {
    if (!replay || !replay->enabled) return false;
    if (!topic) return false;
    if (!replay->topic_filter || !replay->topic_filter[0] ||
        strcmp(replay->topic_filter, "*") == 0)
        return true;
    return strcmp(replay->topic_filter, topic) == 0;
}

static int replay_consumer_count(const char* topic) {
    int consumers = 0;
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].replay_disabled) continue;
        for (int j = 0; j < g_nodes[i].input_count; j++) {
            if (topic_equals(g_nodes[i].inputs[j], topic))
                consumers++;
        }
    }
    return consumers;
}

static int prepare_replay_nodes(ReplayConfig* replay,
                                char* error, size_t error_size) {
    if (!replay || !replay->enabled) return ERR_OK;
    if (!replay->bag_path) return ERR_INVALID_PARAM;
    replay->selected_topic_count = 0;

    BagReader* reader = bag_reader_open(replay->bag_path);
    if (!reader) {
        snprintf(error, error_size, "cannot open replay bag '%s'", replay->bag_path);
        return ERR_IO;
    }

    char bag_topics[64][64];
    int total_topics = bag_reader_get_topics(reader, bag_topics, 64, NULL);
    bag_reader_close(reader);
    if (total_topics < 0) {
        snprintf(error, error_size, "cannot enumerate replay topics from '%s'",
                 replay->bag_path);
        return ERR_IO;
    }

    int selected = 0;
    for (int i = 0; i < total_topics && selected < 64; i++) {
        if (!replay_topic_selected(replay, bag_topics[i])) continue;
        snprintf(replay->selected_topics[selected++], 64, "%s", bag_topics[i]);
    }

    replay->selected_topic_count = selected;
    if (selected == 0) {
        snprintf(error, error_size,
                 "replay topic filter matched no bag topics%s%s",
                 replay->topic_filter ? ": " : "",
                 replay->topic_filter ? replay->topic_filter : "");
        return ERR_NOT_FOUND;
    }

    for (int i = 0; i < g_node_count; i++) g_nodes[i].replay_disabled = false;

    for (int i = 0; i < g_node_count; i++) {
        NodeDesc* nd = &g_nodes[i];
        int overlap = 0;
        int blocking = 0;

        for (int out = 0; out < nd->output_count; out++) {
            bool supplied_by_bag = false;
            for (int rt = 0; rt < selected; rt++) {
                if (topic_equals(nd->outputs[out], replay->selected_topics[rt])) {
                    supplied_by_bag = true;
                    overlap++;
                    break;
                }
            }
            if (!supplied_by_bag && replay_consumer_count(nd->outputs[out]) > 0)
                blocking++;
        }

        if (overlap == 0) continue;
        if (blocking == 0) {
            nd->replay_disabled = true;
            LOG_INFO("launcher", "replay auto-skip node '%s' (bag supplies its live outputs)",
                     nd->name);
            continue;
        }

        char missing[256] = "";
        for (int out = 0; out < nd->output_count; out++) {
            bool supplied_by_bag = false;
            for (int rt = 0; rt < selected; rt++) {
                if (topic_equals(nd->outputs[out], replay->selected_topics[rt])) {
                    supplied_by_bag = true;
                    break;
                }
            }
            if (!supplied_by_bag && replay_consumer_count(nd->outputs[out]) > 0) {
                if (missing[0]) strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
                strncat(missing, nd->outputs[out], sizeof(missing) - strlen(missing) - 1);
            }
        }
        snprintf(error, error_size,
                 "replay topic(s) would duplicate live node '%s', but it also publishes required non-bag topic(s): %s",
                 nd->name, missing[0] ? missing : "(unknown)");
        return ERR_INVALID_PARAM;
    }

    return ERR_OK;
}

static int advertise_replay_topics(Transport* transport,
                                   BagReader* reader,
                                   char replay_topics[][64],
                                   int replay_topic_count) {
    if (!transport || !reader) return ERR_INVALID_PARAM;
    for (int i = 0; i < replay_topic_count; i++) {
        uint32_t type_id = 0;
        uint8_t schema_ver = 0;
        if (bag_reader_get_type_info(reader, replay_topics[i], &type_id, &schema_ver) != 0) {
            type_id = 0;
        }
        (void)schema_ver;
        int rc = transport_advertise(transport, replay_topics[i], type_id);
        if (rc != ERR_OK) return rc;
    }
    return ERR_OK;
}

static int run_replay(ReplayConfig* replay,
                      bool use_ipc,
                      MessageBus* bus,
                      Transport* transport,
                      int duration) {
    if (!replay || !replay->enabled || !replay->bag_path) return ERR_OK;

    BagReader* reader = bag_reader_open(replay->bag_path);
    if (!reader) {
        LOG_WARN("launcher", "replay: cannot open bag '%s'", replay->bag_path);
        return ERR_IO;
    }

    char error[512] = {0};
    int rc = prepare_replay_nodes(replay, error, sizeof(error));
    if (rc != ERR_OK) {
        LOG_WARN("launcher", "replay setup failed: %s", error[0] ? error : err_str(rc));
        bag_reader_close(reader);
        return rc;
    }

    rc = advertise_replay_topics(transport, reader,
                                 replay->selected_topics,
                                 replay->selected_topic_count);
    if (rc != ERR_OK) {
        LOG_WARN("launcher", "replay advertise failed: %s", err_str(rc));
        bag_reader_close(reader);
        return rc;
    }

    ReplayPublishCtx publish_ctx = {
        .use_ipc = use_ipc,
        .bus = bus,
        .transport = transport,
    };
    ReplayStopCtx stop_ctx = {
        .use_deadline = duration > 0,
        .deadline_wall_us = duration > 0
            ? clock_now_monotonic_wall_us() + (uint64_t)duration * 1000000ULL
            : 0,
    };
    BagReplayOptions options;
    memset(&options, 0, sizeof(options));
    options.bus = use_ipc ? NULL : bus;
    options.publish_fn = replay_publish_cb;
    options.publish_user_data = &publish_ctx;
    options.speed = replay->speed;
    options.topic_filter = replay->topic_filter;
    options.start_offset_us = replay->start_offset_us;
    options.end_offset_us = replay->end_offset_us;
    options.loop = replay->loop;
    /* 单进程回放也不驱动 sim clock：被驱动的 clock_now_us() 返回注入的消息时间戳
     * 而非单调墙钟，会让依赖 clock_now_us() 做 usleep 排程的节点(如 monitor 的
     * 60Hz topology 导出)在回放期间只导出一帧 → 3D 停在首帧。而回放时所有计算节点
     * (flowsim/planning/control/...) 都被 bag 输出覆盖而跳过，没有任何节点需要被
     * 驱动的 sim clock —— 与 --multi 子进程保持墙钟一致。重放消息自带时间戳
     * (scene/frame.t_us 等)，消费者无需 sim clock。 */
    options.drive_sim_clock = false;
    options.should_stop = replay_stop_requested;
    options.should_stop_user_data = &stop_ctx;

    LOG_INFO("launcher", "replay start: bag=%s rate=%.3fx topic=%s loop=%s mode=%s",
             replay->bag_path, replay->speed,
             replay->topic_filter && replay->topic_filter[0] ? replay->topic_filter : "*",
             replay->loop ? "on" : "off",
             use_ipc ? "ipc" : "local");

    uint64_t played = 0;
    rc = bag_reader_play_with_options(reader, &options, &played);
    if (!use_ipc) clock_set_sim_mode(false);
    bag_reader_close(reader);

    if (rc == ERR_OK) {
        LOG_INFO("launcher", "replay complete: %" PRIu64 " message(s)", played);
        usleep(200000);
    } else {
        LOG_WARN("launcher", "replay stopped: %s after %" PRIu64 " message(s)",
                 err_str(rc), played);
    }
    return rc;
}

static int parse_nonnegative_double(const char* text, double* out) {
    if (!text || !out) return ERR_INVALID_PARAM;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text || (end && *end != '\0') || value < 0.0) return ERR_INVALID_PARAM;
    *out = value;
    return ERR_OK;
}

static int seconds_to_us(double seconds, uint64_t* out_us) {
    if (!out_us || seconds < 0.0) return ERR_INVALID_PARAM;
    double micros = seconds * 1000000.0;
    if (micros > (double)UINT64_MAX) return ERR_OVERFLOW;
    *out_us = (uint64_t)micros;
    return ERR_OK;
}

/* ── dlopen 单进程模式 ─────────────────────────────────────── */

static int run_dlopen_mode(int duration, int stagger_ms,
                           MessageBus* bus, Transport* transport,
                           DiscoveryManager* discovery, Scheduler* scheduler,
                           ReplayConfig* replay) {

    LOG_INFO("launcher", "mode: dlopen (single-process)");

    /* 初始化所有节点插件 */
    for (int i = 0; i < g_node_count; i++) {
        NodeDesc* nd = &g_nodes[i];
        if (nd->replay_disabled) {
            LOG_INFO("launcher", "[%d/%d] skipped %-16s  (replay-supplied source)",
                     i + 1, g_node_count, nd->name);
            continue;
        }
        if (!nd->library[0]) {
            LOG_WARN("launcher", "node %s: no library path, skipping", nd->name);
            continue;
        }

        /* 尝试加载 .so — 支持绝对路径、相对路径和库名三种格式 */
        /* RTLD_GLOBAL: 把 .so 的符号暴露给后续加载的库 (依赖传递) */
        /* RTLD_LAZY:   推迟解析, 让 .so 能从主进程继承 net_transport* 等符号 */
        int dlflags = RTLD_LAZY | RTLD_GLOBAL;
        nd->lib_handle = dlopen(nd->library, dlflags);
        if (!nd->lib_handle) {
            /* 回退1: build/lib/<libname> */
            char alt[512];
            const char* basename = strrchr(nd->library, '/');
#if defined(_WIN32)
            const char* bslash = strrchr(nd->library, '\\');
            if (!basename || (bslash && bslash > basename)) basename = bslash;
#endif
            basename = basename ? basename + 1 : nd->library;
            snprintf(alt, sizeof(alt), "build/lib/%s", basename);
            nd->lib_handle = dlopen(alt, dlflags);
        }
#if defined(_WIN32)
        if (!nd->lib_handle) {
            char dll_name[256];
            const char* basename = strrchr(nd->library, '/');
            const char* bslash = strrchr(nd->library, '\\');
            if (!basename || (bslash && bslash > basename)) basename = bslash;
            basename = basename ? basename + 1 : nd->library;
            snprintf(dll_name, sizeof(dll_name), "%s", basename);
            size_t n = strlen(dll_name);
            if (n > 3 && strcmp(dll_name + n - 3, ".so") == 0) {
                dll_name[n - 3] = '\0';
                if (strncmp(dll_name, "lib", 3) == 0) {
                    memmove(dll_name, dll_name + 3, strlen(dll_name + 3) + 1);
                }
                strncat(dll_name, ".dll", sizeof(dll_name) - strlen(dll_name) - 1);
            }
            char alt[512];
            snprintf(alt, sizeof(alt), "build-win/lib/%s", dll_name);
            nd->lib_handle = dlopen(alt, dlflags);
        }
#endif
        if (!nd->lib_handle) {
            LOG_WARN("launcher", "dlopen %s failed: %s (skipping)", nd->library, dlerror());
            continue;
        }

        NodeGetPluginFn get_fn = (NodeGetPluginFn)dlsym(nd->lib_handle, NODE_PLUGIN_SYMBOL);
        if (!get_fn) {
            LOG_WARN("launcher", "%s: symbol '%s' not found", nd->library, NODE_PLUGIN_SYMBOL);
            dlclose(nd->lib_handle); nd->lib_handle = NULL;
            continue;
        }

        nd->plugin = get_fn();
        if (!nd->plugin) {
            LOG_WARN("launcher", "%s: node_get_plugin() returned NULL", nd->library);
            dlclose(nd->lib_handle); nd->lib_handle = NULL;
            continue;
        }

        /* ── ABI 版本校验 ──────────────────────────────────────
         * 防止加载与当前 NodePlugin 契约不兼容的旧/新插件而导致
         * 难以排查的运行时崩溃。 */
        if (nd->plugin->api_version == 0) {
            LOG_WARN("launcher",
                     "%s: plugin declares no ABI version (expected %u) — "
                     "loading as legacy, behavior may be undefined",
                     nd->library, NODE_PLUGIN_API_VERSION);
        } else if (nd->plugin->api_version != NODE_PLUGIN_API_VERSION) {
            LOG_WARN("launcher",
                     "%s: incompatible plugin ABI version %u (expected %u) — skipping",
                     nd->library, nd->plugin->api_version, NODE_PLUGIN_API_VERSION);
            nd->plugin = NULL;
            dlclose(nd->lib_handle); nd->lib_handle = NULL;
            continue;
        }

        if (nd->plugin->init(bus, transport, discovery, scheduler,
                             nd->params_json[0] ? nd->params_json : NULL) != 0) {
            LOG_WARN("launcher", "node %s init() failed", nd->name);
            nd->plugin = NULL;
            dlclose(nd->lib_handle); nd->lib_handle = NULL;
            continue;
        }

        LOG_INFO("launcher", "[%d/%d] loaded  %-16s  v%s — %s",
                 i + 1, g_node_count, nd->name,
                 nd->plugin->version, nd->plugin->description);
        usleep((unsigned)stagger_ms * 1000);
    }

    /* 启动所有已初始化的节点（留微小间隔避免多节点广播同一 topic 时
     * 在微秒级内连续 publish 导致频率估算出现尖峰，如 node_info 的 52kHz） */
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].replay_disabled) continue;
        if (g_nodes[i].plugin) {
            g_nodes[i].plugin->start();
            LOG_INFO("launcher", "  started %s", g_nodes[i].name);
            usleep(5000);  /* 5ms stagger — 总计 <50ms，用户无感 */
        }
    }

    LOG_INFO("launcher", "all nodes running (%ds) — dashboard: http://localhost:8800", duration);

    /* 参数服务：必须在所有 init() 之后启动 —— 节点的 param_register_* 经符号
     * interposition 写进本进程这份 registry，此刻才齐全。启动失败不致命，
     * 只是失去远程调参能力，pipeline 照跑。 */
    ParamBridgeServer* param_srv = param_bridge_server_start(NULL);
    if (param_srv)
        LOG_INFO("launcher", "param bridge listening (%d params) — "
                 "flowctl param set <name> <value>", param_count());
    else
        LOG_WARN("launcher", "param bridge failed to start — remote tuning unavailable");

    if (replay && replay->enabled) {
        int replay_rc = run_replay(replay, false, bus, transport, duration);
        if (replay_rc != ERR_OK) g_running = 0;
    } else {
        /* 等待运行时间或信号: duration ≤ 0 表示持续运行直到 Ctrl+C */
        if (duration > 0) {
            for (int t = 0; t < duration && g_running; t++) sleep(1);
        } else {
            LOG_INFO("launcher", "running indefinitely — press Ctrl+C to stop");
            while (g_running) sleep(1);
        }
    }

    /* 优雅停止 */
    LOG_INFO("launcher", "stopping nodes...");
    /* 先停参数服务：之后 registry 里的值不会再被外部改动，节点可以安心退出。 */
    param_bridge_server_stop(param_srv);
    for (int i = g_node_count - 1; i >= 0; i--) {
        if (g_nodes[i].replay_disabled) continue;
        if (g_nodes[i].plugin) { g_nodes[i].plugin->stop(); }
    }
    sleep(1);  /* 给线程时间退出 */
    for (int i = g_node_count - 1; i >= 0; i--) {
        if (g_nodes[i].replay_disabled) continue;
        if (g_nodes[i].plugin) {
            g_nodes[i].plugin->cleanup();
            LOG_INFO("launcher", "  stopped %s", g_nodes[i].name);
        }
        /* NOTE: dlclose 延迟到 message_bus_destroy 之后执行。
         * MessageBus 的 dispatch 线程在 bus 销毁前仍在运行，可能调用
         * 节点注册的订阅回调；若提前 dlclose 卸载了 .so 代码段，
         * dispatch 线程就会跳转到已解除映射的内存 → SIGSEGV。 */
    }
    return 0;
}

/* ── fork+exec 多进程模式 ─────────────────────────────────── */

static pid_t launch_node_process(const NodeDesc* nd, const char* self_exe,
                                 const char* config_path, int duration) {
#if defined(_WIN32)
    (void)nd; (void)self_exe; (void)config_path; (void)duration;
    LOG_WARN("launcher", "--multi is not supported on native Windows; use single-process mode");
    return -1;
#else
    pid_t pid = fork();
    if (pid < 0) { LOG_WARN("launcher", "fork %s: %s", nd->name, strerror(errno)); return -1; }
    if (pid == 0) {
        char dur_str[16];
        snprintf(dur_str, sizeof(dur_str), "%d", duration);
        /* exec flow_node_host (与 launcher 同目录): 每个节点在独立进程内
         * dlopen 自己的 .so 并走 IPC 桥接。复用与单进程模式相同的节点插件,
         * 不再依赖已移除的 flow_e2e --role 单体 demo。 */
        char host_path[512];
        const char* slash = strrchr(self_exe, '/');
        /* 用 snprintf 从「目录前缀 + 可执行文件名」安全拼接, 避免 strcpy 越界。 */
        int n;
        if (slash) {
            int dir_len = (int)(slash + 1 - self_exe);  /* 含末尾 '/' */
            n = snprintf(host_path, sizeof(host_path), "%.*sflow_node_host", dir_len, self_exe);
        } else {
            n = snprintf(host_path, sizeof(host_path), "./flow_node_host");
        }
        if (n < 0 || n >= (int)sizeof(host_path)) {
            LOG_WARN("launcher", "node_host path too long for %s", nd->name);
            _exit(1);
        }
        char* args[] = { host_path, (char*)config_path, (char*)nd->name, dur_str, NULL };
        execv(host_path, args);
        perror("execv"); _exit(1);
    }
    return pid;
#endif
}

static int run_multi_process_mode(int duration, int stagger_ms,
                                  const char* self_exe, const char* config_path,
                                  ReplayConfig* replay,
                                  MessageBus* replay_bus,
                                  Transport* replay_transport) {
#if defined(_WIN32)
    (void)duration; (void)stagger_ms; (void)self_exe; (void)config_path;
    LOG_WARN("launcher", "--multi is not supported on native Windows; use single-process mode");
    return 1;
#else
    LOG_INFO("launcher", "mode: multi-process (fork+exec flow_node_host)");
    for (int i = 0; i < g_node_count && g_running; i++) {
        NodeDesc* nd = &g_nodes[i];
        if (nd->replay_disabled) {
            LOG_INFO("launcher", "[%d/%d] skipped %-16s  (replay-supplied source)",
                     i + 1, g_node_count, nd->name);
            continue;
        }
        nd->pid = launch_node_process(nd, self_exe, config_path, duration);
        if (nd->pid > 0)
            LOG_INFO("launcher", "[%d/%d] started %-16s  pid=%d", i+1, g_node_count, nd->name, (int)nd->pid);
        usleep((unsigned)stagger_ms * 1000);
    }
    LOG_INFO("launcher", "all nodes started (%ds) — dashboard: http://localhost:8800", duration);
    if (replay && replay->enabled && replay_bus && replay_transport && g_running) {
        usleep(300000);  /* let subscriber hosts finish transport_subscribe() */
        int replay_rc = run_replay(replay, true, replay_bus, replay_transport, duration);
        if (replay_rc != ERR_OK) LOG_WARN("launcher", "multi-process replay exited with %s", err_str(replay_rc));
        g_running = 0;
    }
    while (g_running) {
        int status;
        pid_t done = waitpid(-1, &status, WNOHANG);
        if (done < 0 && errno == ECHILD) break;
        usleep(200000);
    }
    for (int i = 0; i < g_node_count; i++)
        if (g_nodes[i].pid > 0) kill(g_nodes[i].pid, SIGTERM);
    for (int i = 0; i < g_node_count; i++)
        if (g_nodes[i].pid > 0) { waitpid(g_nodes[i].pid, NULL, 0); g_nodes[i].pid = -1; }
    return 0;
#endif
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    fp_env_init();  /* FTZ/DAZ：防 denormal 进 JSON 触发 glibc strtod 断言 */
    const char* config_path = "config/pipeline.json";
    const char* bag_path    = NULL;
    ReplayConfig replay = {
        .speed = 1.0,
    };
    int duration = 0;
    int multi_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bag") == 0 && i + 1 < argc) bag_path = argv[++i];
        else if (strcmp(argv[i], "--replay-bag") == 0 && i + 1 < argc) {
            replay.bag_path = argv[++i];
            replay.enabled = true;
        }
        else if (strcmp(argv[i], "--replay-rate") == 0 && i + 1 < argc) {
            if (parse_nonnegative_double(argv[++i], &replay.speed) != ERR_OK) {
                fprintf(stderr, "invalid --replay-rate: %s\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--replay-topic") == 0 && i + 1 < argc) replay.topic_filter = argv[++i];
        else if (strcmp(argv[i], "--replay-start") == 0 && i + 1 < argc) {
            double seconds = 0.0;
            if (parse_nonnegative_double(argv[++i], &seconds) != ERR_OK ||
                seconds_to_us(seconds, &replay.start_offset_us) != ERR_OK) {
                fprintf(stderr, "invalid --replay-start: %s\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--replay-end") == 0 && i + 1 < argc) {
            double seconds = 0.0;
            if (parse_nonnegative_double(argv[++i], &seconds) != ERR_OK ||
                seconds_to_us(seconds, &replay.end_offset_us) != ERR_OK) {
                fprintf(stderr, "invalid --replay-end: %s\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--replay-loop") == 0) replay.loop = true;
        else if (strcmp(argv[i], "--multi") == 0) multi_mode = 1;
        else if (argv[i][0] != '-') config_path = argv[i];
    }

    if (bag_path && replay.enabled) {
        fprintf(stderr, "--bag recording and --replay-bag are mutually exclusive\n");
        return 1;
    }
    if (replay.enabled && replay.end_offset_us > 0 &&
        replay.end_offset_us < replay.start_offset_us) {
        fprintf(stderr, "--replay-end must be >= --replay-start\n");
        return 1;
    }

    /* 按模块名写独立日志文件：launcher → /tmp/flow_logs/launcher.log
     * 单进程模式(dlopen)下各节点通过 log_set_module_dir 写入
     * /tmp/flow_logs/{module}.log。多进程模式下每个 flow_node_host
     * 各自写独立日志文件。环境变量 FLOW_LOG_DIR 可覆盖日志目录。 */
    const char* log_dir = getenv("FLOW_LOG_DIR");
    if (!log_dir || !log_dir[0]) log_dir = "/tmp/flow_logs";
    mkdir(log_dir, 0755);
    char launcher_log[320];
    snprintf(launcher_log, sizeof(launcher_log), "%s/launcher.log", log_dir);
    log_init(LOG_INFO, launcher_log);

    /* 单进程模式：设置模块目录，各节点 LOG_* 自动写入 {module}.log */
    if (!multi_mode) {
        log_set_module_dir(log_dir);
    }

    crash_handler_install();

#if defined(_WIN32)
    LOG_INFO("launcher", "FlowEngine Launcher v2");
    LOG_INFO("launcher", "config: %s", config_path);
    LOG_INFO("launcher", "mode: %s", multi_mode ? "multi-process (fork+exec)" : "single-process (dlopen)");
    LOG_INFO("launcher", "duration: %ds", duration);
#else
    LOG_INFO("launcher", "╔══════════════════════════════════════════╗");
    LOG_INFO("launcher", "║  FlowEngine Launcher v2                  ║");
    LOG_INFO("launcher", "║  config:   %-29s ║", config_path);
    LOG_INFO("launcher", "║  mode:     %-29s ║", multi_mode ? "multi-process (fork+exec)" : "single-process (dlopen)");
    LOG_INFO("launcher", "║  duration: %-29ds ║", duration);
    LOG_INFO("launcher", "╚══════════════════════════════════════════╝");
#endif

    int stagger_ms = 300;
    if (parse_pipeline(config_path, &stagger_ms) <= 0) {
        LOG_WARN("launcher", "no nodes found in %s", config_path);
        log_shutdown(); return 1;
    }
    if (replay.enabled) {
        char error[512] = {0};
        int rc = prepare_replay_nodes(&replay, error, sizeof(error));
        if (rc != ERR_OK) {
            LOG_WARN("launcher", "replay setup failed: %s", error[0] ? error : err_str(rc));
            log_shutdown();
            return 1;
        }
        LOG_INFO("launcher", "replay prepared: %d bag topic(s) selected", replay.selected_topic_count);
    }

    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);

    if (multi_mode) {
#if !defined(_WIN32)
        signal(SIGCHLD, SIG_DFL);
#endif
        MessageBus* replay_bus = NULL;
        Transport* replay_transport = NULL;
        if (replay.enabled) {
            replay_bus = message_bus_create("launcher_replay_ipc");
            replay_transport = replay_bus ? transport_create(replay_bus, NULL, TRANSPORT_IPC) : NULL;
            if (!replay_bus || !replay_transport || transport_start(replay_transport) != ERR_OK) {
                LOG_WARN("launcher", "failed to initialize IPC replay injector");
                if (replay_transport) transport_destroy(replay_transport);
                if (replay_bus) message_bus_destroy(replay_bus);
                log_shutdown();
                return 1;
            }
            BagReader* replay_reader = bag_reader_open(replay.bag_path);
            if (!replay_reader ||
                advertise_replay_topics(replay_transport, replay_reader,
                                        replay.selected_topics,
                                        replay.selected_topic_count) != ERR_OK) {
                LOG_WARN("launcher", "failed to pre-advertise replay IPC topics");
                if (replay_reader) bag_reader_close(replay_reader);
                transport_stop(replay_transport);
                transport_destroy(replay_transport);
                message_bus_destroy(replay_bus);
                log_shutdown();
                return 1;
            }
            bag_reader_close(replay_reader);
        }

        run_multi_process_mode(duration, stagger_ms, argv[0], config_path,
                               &replay, replay_bus, replay_transport);
        if (replay_transport) {
            transport_stop(replay_transport);
            transport_destroy(replay_transport);
        }
        if (replay_bus) message_bus_destroy(replay_bus);
    } else {
        /* dlopen 模式: 需要初始化基础设施 */
        adas_msgs_register_all();

        /* 注册 pipeline 节点到 flow_registry */
        for (int i = 0; i < g_node_count; i++) {
            NodeDesc* nd = &g_nodes[i];
            /* 构建 NULL 结尾的 inputs/outputs 数组 */
            const char* inputs[MAX_TOPICS_PER_NODE + 1];
            const char* outputs[MAX_TOPICS_PER_NODE + 1];
            int ni = nd->input_count < MAX_TOPICS_PER_NODE ? nd->input_count : MAX_TOPICS_PER_NODE;
            int no = nd->output_count < MAX_TOPICS_PER_NODE ? nd->output_count : MAX_TOPICS_PER_NODE;
            for (int j = 0; j < ni; j++) inputs[j] = nd->inputs[j];
            inputs[ni] = NULL;
            for (int j = 0; j < no; j++) outputs[j] = nd->outputs[j];
            outputs[no] = NULL;
            (void)flow_registry_register_task(nd->name, nd->name, nd->library,
                                        ni > 0 ? inputs : NULL,
                                        no > 0 ? outputs : NULL,
                                        NULL);
            /* 注册 input/output topics */
            for (int j = 0; j < ni; j++) {
                flow_registry_register_topic(nd->inputs[j], 0, NULL);
            }
            for (int j = 0; j < no; j++) {
                flow_registry_register_topic(nd->outputs[j], 0, NULL);
            }
            /* 注册 plugin: 关联此节点的 task name */
            const char* tasks[2] = { nd->name, NULL };
            flow_registry_register_plugin(nd->name, nd->library, tasks, NULL);
        }
        LOG_INFO("launcher", "registry: %d total entries", flow_registry_total_count());

        MessageBus* bus       = message_bus_create("launcher_bus");

        /* Bag recording (optional: --bag /path/to/output.bag) */
        BagWriter* bag_writer = NULL;
        if (bag_path) {
            bag_writer = bag_writer_open(bag_path);
            if (bag_writer) {
                bag_writer_attach(bag_writer, bus);
                LOG_INFO("launcher", "bag recording: %s", bag_path);
            }
        }

        /* 从配置加载 QoS 与调度器模式并应用 */
        int cfg_sched_mode = SCHEDULER_MODE_CHOREO;  /* 无配置时保持历史默认 */
        LauncherConfig* qos_cfg = config_load(config_path);
        if (qos_cfg) {
            cfg_sched_mode = qos_cfg->scheduler.mode;  /* 0=classic 1=choreo */
            int qos_count = 0;
            for (int i = 0; i < qos_cfg->process_count; i++) {
                ProcessConfig* pc = &qos_cfg->processes[i];
                for (int k = 0; k < pc->publish_count; k++) {
                    TopicDecl* td = &pc->publish[k];
                    if (td->qos_depth > 0 || td->qos_policy[0]) {
                        TopicQos tq = {0};
                        tq.depth       = (uint32_t)(td->qos_depth > 0 ? td->qos_depth : 8);
                        tq.policy      = strcmp(td->qos_policy, "block") == 0 ? QOS_BLOCK :
                                         strcmp(td->qos_policy, "drop_latest") == 0 ? QOS_DROP_LATEST :
                                         QOS_DROP_OLDEST;
                        tq.reliability = strcmp(td->qos_reliability, "reliable") == 0 ? QOS_RELIABLE : QOS_BEST_EFFORT;
                        tq.deadline_ms = (uint32_t)(td->qos_deadline_ms > 0 ? td->qos_deadline_ms : 0);
                        tq.lifespan_ms = (uint32_t)(td->qos_lifespan_ms > 0 ? td->qos_lifespan_ms : 0);
                        message_bus_set_topic_qos(bus, td->topic, &tq);
                        qos_count++;
                    }
                }
            }
            if (qos_count > 0)
                LOG_INFO("launcher", "QoS applied to %d topics", qos_count);
            config_free(qos_cfg);
        }

        DiscoveryManager* discovery = discovery_create("flow_launcher",
            CAP_PUBLISHER | CAP_SUBSCRIBER | CAP_FUSION);
        discovery_start(discovery);
        /* TRANSPORT_LOCAL: 单进程内节点共享本地总线，不需要 TCP/IPC */
        Transport*        transport = transport_create(bus, discovery, TRANSPORT_LOCAL);
        transport_start(transport);
        SchedulerConfig   scfg      = SCHEDULER_CONFIG_DEFAULT;
        scfg.mode                  = (SchedulerMode)cfg_sched_mode;  /* 读配置，不再写死 */
        LOG_INFO("launcher", "scheduler mode: %s",
                 scfg.mode == SCHEDULER_MODE_CHOREO ? "choreo" : "classic");
        Scheduler*        scheduler = scheduler_create(&scfg);
        scheduler_set_choreo_bus(scheduler, bus);
        scheduler_start(scheduler);

        run_dlopen_mode(duration, stagger_ms, bus, transport, discovery, scheduler, &replay);

        if (bag_writer) {
            bag_writer_close(bag_writer);
            LOG_INFO("launcher", "bag saved: %s", bag_path);
        }

        LOG_INFO("launcher", "stopping scheduler...");
        scheduler_stop(scheduler);  scheduler_destroy(scheduler);
        LOG_INFO("launcher", "stopping transport...");
        transport_stop(transport);  transport_destroy(transport);
        LOG_INFO("launcher", "stopping discovery...");
        discovery_stop(discovery);  discovery_destroy(discovery);
        LOG_INFO("launcher", "destroying message bus...");
        message_bus_destroy(bus);
        LOG_INFO("launcher", "message bus destroyed");

        /* dlclose 必须在 message_bus_destroy 之后：dispatch 线程已停止，
         * 不会再调用任何节点回调，此时卸载 .so 代码段是安全的。
         * Windows: FreeLibrary 在 C++ 静态析构/残留线程时可能死锁挂起；
         * 进程即将退出，OS 回收映像，跳过显式卸载。 */
#if defined(_WIN32)
        for (int i = 0; i < g_node_count; i++) {
            g_nodes[i].lib_handle = NULL;
        }
        LOG_INFO("launcher", "skip dlclose on Windows (process exit reclaim)");
#else
        for (int i = 0; i < g_node_count; i++) {
            if (g_nodes[i].lib_handle) { dlclose(g_nodes[i].lib_handle); g_nodes[i].lib_handle = NULL; }
        }
#endif
        LOG_INFO("launcher", "shutdown complete");
    }

    log_shutdown();
    return 0;
}
