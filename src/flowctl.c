/**
 * flowctl.c — FlowEngine 命令行工具
 *
 * 用法:
 *   flowctl list tasks           列出所有注册的 task
 *   flowctl list topics          列出所有 topic 及统计
 *   flowctl list plugins         列出加载的插件
 *   flowctl graph                打印拓扑图
 *   flowctl state <task>         查看 task 状态机
 *   flowctl topic stats <topic>  查看 topic 统计
 *   flowctl bag info <file>      查看 bag 文件信息
 *   flowctl schema <type>        查看类型 schema
 *   flowctl version              版本信息
 *   flowctl dashboard            启动实时仪表盘
 */

#include "message_bus.h"
#include "discovery.h"
#include "serializer.h"
#include "state_machine.h"
#include "bag.h"
#include "logger.h"
#include "scheduler.h"
#include "flow_registry.h"
#include "param_registry.h"
#include "param_bridge.h"
#include "config_manager.h"
#include "error_codes.h"
#include "adas_msgs_gen.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#define FLOWENGINE_VERSION "1.0.0"
#define FLOWCTL_MAX_TOPICS 64

static cJSON* read_state_json(void) {
    FILE* f = fopen(flowengine_state_file(), "r");
    if (!f) return NULL;
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

static const cJSON* find_topics_array(const cJSON* root) {
    const cJSON* topics = cJSON_GetObjectItemCaseSensitive(root, "topics");
    if (cJSON_IsArray(topics)) return topics;
    const cJSON* metrics = cJSON_GetObjectItemCaseSensitive(root, "metrics");
    if (cJSON_IsObject(metrics)) {
        topics = cJSON_GetObjectItemCaseSensitive(metrics, "topics");
        if (cJSON_IsArray(topics)) return topics;
    }
    return NULL;
}

static void collect_json_strings_by_key(const cJSON* node, const char* key,
                                        char out[][64], int* count, int max_count) {
    if (!node || *count >= max_count) return;
    if (cJSON_IsObject(node)) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(node, key);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
            snprintf(out[*count], 64, "%s", item->valuestring);
            (*count)++;
        }
        for (const cJSON* child = node->child; child && *count < max_count; child = child->next) {
            collect_json_strings_by_key(child, key, out, count, max_count);
        }
    } else if (cJSON_IsArray(node)) {
        for (const cJSON* child = node->child; child && *count < max_count; child = child->next) {
            collect_json_strings_by_key(child, key, out, count, max_count);
        }
    }
}

/* ── 帮助 ────────────────────────────────────────────────── */

static void print_usage(void) {
    printf("FlowEngine CLI v%s — Interactive command-line tool\n\n", FLOWENGINE_VERSION);
    printf("Usage: flowctl <command> [args]\n\n");
    printf("Commands:\n");
    printf("  list tasks              List registered tasks\n");
    printf("  list topics             List all topics with stats\n");
    printf("  list types              List registered message types\n");
    printf("  list plugins            List loaded plugins\n");
    printf("  list params             List registered parameters\n");
    printf("  graph                   Print topology graph\n");
    printf("  state <task>            Show task state machine\n");
    printf("  topic stats <topic>     Per-topic statistics\n");
    printf("  bag info <file>         Bag file metadata\n");
    printf("  bag play <file>         Launch replay pipeline from a bag\n");
    printf("  schema <type>           Type information\n");
    printf("  param list              List live params from the running process\n");
    printf("  param get <name>        Read a param from the running process\n");
    printf("  param set <name> <val>  Change a param live (takes effect next frame)\n");
    printf("  launch <config>         Validate and show launch config\n");
    printf("  doctor [args]           Run unified self-diagnosis (flow_doctor.py)\n");
    printf("  dashboard               Start real-time dashboard\n");
    printf("  version                 Show version\n");
    printf("  help                    This help\n");
    printf("\n");
    printf("Replay:\n");
    printf("  flowctl bag play <file> [--config config/pipeline.json] [--multi]\n");
    printf("                       [--rate 1.0] [--topic <topic>] [--start <sec>]\n");
    printf("                       [--end <sec>] [--loop] [--duration <sec>]\n");
    printf("  Note: attaching to an already-running pipeline is unsupported;\n");
    printf("        launch replay through flow_launcher instead.\n");
}

static void print_version(void) {
    printf("FlowEngine v%s\n", FLOWENGINE_VERSION);
    printf("  Build:  " __DATE__ " " __TIME__ "\n");
    printf("  Modules: bus, ipc, bag, clock, discovery, fusion, scheduler, serializer, statem, transport\n");
}

/* ── list tasks ──────────────────────────────────────────── */

