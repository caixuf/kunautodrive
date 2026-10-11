/* 单测：fault_inject 规格解析与窗口判定（L3-P2）。 */
#include "fault_inject.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); g_fail++; } } while (0)

int main(void) {
    /* 1. 未配置 → 禁用，且任何 topic 都不丢 */
    fault_inject_init(NULL, 0);
    CHECK(!fault_inject_enabled(), "NULL spec must disable injection");
    CHECK(fault_inject_rule_count() == 0, "NULL spec must yield 0 rules");
    CHECK(!fault_inject_should_drop("anything", 1e9), "disabled must never drop");

    fault_inject_init("", 0);
    CHECK(!fault_inject_enabled(), "empty spec must disable injection");

    /* 2. 单条永久规则 */
    fault_inject_init("t/a:1000", 0);
    CHECK(fault_inject_enabled(), "spec must enable injection");
    CHECK(fault_inject_rule_count() == 1, "expect 1 rule");
    CHECK(!fault_inject_should_drop("t/a", 999999), "before start must not drop");
    CHECK(fault_inject_should_drop("t/a", 1000000), "at/after start must drop (permanent)");
    CHECK(fault_inject_should_drop("t/a", 9e9), "permanent rule must keep dropping");
    CHECK(!fault_inject_should_drop("t/b", 9e9), "other topic must not drop");

    /* 3. 限时窗口：窗口外必须恢复 */
    fault_inject_init("t/a:1000:2000", 0);
    CHECK(!fault_inject_should_drop("t/a", 999999), "before window must not drop");
    CHECK(fault_inject_should_drop("t/a", 1000000), "window start must drop");
    CHECK(fault_inject_should_drop("t/a", 2999999), "inside window must drop");
    CHECK(!fault_inject_should_drop("t/a", 3000000), "window end must STOP dropping");

    /* 4. 多条规则（分号分隔）各自独立 */
    fault_inject_init("t/a:1000;t/b:2000", 0);
    CHECK(fault_inject_rule_count() == 2, "expect 2 rules");
    CHECK(fault_inject_should_drop("t/a", 1000000), "rule a must fire");
    CHECK(!fault_inject_should_drop("t/b", 1000000), "rule b not yet");
    CHECK(fault_inject_should_drop("t/b", 2000000), "rule b must fire");

    /* 5. 非法段落被跳过，合法段落仍生效（测试基础设施不该因一格写错整体失效） */
    fault_inject_init("garbage;t/a:1000", 0);
    CHECK(fault_inject_rule_count() == 1, "malformed segment must be skipped, valid kept");
    CHECK(fault_inject_should_drop("t/a", 1000000), "valid rule must still work");
    fault_inject_init("t/a:notanumber", 0);
    CHECK(fault_inject_rule_count() == 0, "non-numeric after_ms must be rejected");

    /* 6. 丢弃计数（门禁据此确认注入确已生效） */
    fault_inject_clear();
    fault_inject_init("t/a:0", 0);
    CHECK(fault_inject_dropped_count() == 0, "counter starts at 0");
    fault_inject_should_drop("t/a", 0);
    fault_inject_should_drop("t/a", 0);
    CHECK(fault_inject_dropped_count() == 2, "counter must accumulate");

    /* 7. 规则回读（monitor 导出用） */
    fault_inject_init("t/x:1500:2500", 0);
    FaultInjectRule r;
    CHECK(fault_inject_get_rule(0, &r), "rule 0 must be readable");
    CHECK(strcmp(r.topic, "t/x") == 0, "topic must round-trip");
    CHECK(r.start_after_us == 1500000ULL, "after_ms must convert to us");
    CHECK(r.duration_us == 2500000ULL, "duration_ms must convert to us");
    CHECK(!fault_inject_get_rule(5, &r), "out-of-range index must return false");

    if (g_fail == 0) printf("fault_inject: all checks passed\n");
    return g_fail;
}
