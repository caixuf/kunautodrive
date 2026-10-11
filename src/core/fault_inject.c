/*
 * 传输层故障注入实现 — 见 include/fault_inject.h 的设计说明。
 *
 * 线程安全：规则表在 init 时一次写满（单线程，节点注册前），运行期只读；
 * 丢弃计数用 __atomic_* 累加，与 message_bus/degrade_ladder 的既有写法一致。
 */

#include "fault_inject.h"
#include "logger.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    FaultInjectRule rules[FAULT_INJECT_MAX_RULES];
    int             count;
    bool            active;       /* 有规则 → 热路径才做窗口判断 */
    uint64_t        start_us;     /* 注入时间基（init 时刻）*/
    atomic_ullong   dropped;      /* 累计丢弃消息数 */
} g_fi;

bool fault_inject_enabled(void) {
    return g_fi.active;
}

int fault_inject_rule_count(void) {
    return g_fi.count;
}

uint64_t fault_inject_dropped_count(void) {
    return (uint64_t)atomic_load_explicit(&g_fi.dropped, memory_order_relaxed);
}

bool fault_inject_get_rule(int index, FaultInjectRule* out) {
    if (!out || index < 0 || index >= g_fi.count) return false;
    *out = g_fi.rules[index];
    return true;
}

void fault_inject_clear(void) {
    memset(&g_fi, 0, sizeof(g_fi));
    atomic_init(&g_fi.dropped, 0);
}

/* 解析一段 "<topic>:<after_ms>[:<duration_ms>]"。
 * 返回 0 成功，-1 段落非法（调用方跳过该段）。 */
static int parse_segment(char* seg, FaultInjectRule* rule) {
    /* topic 名本身可能含 '/'，但不含 ':'，故按 ':' 切分是安全的。 */
    char* save = NULL;
    char* topic = strtok_r(seg, ":", &save);
    char* after = strtok_r(NULL, ":", &save);
    char* dur   = strtok_r(NULL, ":", &save);

    if (!topic || !topic[0] || !after || !after[0]) return -1;
    if (strlen(topic) >= FAULT_INJECT_TOPIC_LEN) return -1;

    char* end = NULL;
    long a = strtol(after, &end, 10);
    if (end == after || *end != '\0' || a < 0) return -1;

    long d = 0;
    if (dur && dur[0]) {
        end = NULL;
        d = strtol(dur, &end, 10);
        if (end == dur || *end != '\0' || d < 0) return -1;
    }

    memset(rule, 0, sizeof(*rule));
    snprintf(rule->topic, sizeof(rule->topic), "%s", topic);
    rule->start_after_us = (uint64_t)a * 1000ULL;
    rule->duration_us    = (uint64_t)d * 1000ULL;
    rule->active         = true;
    return 0;
}

int fault_inject_init(const char* spec, uint64_t start_us) {
    fault_inject_clear();
    g_fi.start_us = start_us;

    if (!spec || !spec[0]) {
        LOG_INFO("fault_inject", "disabled (no FLOW_FAULT_INJECT)");
        return 0;
    }

    /* 规格串可能较长，拷进可写缓冲再 strtok_r（不改调用方的 environ）。 */
    size_t len = strlen(spec);
    if (len > 4096) {
        LOG_WARN("fault_inject", "spec too long (%zu bytes), ignoring", len);
        return 0;
    }
    char* buf = (char*)malloc(len + 1);
    if (!buf) return 0;
    memcpy(buf, spec, len + 1);

    int n = 0;
    char* save = NULL;
    for (char* seg = strtok_r(buf, ";", &save); seg; seg = strtok_r(NULL, ";", &save)) {
        /* 允许空白分隔，便于 shell/CI 里换行书写 */
        while (*seg == ' ' || *seg == '\t') seg++;
        if (!*seg) continue;
        if (n >= FAULT_INJECT_MAX_RULES) {
            LOG_WARN("fault_inject", "rule table full (%d), ignoring rest of spec",
                     FAULT_INJECT_MAX_RULES);
            break;
        }
        FaultInjectRule rule;
        if (parse_segment(seg, &rule) != 0) {
            /* 单段写错不影响其余段：注入是测试基础设施，不该让管线起不来。 */
            LOG_WARN("fault_inject", "skipping malformed segment '%s' "
                     "(want topic:after_ms[:duration_ms])", seg);
            continue;
        }
        g_fi.rules[n++] = rule;
    }
    free(buf);

    g_fi.count  = n;
    g_fi.active = (n > 0);

    for (int i = 0; i < n; i++) {
        LOG_INFO("fault_inject", "rule[%d]: drop '%s' after %.0fms%s",
                 i, g_fi.rules[i].topic,
                 (double)g_fi.rules[i].start_after_us / 1000.0,
                 g_fi.rules[i].duration_us > 0 ? "" : " (permanent)");
    }
    if (n > 0) {
        LOG_WARN("fault_inject", "%d fault-injection rule(s) ACTIVE — "
                 "this run intentionally breaks the pipeline", n);
    }
    return n;
}

bool fault_inject_should_drop(const char* topic, uint64_t now_us) {
    /* 热路径短路：未配置注入时只多一次 bool 读。 */
    if (!g_fi.active || !topic) return false;

    const uint64_t elapsed = now_us - g_fi.start_us;
    for (int i = 0; i < g_fi.count; i++) {
        const FaultInjectRule* r = &g_fi.rules[i];
        if (!r->active) continue;
        if (strcmp(r->topic, topic) != 0) continue;
        if (elapsed < r->start_after_us) continue;
        /* duration_us == 0 → 永久注入 */
        if (r->duration_us > 0 && elapsed >= r->start_after_us + r->duration_us) continue;

        atomic_fetch_add_explicit(&g_fi.dropped, 1ULL, memory_order_relaxed);
        return true;
    }
    return false;
}