static int cmd_list_tasks(void) {
    printf("Tasks:\n");
    printf("  %-20s %-30s %s\n", "NAME", "DESCRIPTION", "I/O");
    printf("  %-20s %-30s %s\n", "────", "───────────", "───");

    /* Try JSON file first */
    cJSON* root = read_state_json();
    if (root) {
        const cJSON* reg = cJSON_GetObjectItemCaseSensitive(root, "registry");
        const cJSON* tasks = cJSON_IsObject(reg) ? cJSON_GetObjectItemCaseSensitive(reg, "tasks") : NULL;
        if (cJSON_IsArray(tasks)) {
            int count = 0;
            const cJSON* item = NULL;
            cJSON_ArrayForEach(item, tasks) {
                if (count >= 20) break;
                const cJSON* jname = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (!cJSON_IsString(jname) || !jname->valuestring) continue;
                const cJSON* jdesc = cJSON_GetObjectItemCaseSensitive(item, "desc");
                const char* desc = (cJSON_IsString(jdesc) && jdesc->valuestring) ? jdesc->valuestring : "";
                printf("  %-20s %-30s %s\n", jname->valuestring, desc, "—");
                count++;
            }
            printf("\n  Total: %d tasks (from registry)\n", count);
            cJSON_Delete(root);
            return 0;
        }
        cJSON_Delete(root);
    }

    /* Fallback: local registry */
    TaskMeta tasks[64];
    int n = flow_registry_list_tasks(tasks, 64);
    for (int i = 0; i < n; i++)
        printf("  %-20s %-30s in:%d out:%d\n", tasks[i].name, tasks[i].description, tasks[i].input_count, tasks[i].output_count);
    printf("\n  Total: %d tasks\n", n);
    return 0;
}

/* ── list topics ─────────────────────────────────────────── */

static int cmd_list_topics(void) {
    printf("Topics:\n");
    printf("  %-30s %-8s %-10s %s\n", "TOPIC", "TYPE_ID", "QOS", "FREQ");
    printf("  %-30s %-8s %-10s %s\n", "─────", "───────", "───", "────");

    /* Primary: query FlowRegistry */
    TopicMeta topics[FLOWCTL_MAX_TOPICS];
    int n = flow_registry_list_topics(topics, FLOWCTL_MAX_TOPICS);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            printf("  %-30s 0x%08x  depth=%-3d %s%.0f Hz\n",
                   topics[i].name, topics[i].type_id,
                   topics[i].qos.depth,
                   topics[i].qos.reliability == 1 ? "reliable " : "",
                   0.0);  /* freq from live stats, not registry */
        }
        printf("\n  Total: %d topics (from registry)\n", n);
        return 0;
    }

    /* Fallback: parse state file for live stats */
    cJSON* root = read_state_json();
    if (!root) {
        printf("  (no data — start flow_launcher first)\n");
        return 0;
    }

    int tcount = 0;
    char seen[FLOWCTL_MAX_TOPICS][64];
    const cJSON* topics_arr = find_topics_array(root);
    if (topics_arr) {
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, topics_arr) {
            if (tcount >= FLOWCTL_MAX_TOPICS) break;
            const cJSON* jtopic = cJSON_GetObjectItemCaseSensitive(item, "topic");
            if (!cJSON_IsString(jtopic) || !jtopic->valuestring) continue;
            const char* tname = jtopic->valuestring;
            /* Deduplicate */
            int dup = 0;
            for (int j = 0; j < tcount; j++) {
                if (strcmp(seen[j], tname) == 0) { dup = 1; break; }
            }
            if (!dup) {
                snprintf(seen[tcount], 64, "%s", tname);
                const cJSON* jfreq = cJSON_GetObjectItemCaseSensitive(item, "freq");
                double freq = cJSON_IsNumber(jfreq) ? jfreq->valuedouble : 0.0;
                printf("  %-30s %-8s %-10s %s%.1f Hz\n",
                       tname, "—", "—",
                       freq > 0 ? "" : "—", freq);
                tcount++;
            }
        }
    }
    cJSON_Delete(root);
    printf("\n  Total: %d topics (from state file)\n", tcount);
    return 0;
}

/* ── list types ──────────────────────────────────────────── */

