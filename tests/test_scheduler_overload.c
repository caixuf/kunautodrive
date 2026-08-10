#include "scheduler.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            failures++; \
        } \
    } while (0)

static atomic_uint critical_runs;
static atomic_uint low_runs;

static int run_critical(TaskBase* task) {
    (void)task;
    atomic_fetch_add(&critical_runs, 1);
    return 0;
}

static int run_slow_low(TaskBase* task) {
    (void)task;
    atomic_fetch_add(&low_runs, 1);
    usleep(3000);
    return 0;
}

static const TaskInterface critical_vtable = {
    .execute = run_critical,
};

static const TaskInterface slow_low_vtable = {
    .execute = run_slow_low,
};

static void init_task(TaskBase* task, const TaskInterface* vtable,
                      TaskPriority priority, const char* name) {
    memset(task, 0, sizeof(*task));
    task->vtable = vtable;
    task->config.priority = priority;
    snprintf(task->config.name, sizeof(task->config.name), "%s", name);
}

typedef struct {
    Scheduler* scheduler;
    int result;
} RunLoopArgs;

static void* run_loop(void* arg) {
    RunLoopArgs* args = (RunLoopArgs*)arg;
    args->result = scheduler_run_loop(args->scheduler);
    return NULL;
}

static void test_classic_priority_and_shedding(void) {
    SchedulerConfig config = SCHEDULER_CONFIG_DEFAULT;
    config.worker_thread_count = 1;

    Scheduler* scheduler = scheduler_create(&config);
    CHECK(scheduler != NULL, "create classic scheduler");
    if (!scheduler) return;

    TaskBase critical;
    TaskBase low;
    init_task(&critical, &critical_vtable, TASK_PRIORITY_CRITICAL, "critical");
    init_task(&low, &slow_low_vtable, TASK_PRIORITY_LOW, "slow-low");

    int critical_id = scheduler_register_task(scheduler, &critical, "critical");
    int low_id = scheduler_register_task(scheduler, &low, "slow-low");
    CHECK(critical_id >= 0 && low_id >= 0, "register classic tasks");
    CHECK(scheduler_set_execution_budget(scheduler, low_id, 1000) == 0,
          "set low dispatch budget");

    atomic_store(&critical_runs, 0);
    atomic_store(&low_runs, 0);
    RunLoopArgs args = { .scheduler = scheduler, .result = -1 };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_loop, &args) == 0,
          "start classic run loop");

    usleep(60000);
    scheduler_stop(scheduler);
    pthread_join(thread, NULL);

    SchedulerTaskStats critical_stats;
    SchedulerTaskStats low_stats;
    scheduler_get_task_stats(scheduler, critical_id, &critical_stats);
    scheduler_get_task_stats(scheduler, low_id, &low_stats);

    CHECK(args.result == 0, "classic run loop exits cleanly after stop");
    CHECK(low_stats.dispatch_count > 0, "low task was dispatched");
    CHECK(low_stats.budget_overrun_count > 0, "low overruns are counted");
    CHECK(low_stats.shed_count > 0, "one low dispatch is shed after an overrun");
    CHECK(critical_stats.dispatch_count > low_stats.dispatch_count,
          "critical task receives slots while low work is shed");
    CHECK(critical_stats.shed_count == 0,
          "critical task is never shed by budget policy");

    scheduler_destroy(scheduler);
}