static int cmd_list_types(void) {
    /* Populate registry via serializer notifications (same as cmd_schema) */
    adas_msgs_register_all();

    printf("Types:\n");
    printf("  %-30s %-12s %s\n", "NAME", "TYPE_ID", "SIZE");
    printf("  %-30s %-12s %s\n", "────", "───────", "────");
    FlowTypeMeta types[64];
    int n = flow_registry_list_types(types, 64);
    if (n == 0) {
        printf("  (none registered — start flow_launcher first)\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        printf("  %-30s 0x%08x   %zu bytes\n",
               types[i].name, types[i].type_id, types[i].struct_size);
    printf("\n  Total: %d types (from registry)\n", n);
    return 0;
}

/* ── list params ─────────────────────────────────────────── */

static int cmd_list_params(void) {
    printf("Params:\n");
    printf("  %-28s %-8s %-12s %s\n", "NAME", "TYPE", "VALUE", "DESC");
    printf("  %-28s %-8s %-12s %s\n", "────", "────", "─────", "────");
    FlowParamMeta params[64];
    int n = flow_registry_list_params(params, 64);
    for (int i = 0; i < n; i++) {
        const char* type_str = params[i].type == (int)PARAM_INT   ? "int" :
                               params[i].type == (int)PARAM_FLOAT ? "float" :
                               params[i].type == (int)PARAM_BOOL  ? "bool" : "str";
        printf("  %-28s %-8s %-12s %s%s\n", params[i].name, type_str,
               params[i].value_str, params[i].description,
               params[i].hot_reload ? " 🔥" : "");
    }
    printf("\n  Total: %d params (from registry)\n", n);
    return 0;
}

/* ── graph ───────────────────────────────────────────────── */

static int cmd_graph(void) {
    printf("Topology:\n\n");

    cJSON* root = read_state_json();
    if (!root) {
        printf("  (no data)\n");
        return 0;
    }

    /* Parse and build simple graph */
    char nodes[32][64] = {{0}};
    int ncount = 0;
    collect_json_strings_by_key(root, "name", nodes, &ncount, 32);

    /* Simple ASCII rendering */
    for (int i = 0; i < ncount; i++) {
        printf("  [%s]", nodes[i]);
        if (i < ncount - 1) printf(" ──→ ");
    }
    printf("\n\n");

    /* Topic edges */
    char topics[FLOWCTL_MAX_TOPICS][64] = {{0}};
    int tcount = 0;
    collect_json_strings_by_key(root, "topic", topics, &tcount, FLOWCTL_MAX_TOPICS);
    char prev_topic[64] = "";
    for (int i = 0; i < tcount; i++) {
        if (topics[i][0] && strcmp(topics[i], prev_topic) != 0) {
            printf("    ├─ %s\n", topics[i]);
            snprintf(prev_topic, sizeof(prev_topic), "%.63s", topics[i]);
        }
    }
    cJSON_Delete(root);

    printf("\n  Run 'flowctl dashboard' for interactive topology viewer\n");
    return 0;
}

/* ── state ───────────────────────────────────────────────── */

static int cmd_state(const char* task_name) {
    if (!task_name) {
        fprintf(stderr, "Usage: flowctl state <task_name>\n");
        return 1;
    }

    /* Try registry first */
    const TaskMeta* tm = flow_registry_get_task(task_name);
    if (tm) {
        printf("Task: %s\n", tm->name);
        printf("  Description: %s\n", tm->description);
        printf("  Plugin:      %s\n", tm->plugin[0] ? tm->plugin : "(none)");
        printf("  Inputs:      %d topic(s)\n", tm->input_count);
        for (int j = 0; j < tm->input_count; j++)
            printf("    ← %s\n", tm->inputs[j]);
        printf("  Outputs:     %d topic(s)\n", tm->output_count);
        for (int j = 0; j < tm->output_count; j++)
            printf("    → %s\n", tm->outputs[j]);
        return 0;
    }

    /* Fallback: show standard lifecycle diagram */
    printf("State Machine: %s\n\n", task_name);
    printf("  transitions:\n");
    printf("    INITIALIZED + START   → RUNNING\n");
    printf("    RUNNING     + STOP    → STOPPING\n");
    printf("    STOPPING    + DONE    → STOPPED\n");
    printf("    RUNNING     + PAUSE   → PAUSED\n");
    printf("    PAUSED      + RESUME  → RUNNING\n");
    printf("    RUNNING     + ERROR   → ERROR\n");
    printf("    ERROR       + RESTART → INITIALIZED\n");
    printf("\n  (start flow_launcher for live task state)\n");
    return 0;
}

/* ── topic stats ──────────────────────────────────────────── */

static int cmd_topic_stats(const char* topic) {
    if (!topic) {
        fprintf(stderr, "Usage: flowctl topic stats <topic_name>\n");
        return 1;
    }

    /* Create a local bus to demonstrate the API */
    MessageBus* bus = message_bus_create("flowctl_query");
    if (!bus) { fprintf(stderr, "Error: cannot create bus\n"); return 1; }

    /* Try to read stats from discovery JSON first */
    cJSON* root = read_state_json();
    if (root) {
        printf("Topic: %s\n\n", topic);

        const cJSON* topics_arr = find_topics_array(root);
        if (topics_arr) {
            const cJSON* item = NULL;
            cJSON_ArrayForEach(item, topics_arr) {
                const cJSON* jtopic = cJSON_GetObjectItemCaseSensitive(item, "topic");
                if (!cJSON_IsString(jtopic) || !jtopic->valuestring) continue;
                if (strcmp(jtopic->valuestring, topic) == 0) {
                    const cJSON* jpub  = cJSON_GetObjectItemCaseSensitive(item, "pub");
                    const cJSON* jdel  = cJSON_GetObjectItemCaseSensitive(item, "del");
                    const cJSON* jdrop = cJSON_GetObjectItemCaseSensitive(item, "drop");
                    const cJSON* jlat  = cJSON_GetObjectItemCaseSensitive(item, "lat_us");
                    const cJSON* jp50  = cJSON_GetObjectItemCaseSensitive(item, "p50_us");
                    const cJSON* jp99  = cJSON_GetObjectItemCaseSensitive(item, "p99_us");
                    const cJSON* jfreq = cJSON_GetObjectItemCaseSensitive(item, "freq");
                    const cJSON* jsubs = cJSON_GetObjectItemCaseSensitive(item, "subs");

                    unsigned long pub     = cJSON_IsNumber(jpub)  ? (unsigned long)jpub->valuedouble  : 0;
                    unsigned long del     = cJSON_IsNumber(jdel)  ? (unsigned long)jdel->valuedouble  : 0;
                    unsigned long drop    = cJSON_IsNumber(jdrop) ? (unsigned long)jdrop->valuedouble : 0;
                    unsigned long lat_avg = cJSON_IsNumber(jlat)  ? (unsigned long)jlat->valuedouble  : 0;
                    unsigned long p50     = cJSON_IsNumber(jp50)  ? (unsigned long)jp50->valuedouble  : 0;
                    unsigned long p99     = cJSON_IsNumber(jp99)  ? (unsigned long)jp99->valuedouble  : 0;
                    double        freq    = cJSON_IsNumber(jfreq) ? jfreq->valuedouble                : 0.0;
                    unsigned int  subs    = cJSON_IsNumber(jsubs) ? (unsigned int)jsubs->valuedouble  : 0;

                    printf("  %-22s %lu\n",      "Published:",    pub);
                    printf("  %-22s %lu\n",      "Delivered:",    del);
                    printf("  %-22s %lu\n",      "Dropped:",      drop);
                    printf("  %-22s %u\n",       "Subscribers:",  subs);
                    printf("  %-22s %.1f Hz\n",  "Frequency:",    freq);
                    printf("  %-22s %lu µs\n",   "Avg latency:",  lat_avg);
                    printf("  %-22s %lu µs\n",   "P50 latency:",  p50);
                    printf("  %-22s %lu µs\n",   "P99 latency:",  p99);
                    cJSON_Delete(root);
                    message_bus_destroy(bus);
                    return 0;
                }
            }
        }
        cJSON_Delete(root);
        printf("  (topic not found in state file)\n");
        message_bus_destroy(bus);
        return 0;
    }

    message_bus_destroy(bus);
    printf("Topic: %s\n\n", topic);
    printf("  (no data source — start flow_launcher first)\n");
    return 0;
}

/* ── bag info ─────────────────────────────────────────────── */

static int cmd_bag_info(const char* path) {
    if (!path) {
        fprintf(stderr, "Usage: flowctl bag info <file.bag>\n");
        return 1;
    }

    BagReader* r = bag_reader_open(path);
    if (!r) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return 1;
    }

    uint64_t msg_count = 0, duration_us = 0;
    bag_reader_info(r, &msg_count, &duration_us);

    printf("Bag: %s\n\n", path);
    printf("  Messages:   %" PRIu64 "\n", msg_count);
    printf("  Duration:   %.1f seconds\n", (double)duration_us / 1000000.0);
    printf("  Avg rate:   %.1f Hz\n",
           duration_us > 0 ? (double)msg_count / ((double)duration_us / 1000000.0) : 0);

    /* Topic list */
    char topics[64][64];
    uint64_t counts[64];
    int n = bag_reader_get_topics(r, topics, 64, counts);
    double duration_sec = (double)duration_us / 1000000.0;
    printf("  Topics:     %d\n", n);
    for (int i = 0; i < n && i < 10; i++) {
        double freq = (duration_sec > 0.0) ? counts[i] / duration_sec : 0.0;
        printf("    %-30s %6" PRIu64 " msgs  %6.1f Hz\n",
               topics[i], counts[i], freq);
    }

    /* Type info per topic (v2 format) */
    printf("\n  Type info:\n");
    for (int i = 0; i < n && i < 10; i++) {
        uint32_t type_id;
        uint8_t schema_ver;
        if (bag_reader_get_type_info(r, topics[i], &type_id, &schema_ver) == 0) {
            printf("    %-30s id=0x%08x  schema v%u\n",
                   topics[i], type_id, schema_ver);
        } else {
            printf("    %-30s (no type info)\n", topics[i]);
        }
    }

    bag_reader_close(r);
    return 0;
}

static int parse_nonnegative_double_arg(const char* text, double* out) {
    if (!text || !out) return ERR_INVALID_PARAM;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text || (end && *end != '\0') || value < 0.0) return ERR_INVALID_PARAM;
    *out = value;
    return ERR_OK;
}

static void resolve_sibling_executable(const char* argv0, const char* name,
                                       char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!argv0 || !argv0[0]) {
        snprintf(out, out_size, "%s", name);
        return;
    }
    const char* slash = strrchr(argv0, '/');
    if (!slash) {
        snprintf(out, out_size, "%s", name);
        return;
    }
    int dir_len = (int)(slash + 1 - argv0);
    snprintf(out, out_size, "%.*s%s", dir_len, argv0, name);
}

static int cmd_bag_play(int argc, char** argv) {
    if (argc < 4 || !argv[3]) {
        fprintf(stderr, "Usage: flowctl bag play <file.bag> [--config <pipeline.json>] [--multi] [--rate <x>] [--topic <topic>] [--start <sec>] [--end <sec>] [--loop] [--duration <sec>]\n");
        return 1;
    }

    const char* bag_path = argv[3];
    const char* config_path = "config/pipeline.json";
    const char* topic_filter = NULL;
    const char* duration_arg = NULL;
    double rate = 1.0;
    double start_sec = 0.0;
    double end_sec = 0.0;
    int multi_mode = 0;
    int loop = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--multi") == 0) {
            multi_mode = 1;
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            if (parse_nonnegative_double_arg(argv[++i], &rate) != ERR_OK) {
                fprintf(stderr, "Error: invalid --rate '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--topic") == 0 && i + 1 < argc) {
            topic_filter = argv[++i];
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            if (parse_nonnegative_double_arg(argv[++i], &start_sec) != ERR_OK) {
                fprintf(stderr, "Error: invalid --start '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--end") == 0 && i + 1 < argc) {
            if (parse_nonnegative_double_arg(argv[++i], &end_sec) != ERR_OK) {
                fprintf(stderr, "Error: invalid --end '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--loop") == 0) {
            loop = 1;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_arg = argv[++i];
        } else if (strcmp(argv[i], "--attach") == 0 ||
                   strcmp(argv[i], "--running") == 0) {
            fprintf(stderr, "Error: attaching replay to an already-running pipeline is unsupported; launch via 'flowctl bag play <file> [--multi]'\n");
            return 1;
        } else {
            fprintf(stderr, "Error: unknown replay option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (end_sec > 0.0 && end_sec < start_sec) {
        fprintf(stderr, "Error: --end must be >= --start\n");
        return 1;
    }

    BagReader* reader = bag_reader_open(bag_path);
    if (!reader) {
        fprintf(stderr, "Error: cannot open '%s'\n", bag_path);
        return 1;
    }
    bag_reader_close(reader);

    char flow_launcher_path[512];
    resolve_sibling_executable(argv[0], "flow_launcher",
                               flow_launcher_path, sizeof(flow_launcher_path));

    char rate_buf[32];
    char start_buf[32];
    char end_buf[32];
    snprintf(rate_buf, sizeof(rate_buf), "%.12g", rate);
    snprintf(start_buf, sizeof(start_buf), "%.12g", start_sec);
    snprintf(end_buf, sizeof(end_buf), "%.12g", end_sec);

    char* exec_argv[24];
    int n = 0;
    exec_argv[n++] = flow_launcher_path;
    exec_argv[n++] = (char*)config_path;
    if (multi_mode) exec_argv[n++] = "--multi";
    if (duration_arg) { exec_argv[n++] = "--duration"; exec_argv[n++] = (char*)duration_arg; }
    exec_argv[n++] = "--replay-bag";
    exec_argv[n++] = (char*)bag_path;
    exec_argv[n++] = "--replay-rate";
    exec_argv[n++] = rate_buf;
    if (topic_filter && topic_filter[0]) {
        exec_argv[n++] = "--replay-topic";
        exec_argv[n++] = (char*)topic_filter;
    }
    if (start_sec > 0.0) {
        exec_argv[n++] = "--replay-start";
        exec_argv[n++] = start_buf;
    }
    if (end_sec > 0.0) {
        exec_argv[n++] = "--replay-end";
        exec_argv[n++] = end_buf;
    }
    if (loop) exec_argv[n++] = "--replay-loop";
    exec_argv[n] = NULL;

    if (strchr(flow_launcher_path, '/'))
        execv(flow_launcher_path, exec_argv);
    else
        execvp(flow_launcher_path, exec_argv);
    perror("exec");
    return 1;
}

/* ── schema ───────────────────────────────────────────────── */

static int cmd_schema(const char* type_name) {
    if (!type_name) {
        fprintf(stderr, "Usage: flowctl schema <type_name>\n");
        return 1;
    }

    adas_msgs_register_all();

    const TypeRegistryEntry* e = serializer_lookup_by_name(type_name);
    if (!e) {
        printf("Type '%s' not found in registry.\n", type_name);
        printf("Registered types (%d):\n", serializer_type_count());
        /* List all */
        for (uint32_t tid = 0; tid < 0xFFFFFFFF; tid++) {
            const TypeRegistryEntry* te = serializer_lookup_type(tid);
            if (te) {
                printf("  %s (id=0x%08x, size=%zu, schema v%u)\n",
                       te->type_name, te->type_id,
                       te->struct_size, te->schema_version);
            }
            if (tid > 0xFFFF) break;
        }
        return 1;
    }

    printf("Type: %s\n\n", e->type_name);
    printf("  Type ID:        0x%08x\n", e->type_id);
    printf("  Schema Version: %u\n", e->schema_version);
    printf("  Schema Hash:    0x%08x\n", e->schema_hash);
    printf("  Struct Size:    %zu bytes\n", e->struct_size);
    printf("  Serializer:     %s\n", e->serialize ? "yes" : "no");
    printf("  Deserializer:   %s\n", e->deserialize ? "yes" : "no");
    printf("  Endian Swap:    %s\n", e->endian_swap ? "yes" : "no");
    if (e->fields && e->field_count > 0) {
        printf("\n  Fields (%u):\n", e->field_count);
        printf("    %-20s %-8s %-8s %-8s %s\n",
               "name", "kind", "offset", "elem_sz", "array");
        for (uint16_t i = 0; i < e->field_count; i++) {
            const FieldDesc* f = &e->fields[i];
            printf("    %-20s %-8s %-8u %-8u %u\n",
                   f->name, field_kind_str(f->kind),
                   f->offset, f->elem_size, f->array_len);
        }
    }
    return 0;
}

/* ── dashboard ────────────────────────────────────────────── */

static int cmd_dashboard(void) {
    printf("Starting FlowBoard dashboard...\n");
    printf("  Server: http://localhost:8800\n");
    printf("  API:    /api/topology  /api/stream\n\n");
    printf("Run in another terminal:\n");
    printf("  ./build/bin/flow_launcher config/pipeline.json --duration 30 &\n");
    printf("  ./build/bin/flowmond --html-path tools/flowboard/index.html\n");
    printf("  (state file: %s)\n", FLOWENGINE_DEFAULT_STATE_FILE);
    printf("\nThen open http://localhost:8800 in browser.\n");
    return 0;
}

static int cmd_doctor(int argc, char** argv) {
#ifdef _WIN32
    char cmd_buf[1024] = "python3 tools/flow_doctor.py";
    size_t used = strlen(cmd_buf);
    for (int i = 2; i < argc && used + 2 < sizeof(cmd_buf); i++) {
        size_t alen = strlen(argv[i]);
        if (used + 1 + alen >= sizeof(cmd_buf)) break;
        cmd_buf[used++] = ' ';
        memcpy(cmd_buf + used, argv[i], alen);
        used += alen;
        cmd_buf[used] = '\0';
    }
    return system(cmd_buf);
#else
    char** py_argv = (char**)calloc((size_t)(argc + 2), sizeof(char*));
    if (!py_argv) return 1;
    py_argv[0] = (char*)"python3";
    py_argv[1] = (char*)"tools/flow_doctor.py";
    for (int i = 2; i < argc; i++) {
        py_argv[i] = argv[i];
    }
    py_argv[argc] = NULL;
    execvp("python3", py_argv);
    perror("flowctl doctor: execvp python3");
    free(py_argv);
    return 127;
#endif
}

/* ══════════════════════════════════════════════════════════ */
/* Main                                                        */
/* ══════════════════════════════════════════════════════════ */

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char* cmd  = argv[1];
    const char* arg1 = argc > 2 ? argv[2] : NULL;
    const char* arg2 = argc > 3 ? argv[3] : NULL;

    /* ── list ── */
    if (strcmp(cmd, "list") == 0) {
        if (!arg1) { print_usage(); return 1; }
        if (strcmp(arg1, "tasks") == 0)   return cmd_list_tasks();
        if (strcmp(arg1, "topics") == 0)  return cmd_list_topics();
        if (strcmp(arg1, "types") == 0)   return cmd_list_types();
        if (strcmp(arg1, "params") == 0)  return cmd_list_params();
        if (strcmp(arg1, "plugins") == 0) {
            PluginMeta plugins[32];
            int n = flow_registry_list_plugins(plugins, 32);
            if (n == 0) {
                printf("Plugins: (none registered — start flow_launcher first)\n");
            } else {
                printf("%-20s %-40s %s\n", "NAME", "PATH", "TASKS");
                for (int i = 0; i < n; i++)
                    printf("  %-18s %-40s %d tasks\n", plugins[i].name, plugins[i].path, plugins[i].task_count);
                printf("  Total: %d plugins (from registry)\n", n);
            }
            return 0;
        }
        print_usage(); return 1;
    }

    /* ── graph ── */
    if (strcmp(cmd, "graph") == 0) return cmd_graph();

    /* ── topology ── */
    if (strcmp(cmd, "topology") == 0) {
        if (!arg1) { fprintf(stderr, "Usage: flowctl topology dot\n"); return 1; }
        if (strcmp(arg1, "dot") == 0) {
            /* Generate Graphviz DOT format from FlowRegistry */
            char* json = flow_registry_export_json();
            if (!json) { fprintf(stderr, "Error: registry export failed\n"); return 1; }
            printf("// Graphviz DOT — pipe to:  dot -Tsvg -o topology.svg\n");
            printf("digraph FlowEngine {\n");
            printf("  rankdir=LR;\n");
            printf("  node [shape=box, style=filled, fillcolor=\"#1a1a2e\","
                   " fontcolor=\"#58a6ff\", fontname=\"monospace\", fontsize=10];\n");
            printf("  edge [color=\"#30363d\", fontcolor=\"#8b949e\", fontsize=9];\n\n");
            /* Parse registry JSON for tasks and topic edges */
            cJSON* root = cJSON_Parse(json);
            if (root) {
                const cJSON* tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
                if (cJSON_IsArray(tasks)) {
                    const cJSON* task = NULL;
                    cJSON_ArrayForEach(task, tasks) {
                        const cJSON* jname = cJSON_GetObjectItemCaseSensitive(task, "name");
                        if (!cJSON_IsString(jname) || !jname->valuestring) continue;
                        const char* tname = jname->valuestring;
                        printf("  \"%s\" [fillcolor=\"#0d1117\", fontcolor=\"#c9d1d9\"];\n", tname);

                        const cJSON* outputs = cJSON_GetObjectItemCaseSensitive(task, "outputs");
                        const cJSON* out_t = NULL;
                        cJSON_ArrayForEach(out_t, outputs) {
                            if (cJSON_IsString(out_t) && out_t->valuestring) {
                                printf("  \"%s\" -> \"%s\" [label=\"%s\"];\n",
                                       tname, out_t->valuestring, out_t->valuestring);
                            }
                        }
                        const cJSON* inputs = cJSON_GetObjectItemCaseSensitive(task, "inputs");
                        const cJSON* in_t = NULL;
                        cJSON_ArrayForEach(in_t, inputs) {
                            if (cJSON_IsString(in_t) && in_t->valuestring) {
                                printf("  \"%s\" -> \"%s\" [label=\"%s\"];\n",
                                       in_t->valuestring, tname, in_t->valuestring);
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
            printf("}\n");
            free(json);
            return 0;
        }
        fprintf(stderr, "Usage: flowctl topology dot\n");
        return 1;
    }

    /* ── state ── */
    if (strcmp(cmd, "state") == 0) return cmd_state(arg1);

    /* ── topic ── */
    if (strcmp(cmd, "topic") == 0) {
        if (!arg1 || strcmp(arg1, "stats") != 0 || !arg2) {
            print_usage(); return 1;
        }
        return cmd_topic_stats(arg2);
    }

    /* ── bag ── */
    if (strcmp(cmd, "bag") == 0) {
        if (!arg1) { print_usage(); return 1; }
        if (strcmp(arg1, "info") == 0 && arg2) return cmd_bag_info(arg2);
        if (strcmp(arg1, "play") == 0) return cmd_bag_play(argc, argv);
        if (strcmp(arg1, "check") == 0 && arg2) {
            BagReader* r = bag_reader_open(arg2);
            if (!r) { printf("✗ Cannot open '%s'\n", arg2); return 1; }
            uint64_t count, dur;
            bag_reader_info(r, &count, &dur);
            printf("✓ Bag file valid: %" PRIu64 " msgs, %.1fs\n", count, (double)dur/1000000.0);
            char topics[64][64];
            int n = bag_reader_get_topics(r, topics, 64, NULL);
            printf("  Topics: %d\n", n);
            bag_reader_close(r);
            return 0;
        }
        print_usage(); return 1;
    }

    /* ── schema ── */
    if (strcmp(cmd, "schema") == 0) return cmd_schema(arg1);

    /* ── param ──
     * 全部走 param_bridge 打到运行中的 flow_launcher。此前这里读写的是
     * flowctl 自己进程内那份 registry —— set 完打印"✓ updated"，跑着的
     * 车却什么都没变，是个纯粹的假象。 */
    if (strcmp(cmd, "param") == 0) {
        if (!arg1) { fprintf(stderr, "Usage: flowctl param list|get|set [name] [value]\n"); return 1; }

        char req[PARAM_BRIDGE_MAX_LINE];
        char resp[16384];

        if (strcmp(arg1, "list") == 0) {
            snprintf(req, sizeof(req), "LIST");
            int rc = param_bridge_client_request(req, resp, sizeof(resp));
            if (rc != ERR_OK) { fprintf(stderr, "✗ %s\n", resp); return 1; }
            /* 响应首行是参数个数，其后每行 "<name> <type> <value> <min> <max>" */
            char* save = NULL;
            char* line = strtok_r(resp, "\n", &save);
            int total = line ? atoi(line) : 0;
            printf("%-32s %-8s %-12s %s\n", "NAME", "TYPE", "VALUE", "RANGE");
            while ((line = strtok_r(NULL, "\n", &save))) {
                char nm[PARAM_NAME_LEN] = {0}, ty[16] = {0}, va[64] = {0}, lo[64] = {0}, hi[64] = {0};
                if (sscanf(line, "%63s %15s %63s %63s %63s", nm, ty, va, lo, hi) >= 3)
                    printf("  %-30s %-8s %-12s [%s, %s]\n", nm, ty, va, lo, hi);
            }
            printf("  Total: %d params (live from running process)\n", total);
            return 0;
        }

        if (strcmp(arg1, "get") == 0) {
            if (!arg2) { fprintf(stderr, "Usage: flowctl param get <name>\n"); return 1; }
            snprintf(req, sizeof(req), "GET %s", arg2);
            int rc = param_bridge_client_request(req, resp, sizeof(resp));
            if (rc != ERR_OK) { fprintf(stderr, "✗ %s\n", resp); return 1; }
            printf("%s = %s\n", arg2, resp);
            return 0;
        }

        if (strcmp(arg1, "set") == 0) {
            if (!arg2 || !argv[4]) { fprintf(stderr, "Usage: flowctl param set <name> <value>\n"); return 1; }
            snprintf(req, sizeof(req), "SET %s %s", arg2, argv[4]);
            int rc = param_bridge_client_request(req, resp, sizeof(resp));
            if (rc != ERR_OK) { fprintf(stderr, "✗ %s\n", resp); return 1; }
            printf("✓ %s = %s (applied to running process)\n", arg2, resp);
            return 0;
        }

        fprintf(stderr, "Usage: flowctl param list|get|set [name] [value]\n");
        return 1;
    }

    /* ── registry ── */
    if (strcmp(cmd, "registry") == 0) {
        /* Try JSON file first (live data from e2e) */
        cJSON* root = read_state_json();
        if (root) {
            const cJSON* reg = cJSON_GetObjectItemCaseSensitive(root, "registry");
            if (cJSON_IsObject(reg)) {
                char* printed = cJSON_PrintUnformatted(reg);
                if (printed) {
                    printf("%s\n", printed);
                    cJSON_free(printed);
                    cJSON_Delete(root);
                    return 0;
                }
            }
            cJSON_Delete(root);
        }
        /* Fallback: local registry */
        char* json = flow_registry_export_json();
        if (json) { printf("%s\n", json); free(json); }
        return 0;
    }

    /* ── launch ── */
    if (strcmp(cmd, "launch") == 0) {
        if (!arg1) { fprintf(stderr, "Usage: flowctl launch <config.json>\n"); return 1; }
        LauncherConfig* cfg = config_load(arg1);
        if (!cfg) { fprintf(stderr, "Error: cannot load config '%s'\n", arg1); return 1; }
        printf("Configuration loaded: %d processes\n\n", cfg->process_count);
        for (int i = 0; i < cfg->process_count; i++) {
            ProcessConfig* pc = &cfg->processes[i];
            printf("  %-16s  lib=%-40s  pub=%d  sub=%d  deps=%d\n",
                   pc->name, pc->library_path,
                   pc->publish_count, pc->subscribe_count, pc->depends_count);
        }
        printf("\nScheduler: %s  workers=%d  tick=%dus\n",
               cfg->scheduler.mode == 1 ? "choreo" : "classic",
               cfg->scheduler.worker_threads, cfg->scheduler.tick_us);
        printf("\nRun with: ./build/bin/flow_launcher %s\n", arg1);
        config_free(cfg);
        return 0;
    }

    /* ── doctor ── */
    if (strcmp(cmd, "doctor") == 0) return cmd_doctor(argc, argv);

    /* ── dashboard ── */
    if (strcmp(cmd, "dashboard") == 0) return cmd_dashboard();

    /* ── version ── */
    if (strcmp(cmd, "version") == 0) { print_version(); return 0; }

    /* ── help ── */
    print_usage();
    return 0;
}