static void test_choreo_accounting_and_shedding(void) {
    SchedulerConfig config = SCHEDULER_CONFIG_DEFAULT;
    config.mode = SCHEDULER_MODE_CHOREO;

    Scheduler* scheduler = scheduler_create(&config);
    MessageBus* bus = message_bus_create("scheduler-overload-test");
    CHECK(scheduler != NULL && bus != NULL, "create choreo scheduler and bus");
    if (!scheduler || !bus) {
        if (bus) message_bus_destroy(bus);
        if (scheduler) scheduler_destroy(scheduler);
        return;
    }

    TaskBase low;
    TaskBase critical;
    TaskBase quota_limited;
    init_task(&low, &slow_low_vtable, TASK_PRIORITY_LOW, "choreo-low");
    init_task(&critical, &critical_vtable, TASK_PRIORITY_CRITICAL, "choreo-critical");
    init_task(&quota_limited, &slow_low_vtable, TASK_PRIORITY_LOW, "choreo-quota");
    int low_id = scheduler_register_task(scheduler, &low, "choreo-low");
    int critical_id = scheduler_register_task(scheduler, &critical, "choreo-critical");
    int quota_id = scheduler_register_task(scheduler, &quota_limited, "choreo-quota");
    CHECK(low_id >= 0 && critical_id >= 0 && quota_id >= 0,
          "register choreo tasks");

    scheduler_set_choreo_bus(scheduler, bus);
    CHECK(scheduler_set_execution_budget(scheduler, low_id, 100) == 0,
          "set choreo low budget");
    CHECK(scheduler_set_execution_budget(scheduler, critical_id, 100) == 0,
          "set choreo critical budget");
    CHECK(scheduler_choreo_trigger_on(scheduler, low_id, "overload/low") == 0,
          "register low choreo trigger");
    CHECK(scheduler_choreo_trigger_on(scheduler, critical_id, "overload/critical") == 0,
          "register critical choreo trigger");
    CHECK(scheduler_choreo_trigger_on(scheduler, quota_id, "overload/quota") == 0,
          "register quota-limited choreo trigger");

    ResourceQuota quota = { .max_execution_count = 1 };
    CHECK(scheduler_set_quota(scheduler, quota_id, &quota) == 0,
          "set hard execution quota");

    CHECK(scheduler_record_execution(scheduler, low_id, 200) == 0,
          "record low choreo overrun");
    CHECK(scheduler_record_execution(scheduler, critical_id, 200) == 0,
          "record critical choreo overrun");
    CHECK(scheduler_record_execution(scheduler, quota_id, 1) == 0,
          "record quota-limited dispatch");

    int payload = 1;
    message_bus_publish(bus, "overload/low", "test", &payload, sizeof(payload));
    message_bus_publish(bus, "overload/critical", "test", &payload, sizeof(payload));
    message_bus_publish(bus, "overload/quota", "test", &payload, sizeof(payload));
    usleep(10000);

    SchedulerTaskStats low_stats;
    SchedulerTaskStats critical_stats;
    SchedulerTaskStats quota_stats;
    ChoreoStats low_choreo;
    ChoreoStats critical_choreo;
    ChoreoStats quota_choreo;
    scheduler_get_task_stats(scheduler, low_id, &low_stats);
    scheduler_get_task_stats(scheduler, critical_id, &critical_stats);
    scheduler_get_task_stats(scheduler, quota_id, &quota_stats);
    scheduler_get_choreo_stats(scheduler, low_id, &low_choreo);
    scheduler_get_choreo_stats(scheduler, critical_id, &critical_choreo);
    scheduler_get_choreo_stats(scheduler, quota_id, &quota_choreo);

    CHECK(low_stats.budget_overrun_count == 1, "low choreo overrun is visible");
    CHECK(low_stats.shed_count == 1, "low choreo trigger is shed once");
    CHECK(low_choreo.triggers_missed == 1, "shed trigger is reported as missed");
    CHECK(critical_stats.budget_overrun_count == 1,
          "critical choreo overrun is visible");
    CHECK(critical_stats.shed_count == 0, "critical choreo trigger is not shed");
    CHECK(critical_choreo.triggers_fired == 1,
          "critical choreo trigger remains deliverable");
    CHECK(quota_stats.quota_denied_count == 1,
          "hard ResourceQuota denial is observable");
    CHECK(quota_choreo.triggers_missed == 1,
          "quota-denied choreo trigger is reported as missed");

    message_bus_destroy(bus);
    scheduler_destroy(scheduler);
}

int main(void) {
    test_classic_priority_and_shedding();
    test_choreo_accounting_and_shedding();

    if (failures != 0) {
        fprintf(stderr, "scheduler overload tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("scheduler overload tests: PASS\n");
    return 0;
}
