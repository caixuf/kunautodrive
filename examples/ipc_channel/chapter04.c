/**
 * chapter04.c — 第 04 章配套程序（发布/订阅、布局核对、测量、故障实验）
 *
 * 构建：cmake --build build --target ipc_chapter
 * 用法：ipc_chapter <layout|pub|sub|demo|bench|fault-slow|fault-late|fault-orphan|fault-kill>
 *
 * pub/sub 可带一个次数参数：ipc_chapter pub 5
 * Linux 以外只保证 layout/pub/sub/fault-slow/fault-late 能编译；
 * demo、bench、fault-orphan、fault-kill 依赖 fork 与 /dev/shm。
 *
 * 本文件不修改 ipc_channel 的行为，只调用公开 API，再用 shm_unlink 把自己
 * 建出来的对象收掉。库本身的 ipc_channel_close 不会 shm_unlink。
 */

#include "ipc_channel.h"
#include "dashboard_bridge.h"
#include "clock_service.h"
#include "error_codes.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/wait.h>
#endif

#define CH04_DEPTH 16u

typedef struct {
    uint64_t seq;
    uint64_t send_ns;
    uint32_t nbytes;
    uint32_t checksum;
} SampleHdr;

/* 与 src/core/ipc_channel.c 里 POSIX 路径的 ShmHeader / ShmSlot 保持一致。
 * layout 模式会用真实 shm 文件的 st_size 核对；对不上就直接失败。 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint32_t        queue_depth;
    uint32_t        _pad;
    uint64_t        head;
} Ch04ShmHeader;

typedef struct {
    uint64_t seq;
    Message  msg;
} Ch04ShmSlot;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void fill_sample(uint8_t* buf, uint32_t nbytes, uint64_t seq) {
    SampleHdr* h = (SampleHdr*)buf;
    uint8_t* body = buf + sizeof(SampleHdr);
    uint32_t body_n = nbytes - (uint32_t)sizeof(SampleHdr);
    for (uint32_t i = 0; i < body_n; i++)
        body[i] = (uint8_t)(seq + i);
    h->seq = seq;
    h->nbytes = nbytes;
    h->checksum = fnv1a(body, body_n);
    h->send_ns = 0;
}

static int sample_ok(const uint8_t* buf, uint32_t size) {
    if (size < sizeof(SampleHdr)) return 0;
    const SampleHdr* h = (const SampleHdr*)buf;
    if (h->nbytes != size || size < sizeof(SampleHdr)) return 0;
    const uint8_t* body = buf + sizeof(SampleHdr);
    uint32_t body_n = size - (uint32_t)sizeof(SampleHdr);
    if (h->checksum != fnv1a(body, body_n)) return 0;
    for (uint32_t i = 0; i < body_n; i++) {
        if (body[i] != (uint8_t)(h->seq + i)) return 0;
    }
    return 1;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t* v, uint32_t n, int pct) {
    if (n == 0) return 0;
    qsort(v, n, sizeof(uint64_t), cmp_u64);
    uint32_t i = (uint32_t)(((uint64_t)pct * (n - 1)) / 100);
    return v[i];
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "用法: %s <layout|pub|sub|demo|bench|fault-slow|fault-late|fault-orphan|fault-kill> [count]\n"
            "  pub/sub 的 count 省略时一直跑到 Ctrl+C。\n"
            "  demo / bench / fault-* 只在 Linux 上提供。\n",
            argv0);
}

/* ── 交互式 pub/sub ──────────────────────────────────────── */

typedef struct {
    int got;
    int bad;
} SubCount;

static void on_walk_msg(const Message* msg, void* user_data) {
    SubCount* c = (SubCount*)user_data;
    c->got++;
    if (msg->data_size < sizeof(SampleHdr)) {
        c->bad++;
        printf("[sub] size=%u (小于 SampleHdr)\n", msg->data_size);
        return;
    }
    const SampleHdr* h = (const SampleHdr*)msg->data;
    int ok = sample_ok(msg->data, msg->data_size);
    if (!ok) c->bad++;
    printf("[sub] topic=%s seq=%" PRIu64 " bytes=%u checksum=%s\n",
           msg->topic, h->seq, msg->data_size, ok ? "ok" : "BAD");
}

static int run_pub(const char* name, int count) {
    IpcChannel* ch = ipc_channel_open(name, IPC_ROLE_PUBLISHER, CH04_DEPTH);
    if (!ch) {
        fprintf(stderr, "[pub] ipc_channel_open 失败 errno=%d\n", errno);
        return 1;
    }
    printf("[pub] channel=%s shm=/dev/shm/%s_shm depth=%u\n", name, name, CH04_DEPTH);
    uint8_t* buf = (uint8_t*)malloc(256);
    if (!buf) { ipc_channel_close(ch); return 1; }

    uint64_t seq = 0;
    int sent = 0;
    while (g_running && (count < 0 || sent < count)) {
        fill_sample(buf, 256, seq);
        ((SampleHdr*)buf)->send_ns = now_ns();
        int rc = ipc_channel_publish(ch, "ch04/walk", "ch04_pub", buf, 256);
        if (rc != 0)
            printf("[pub] publish 失败 rc=%d（不是队列满；环形缓冲不会以满拒绝）\n", rc);
        else
            printf("[pub] seq=%" PRIu64 "\n", seq);
        seq++;
        sent++;
        if (count < 0 || sent < count) sleep(1);
    }
    free(buf);
    ipc_channel_close(ch);
    printf("[pub] close 返回。POSIX 路径不会在这里 shm_unlink。\n");
    return 0;
}

static int run_sub(const char* name, int count) {
    IpcChannel* ch = NULL;
    for (int i = 0; i < 50 && !ch && g_running; i++) {
        ch = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, CH04_DEPTH);
        if (!ch) {
            printf("[sub] 等待发布端创建 %s ...\n", name);
            usleep(100000);
        }
    }
    if (!ch) {
        fprintf(stderr, "[sub] 打开失败。先起 pub。\n");
        return 1;
    }
    SubCount c = {0, 0};
    if (ipc_channel_subscribe(ch, on_walk_msg, &c) != 0) {
        ipc_channel_close(ch);
        return 1;
    }
    printf("[sub] 已连接 %s。读游标是本进程私有的。\n", name);
    while (g_running && (count < 0 || c.got < count)) {
        if (ipc_channel_recv_once(ch, 1000) != 0 && count >= 0) {
            /* 超时：继续等，直到次数凑齐或被信号打断 */
        }
    }
    printf("[sub] received=%d bad=%d drop_count=%" PRIu64 "\n",
           c.got, c.bad, ipc_channel_get_drop_count(ch));
    ipc_channel_close(ch);
    return c.bad ? 1 : 0;
}

/* ── 布局：用真实文件大小核对复制出来的结构体 ─────────────── */

static int run_layout(void) {
    const char* name = "ch04_layout";
    const uint32_t depth = 32;
    size_t expect = sizeof(Ch04ShmHeader) + (size_t)depth * sizeof(Ch04ShmSlot);

    printf("sizeof(Message)              %zu\n", sizeof(Message));
    printf("  offsetof topic             %zu\n", offsetof(Message, topic));
    printf("  offsetof sender            %zu\n", offsetof(Message, sender));
    printf("  offsetof msg_id            %zu\n", offsetof(Message, msg_id));
    printf("  offsetof type              %zu\n", offsetof(Message, type));
    printf("  offsetof timestamp_us      %zu\n", offsetof(Message, timestamp_us));
    printf("  offsetof topic_idx         %zu\n", offsetof(Message, topic_idx));
    printf("  offsetof data_size         %zu\n", offsetof(Message, data_size));
    printf("  offsetof type_id           %zu\n", offsetof(Message, type_id));
    printf("  offsetof data              %zu\n", offsetof(Message, data));
    printf("  MSG_BUS_MAX_DATA_SIZE      %d\n", MSG_BUS_MAX_DATA_SIZE);
    printf("sizeof(pthread_mutex_t)      %zu\n", sizeof(pthread_mutex_t));
    printf("sizeof(pthread_cond_t)       %zu\n", sizeof(pthread_cond_t));
    printf("sizeof(Ch04ShmHeader)         %zu\n", sizeof(Ch04ShmHeader));
    printf("  offsetof mutex             %zu\n", offsetof(Ch04ShmHeader, mutex));
    printf("  offsetof cond              %zu\n", offsetof(Ch04ShmHeader, cond));
    printf("  offsetof queue_depth       %zu\n", offsetof(Ch04ShmHeader, queue_depth));
    printf("  offsetof head              %zu\n", offsetof(Ch04ShmHeader, head));
    printf("sizeof(Ch04ShmSlot)           %zu\n", sizeof(Ch04ShmSlot));
    printf("  offsetof slot.seq          %zu\n", offsetof(Ch04ShmSlot, seq));
    printf("  offsetof slot.msg          %zu\n", offsetof(Ch04ShmSlot, msg));
    printf("formula depth=%u total        %zu\n", depth, expect);
    printf("sizeof(DashboardChunk)        %zu\n", sizeof(DashboardChunk));
    printf("DASHBOARD_CHUNK_DATA_SIZE     %d\n", DASHBOARD_CHUNK_DATA_SIZE);
    printf("DASHBOARD_BRIDGE_QUEUE_DEPTH  %d\n", DASHBOARD_BRIDGE_QUEUE_DEPTH);
    printf("chunks for 200000 bytes       %u\n",
           (unsigned)((200000u + DASHBOARD_CHUNK_DATA_SIZE - 1) / DASHBOARD_CHUNK_DATA_SIZE));
    printf("chunks for 10000 bytes        %u\n",
           (unsigned)((10000u + DASHBOARD_CHUNK_DATA_SIZE - 1) / DASHBOARD_CHUNK_DATA_SIZE));

#if defined(__linux__)
    IpcChannel* ch = ipc_channel_open(name, IPC_ROLE_PUBLISHER, depth);
    if (!ch) {
        fprintf(stderr, "[layout] publisher open 失败 errno=%d\n", errno);
        return 1;
    }
    char path[128];
    snprintf(path, sizeof(path), "/dev/shm/%s_shm", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[layout] stat %s 失败 errno=%d\n", path, errno);
        ipc_channel_close(ch);
        return 1;
    }
    printf("stat %s st_size=%lld\n", path, (long long)st.st_size);
    int mismatch = ((size_t)st.st_size != expect);
    if (mismatch)
        fprintf(stderr, "[layout] FAIL 文件大小与公式不一致\n");
    else
        printf("[layout] OK 文件大小与 ShmHeader + depth * ShmSlot 一致\n");
    ipc_channel_close(ch);
    /* close 不会摘名字；示例自己清，避免 /dev/shm 残留。 */
    if (shm_unlink("/ch04_layout_shm") != 0 && errno != ENOENT) {
        fprintf(stderr, "[layout] shm_unlink 失败 errno=%d\n", errno);
        return 1;
    }
    if (stat(path, &st) == 0) {
        fprintf(stderr, "[layout] unlink 后对象仍在\n");
        return 1;
    }
    printf("[layout] 示例已 shm_unlink，/dev/shm 条目消失\n");
    return mismatch ? 1 : 0;
#else
    printf("[layout] 非 Linux：跳过 /dev/shm 核对\n");
    return 0;
#endif
}

/* ── 慢消费者 / 晚加入：同一进程里的两个 IpcChannel ──────── */

typedef struct {
    uint32_t seqs[64];
    int n;
    int overflow;
} SeqLog;

static void on_seq(const Message* msg, void* user_data) {
    SeqLog* log = (SeqLog*)user_data;
    if (msg->data_size < sizeof(uint32_t)) return;
    uint32_t seq = 0;
    memcpy(&seq, msg->data, sizeof(seq));
    if (log->n < (int)(sizeof(log->seqs) / sizeof(log->seqs[0])))
        log->seqs[log->n++] = seq;
    else
        log->overflow++;
}

static int open_pair(const char* name, uint32_t depth,
                     IpcChannel** pub, IpcChannel** sub) {
    *pub = ipc_channel_open(name, IPC_ROLE_PUBLISHER, depth);
    if (!*pub) return -1;
    for (int i = 0; i < 50 && !*sub; i++) {
        *sub = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, depth);
        if (!*sub) usleep(20000);
    }
    if (!*sub) {
        ipc_channel_close(*pub);
        *pub = NULL;
        return -1;
    }
    return 0;
}

static int drain(IpcChannel* sub, int max_n) {
    int n = 0;
    while (n < max_n && ipc_channel_recv_once(sub, 20) == 0) n++;
    return n;
}

static int run_fault_slow(void) {
    const char* name = "ch04_fault_slow";
    const uint32_t depth = 8;
    const int total = 100;
    IpcChannel *pub = NULL, *sub = NULL;
    if (open_pair(name, depth, &pub, &sub) != 0) return 1;
    SeqLog log;
    memset(&log, 0, sizeof(log));
    ipc_channel_subscribe(sub, on_seq, &log);

    /* 先空读一次，把本订阅者的 read_cursor 锚在 head==0。 */
    (void)ipc_channel_recv_once(sub, 1);

    for (int i = 0; i < total; i++) {
        uint32_t seq = (uint32_t)i;
        if (ipc_channel_publish(pub, "ch04/slow", "pub", &seq, sizeof(seq)) != 0) {
            fprintf(stderr, "[fault-slow] publish 失败\n");
            ipc_channel_close(sub);
            ipc_channel_close(pub);
            return 1;
        }
    }
    int got = drain(sub, total + 4);
    uint64_t drops = ipc_channel_get_drop_count(sub);
    printf("[fault-slow] depth=%u published=%d delivered=%d drop_count=%" PRIu64 "\n",
           depth, total, got, drops);
    printf("[fault-slow] seqs:");
    for (int i = 0; i < log.n; i++) printf(" %u", log.seqs[i]);
    printf("\n");
    uint64_t expect_drop = (uint64_t)(total - (int)depth);
    int ok = (got == (int)depth && drops == expect_drop && log.n == (int)depth &&
              log.seqs[0] == (uint32_t)(total - (int)depth));
    printf("[fault-slow] %s（锚住游标后再淹没：drop_count 应等于 %u，只留下最后 %u 条）\n",
           ok ? "OK" : "UNEXPECTED", (unsigned)expect_drop, depth);
    ipc_channel_close(sub);
    ipc_channel_close(pub);
#if defined(__linux__)
    shm_unlink("/ch04_fault_slow_shm");
#endif
    return ok ? 0 : 1;
}

static int run_fault_late(void) {
    const char* name = "ch04_fault_late";
    const uint32_t depth = 8;
    const int total = 100;
    IpcChannel* pub = ipc_channel_open(name, IPC_ROLE_PUBLISHER, depth);
    if (!pub) return 1;
    for (int i = 0; i < total; i++) {
        uint32_t seq = (uint32_t)i;
        if (ipc_channel_publish(pub, "ch04/late", "pub", &seq, sizeof(seq)) != 0) {
            ipc_channel_close(pub);
            return 1;
        }
    }
    IpcChannel* sub = NULL;
    for (int i = 0; i < 50 && !sub; i++) {
        sub = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, /*请求 4，实际以发布端为准*/ 4);
        if (!sub) usleep(20000);
    }
    if (!sub) { ipc_channel_close(pub); return 1; }
    SeqLog log;
    memset(&log, 0, sizeof(log));
    ipc_channel_subscribe(sub, on_seq, &log);
    int got = drain(sub, total + 4);
    uint64_t drops = ipc_channel_get_drop_count(sub);
    printf("[fault-late] requested_sub_depth=4 delivered=%d drop_count=%" PRIu64 "\n",
           got, drops);
    printf("[fault-late] seqs:");
    for (int i = 0; i < log.n; i++) printf(" %u", log.seqs[i]);
    printf("\n");
    /* 晚加入把游标直接放在 head-depth，不把窗口之前的消息算进 drop_count。
     * 订阅端传入的 depth=4 会被发布端文件大小改写成 8。 */
    int ok = (got == (int)depth && drops == 0 && log.n == (int)depth &&
              log.seqs[0] == (uint32_t)(total - (int)depth));
    printf("[fault-late] %s（晚加入：drop_count 保持 0，仍能读到最后 %u 条）\n",
           ok ? "OK" : "UNEXPECTED", depth);
    ipc_channel_close(sub);
    ipc_channel_close(pub);
#if defined(__linux__)
    shm_unlink("/ch04_fault_late_shm");
#endif
    return ok ? 0 : 1;
}

#if defined(__linux__)

static int shm_inode(const char* path, ino_t* ino, off_t* size) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (ino) *ino = st.st_ino;
    if (size) *size = st.st_size;
    return 0;
}

/* 订阅端在父进程里，发布端是子进程。杀子进程后名字还在，已映射的订阅端
 * 仍能读到旧槽；新的发布端 shm_open 前会 unlink，旧映射看不到新数据。 */
typedef struct {
    int got_old;
    int got_new;
    char last[32];
} OrphanObs;

static void on_orphan(const Message* msg, void* user_data) {
    OrphanObs* o = (OrphanObs*)user_data;
    size_t n = msg->data_size < sizeof(o->last) - 1 ? msg->data_size : sizeof(o->last) - 1;
    memcpy(o->last, msg->data, n);
    o->last[n] = '\0';
    if (strcmp(o->last, "OLD") == 0) o->got_old++;
    if (strcmp(o->last, "NEW") == 0) o->got_new++;
}

static int run_fault_orphan(void) {
    const char* name = "ch04_orphan";
    const char* path = "/dev/shm/ch04_orphan_shm";
    shm_unlink("/ch04_orphan_shm");

    IpcChannel* pub = ipc_channel_open(name, IPC_ROLE_PUBLISHER, 8);
    if (!pub) return 1;
    if (ipc_channel_publish(pub, "ch04/orphan", "pub", "OLD", 3) != 0) {
        ipc_channel_close(pub);
        return 1;
    }
    ino_t ino_live = 0;
    off_t sz = 0;
    if (shm_inode(path, &ino_live, &sz) != 0) {
        fprintf(stderr, "[fault-orphan] 发布后 stat 失败\n");
        ipc_channel_close(pub);
        return 1;
    }
    printf("[fault-orphan] 发布端存活时 %s inode=%llu size=%lld\n",
           path, (unsigned long long)ino_live, (long long)sz);

    IpcChannel* sub = NULL;
    OrphanObs obs;
    memset(&obs, 0, sizeof(obs));
    for (int i = 0; i < 50 && !sub; i++)
        sub = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, 8);
    if (!sub) { ipc_channel_close(pub); return 1; }
    ipc_channel_subscribe(sub, on_orphan, &obs);

    ipc_channel_close(pub);
    pub = NULL;
    ino_t ino_after_close = 0;
    int still = shm_inode(path, &ino_after_close, NULL) == 0;
    printf("[fault-orphan] 正常 close 之后对象%s inode=%llu\n",
           still ? "仍在" : "已消失",
           still ? (unsigned long long)ino_after_close : 0ull);

    int got = drain(sub, 4);
    printf("[fault-orphan] close 之后旧订阅端读到 %d 条 last=\"%s\"\n", got, obs.last);

    /* 新发布端：open 内部先 unlink 再创建。旧订阅端的映射还指着旧对象。 */
    pub = ipc_channel_open(name, IPC_ROLE_PUBLISHER, 8);
    if (!pub) { ipc_channel_close(sub); return 1; }
    ino_t ino_new = 0;
    if (shm_inode(path, &ino_new, NULL) != 0) {
        fprintf(stderr, "[fault-orphan] 新发布端之后 stat 失败\n");
        ipc_channel_close(pub);
        ipc_channel_close(sub);
        return 1;
    }
    if (ipc_channel_publish(pub, "ch04/orphan", "pub", "NEW", 3) != 0) {
        ipc_channel_close(pub);
        ipc_channel_close(sub);
        return 1;
    }
    int got_after = drain(sub, 4);
    printf("[fault-orphan] 新 inode=%llu（与旧 %s）旧订阅端再读 %d 条 last=\"%s\" got_new=%d\n",
           (unsigned long long)ino_new,
           (ino_new == ino_after_close) ? "相同" : "不同",
           got_after, obs.last, obs.got_new);

    IpcChannel* sub2 = NULL;
    OrphanObs obs2;
    memset(&obs2, 0, sizeof(obs2));
    for (int i = 0; i < 50 && !sub2; i++)
        sub2 = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, 8);
    if (!sub2) {
        ipc_channel_close(pub);
        ipc_channel_close(sub);
        return 1;
    }
    ipc_channel_subscribe(sub2, on_orphan, &obs2);
    int got2 = drain(sub2, 4);
    printf("[fault-orphan] 新订阅端读到 %d 条 last=\"%s\"\n", got2, obs2.last);

    /* kill -9：子进程死在发布循环里，名字必须留下来。 */
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        IpcChannel* c = ipc_channel_open("ch04_orphan_kill", IPC_ROLE_PUBLISHER, 4);
        if (!c) _exit(2);
        uint32_t seq = 0;
        while (1) {
            ipc_channel_publish(c, "ch04/kill", "pub", &seq, sizeof(seq));
            seq++;
        }
    }
    usleep(50000);
    const char* kpath = "/dev/shm/ch04_orphan_kill_shm";
    int existed = shm_inode(kpath, NULL, NULL) == 0;
    if (kill(child, SIGKILL) != 0) {
        fprintf(stderr, "[fault-orphan] kill 失败 errno=%d\n", errno);
    }
    int status = 0;
    waitpid(child, &status, 0);
    int remains = shm_inode(kpath, NULL, NULL) == 0;
    printf("[fault-orphan] kill -9 前对象%s，之后对象%s（WIFSIGNALED=%d）\n",
           existed ? "存在" : "不存在",
           remains ? "仍在" : "消失",
           WIFSIGNALED(status));

    int ok = still && ino_after_close == ino_live && got >= 1 && obs.got_old >= 1 &&
             ino_new != ino_after_close && obs.got_new == 0 &&
             got2 >= 1 && obs2.got_new >= 1 && existed && remains;
    printf("[fault-orphan] %s\n", ok ? "OK" : "UNEXPECTED");

    ipc_channel_close(sub2);
    ipc_channel_close(sub);
    ipc_channel_close(pub);
    shm_unlink("/ch04_orphan_shm");
    shm_unlink("/ch04_orphan_kill_shm");
    return ok ? 0 : 1;
}

typedef struct {
    uint32_t good;
    uint32_t bad;
    uint32_t n;
    uint64_t first_seq;
    int have_first;
} CheckLog;

static void on_check(const Message* msg, void* user_data) {
    CheckLog* c = (CheckLog*)user_data;
    c->n++;
    if (msg->data_size < sizeof(SampleHdr) ||
        !sample_ok(msg->data, msg->data_size)) {
        c->bad++;
        return;
    }
    c->good++;
    if (!c->have_first) {
        c->first_seq = ((const SampleHdr*)msg->data)->seq;
        c->have_first = 1;
    }
}

static int run_fault_kill(void) {
    const int trials = 40;
    const uint32_t depth = 8;
    const uint32_t nbytes = 65536;
    int torn_trials = 0;
    int clean_trials = 0;
    int no_data = 0;
    int kill_fail = 0;

    printf("[fault-kill] trials=%d depth=%u payload=%u（子进程紧循环 publish，父进程 SIGKILL 后晚加入读窗口）\n",
           trials, depth, nbytes);

    for (int t = 0; t < trials; t++) {
        char name[64];
        snprintf(name, sizeof(name), "ch04_kill_%d", t);
        char shm_name[80];
        snprintf(shm_name, sizeof(shm_name), "/%s_shm", name);
        shm_unlink(shm_name);

        pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) {
            IpcChannel* ch = ipc_channel_open(name, IPC_ROLE_PUBLISHER, depth);
            if (!ch) _exit(2);
            uint8_t* buf = (uint8_t*)malloc(nbytes);
            if (!buf) _exit(3);
            uint64_t seq = 0;
            while (1) {
                fill_sample(buf, nbytes, seq);
                ((SampleHdr*)buf)->send_ns = now_ns();
                ipc_channel_publish(ch, "ch04/kill", "pub", buf, nbytes);
                seq++;
            }
        }

        /* 等环形缓冲至少写满一轮，再杀。 */
        int armed = 0;
        for (int i = 0; i < 200; i++) {
            char path[96];
            snprintf(path, sizeof(path), "/dev/shm/%s_shm", name);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size > 0) {
                armed = 1;
                break;
            }
            usleep(1000);
        }
        if (!armed) {
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            shm_unlink(shm_name);
            kill_fail++;
            continue;
        }
        usleep(20000);
        if (kill(child, SIGKILL) != 0) kill_fail++;
        int status = 0;
        waitpid(child, &status, 0);

        IpcChannel* sub = NULL;
        for (int i = 0; i < 20 && !sub; i++) {
            sub = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, depth);
            if (!sub) usleep(1000);
        }
        CheckLog log;
        memset(&log, 0, sizeof(log));
        if (sub) {
            ipc_channel_subscribe(sub, on_check, &log);
            for (int i = 0; i < (int)depth + 2; i++) {
                if (ipc_channel_recv_once(sub, 50) != 0) break;
            }
            ipc_channel_close(sub);
        }
        if (log.bad > 0) torn_trials++;
        else if (log.good > 0) clean_trials++;
        else no_data++;

        printf("[fault-kill] trial %02d good=%u bad=%u delivered=%u first_seq=%" PRIu64 "\n",
               t, log.good, log.bad, log.n,
               log.have_first ? log.first_seq : 0);
        shm_unlink(shm_name);
    }

    printf("[fault-kill] summary torn_trials=%d clean_trials=%d no_data=%d kill_fail=%d\n",
           torn_trials, clean_trials, no_data, kill_fail);
    /* 杀在临界区里时，最旧的那个仍在窗口内的槽可能是写了一半的。
     * 40 次里允许全部干净（窗口太窄），但不允许程序挂死。有 torn 就如实打印。 */
    return 0;
}

typedef struct {
    volatile int ready;
    volatile int stop;
    /* 0 = 不记录延迟，1 = 记录。只在父进程切换，子进程回调里读。 */
    volatile int phase;
    uint64_t e2e_ns[2048];
    volatile uint32_t e2e_n;
    volatile uint64_t delivered;
    uint64_t drops;
    uint32_t bad;
} BenchShared;

static void on_bench(const Message* msg, void* user_data) {
    BenchShared* sh = (BenchShared*)user_data;
    uint64_t recv = now_ns();
    __atomic_fetch_add(&sh->delivered, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&sh->phase, __ATOMIC_ACQUIRE) != 1) return;
    if (msg->data_size < sizeof(SampleHdr)) {
        __atomic_fetch_add(&sh->bad, 1, __ATOMIC_RELAXED);
        return;
    }
    const SampleHdr* h = (const SampleHdr*)msg->data;
    if (h->send_ns == 0 || recv < h->send_ns) return;
    uint32_t i = __atomic_fetch_add(&sh->e2e_n, 1, __ATOMIC_RELAXED);
    if (i < 2048) sh->e2e_ns[i] = recv - h->send_ns;
}

static void bench_child(BenchShared* sh, const char* name, uint32_t depth) {
    IpcChannel* ch = NULL;
    for (int i = 0; i < 100 && !ch; i++) {
        ch = ipc_channel_open(name, IPC_ROLE_SUBSCRIBER, depth);
        if (!ch) usleep(10000);
    }
    if (!ch) _exit(2);
    ipc_channel_subscribe(ch, on_bench, sh);
    __atomic_store_n(&sh->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE)) {
        if (ipc_channel_recv_once(ch, 200) != 0) {
            /* 超时只为了重新看 stop 标志 */
        }
    }
    /* 把发布端停手之前已经在环里的消息抽干。 */
    while (ipc_channel_recv_once(ch, 20) == 0) {}
    sh->drops = ipc_channel_get_drop_count(ch);
    ipc_channel_close(ch);
    _exit(0);
}

static int publish_n(IpcChannel* ch, uint8_t* buf, uint32_t nbytes,
                     uint64_t* seq, int n, unsigned pace_us,
                     uint64_t* call_ns, uint32_t call_cap, uint32_t* call_n) {
    for (int i = 0; i < n; i++) {
        fill_sample(buf, nbytes, *seq);
        ((SampleHdr*)buf)->send_ns = now_ns();
        uint64_t t0 = now_ns();
        int rc = ipc_channel_publish(ch, "ch04/bench", "pub", buf, nbytes);
        uint64_t dt = now_ns() - t0;
        if (rc != 0) return -1;
        if (call_ns && *call_n < call_cap) call_ns[(*call_n)++] = dt;
        (*seq)++;
        if (pace_us) usleep(pace_us);
    }
    return 0;
}

static void report_ns(const char* label, uint64_t* v, uint32_t n) {
    if (n == 0) {
        printf("  %-18s n=0\n", label);
        return;
    }
    uint64_t* copy = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    if (!copy) return;
    memcpy(copy, v, (size_t)n * sizeof(uint64_t));
    uint64_t mn = percentile(copy, n, 0);
    uint64_t p50 = percentile(copy, n, 50);
    uint64_t p99 = percentile(copy, n, 99);
    uint64_t mx = percentile(copy, n, 100);
    printf("  %-18s n=%-5u min=%7.1f us  p50=%7.1f us  p99=%7.1f us  max=%7.1f us\n",
           label, n, mn / 1000.0, p50 / 1000.0, p99 / 1000.0, mx / 1000.0);
    free(copy);
}

#endif /* __linux__ helpers */

#if defined(__linux__)

static int run_bench_real(void) {
    printf("[bench] host ");
    fflush(stdout);
    int ur = system("uname -srvm");
    (void)ur;
    FILE* cpu = fopen("/proc/cpuinfo", "r");
    if (cpu) {
        char line[256];
        int printed = 0;
        while (printed < 2 && fgets(line, sizeof(line), cpu)) {
            if (strncmp(line, "model name", 10) == 0 || strncmp(line, "cpu MHz", 7) == 0) {
                fputs(line, stdout);
                printed++;
            }
        }
        fclose(cpu);
    }
    printf("[bench] CPUs=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    printf("[bench] clock=CLOCK_MONOTONIC (ns). e2e = 回调入口时刻 - publish 调用前写入的 send_ns\n");
    printf("[bench] 这段 e2e 含：锁、memset 整份 Message、memcpy 负载、cond broadcast、对端被唤醒、对端再锁、整份 Message 赋值\n");
    printf("[bench] sizeof(Message)=%zu sizeof(ShmSlot)=%zu MSG_BUS_MAX_DATA_SIZE=%d\n",
           sizeof(Message), sizeof(Ch04ShmSlot), MSG_BUS_MAX_DATA_SIZE);

    const uint32_t depth = 32;
    const uint32_t sizes[] = {64u, 4096u, 65536u};

    printf("\n[bench] A. publish-only 无订阅者，预热 200 + 样本 2000，不限速\n");
    IpcChannel* solo = ipc_channel_open("ch04_bench_solo", IPC_ROLE_PUBLISHER, depth);
    if (!solo) return 1;
    for (unsigned s = 0; s < 3; s++) {
        uint32_t nbytes = sizes[s];
        uint8_t* buf = (uint8_t*)malloc(nbytes);
        uint64_t* calls = (uint64_t*)calloc(2000, sizeof(uint64_t));
        if (!buf || !calls) return 1;
        uint64_t seq = 0;
        uint32_t ncall = 0;
        if (publish_n(solo, buf, nbytes, &seq, 200, 0, NULL, 0, &ncall) != 0) return 1;
        ncall = 0;
        if (publish_n(solo, buf, nbytes, &seq, 2000, 0, calls, 2000, &ncall) != 0) return 1;
        char label[32];
        snprintf(label, sizeof(label), "solo %uB", nbytes);
        report_ns(label, calls, ncall);
        free(calls);
        free(buf);
    }
    ipc_channel_close(solo);
    shm_unlink("/ch04_bench_solo_shm");

    printf("\n[bench] B. 单程延迟：另一进程订阅，每条间隔 1000 us，每档预热 100 + 样本 500\n");
    printf("[bench] C. 吞吐：同一订阅者，不限速连续 publish 2000 条\n");

    for (unsigned s = 0; s < 3; s++) {
        uint32_t nbytes = sizes[s];
        char name[64];
        snprintf(name, sizeof(name), "ch04_bench_%u", nbytes);
        char shm_name[80];
        snprintf(shm_name, sizeof(shm_name), "/%s_shm", name);
        shm_unlink(shm_name);

        IpcChannel* pub = ipc_channel_open(name, IPC_ROLE_PUBLISHER, depth);
        if (!pub) return 1;

        BenchShared* sh = (BenchShared*)mmap(NULL, sizeof(BenchShared),
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (sh == MAP_FAILED) return 1;
        memset(sh, 0, sizeof(*sh));

        pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) bench_child(sh, name, depth);

        for (int i = 0; i < 300 && !__atomic_load_n(&sh->ready, __ATOMIC_ACQUIRE); i++)
            usleep(10000);
        if (!__atomic_load_n(&sh->ready, __ATOMIC_ACQUIRE)) {
            fprintf(stderr, "[bench] subscriber not ready\n");
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            ipc_channel_close(pub);
            return 1;
        }

        uint8_t* buf = (uint8_t*)malloc(nbytes);
        uint64_t* calls = (uint64_t*)calloc(500, sizeof(uint64_t));
        if (!buf || !calls) return 1;
        uint64_t seq = 0;
        uint32_t ncall = 0;
        __atomic_store_n(&sh->phase, 0, __ATOMIC_RELEASE);
        if (publish_n(pub, buf, nbytes, &seq, 100, 1000, NULL, 0, &ncall) != 0) return 1;
        usleep(20000);
        __atomic_store_n(&sh->e2e_n, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&sh->phase, 1, __ATOMIC_RELEASE);
        ncall = 0;
        if (publish_n(pub, buf, nbytes, &seq, 500, 1000, calls, 500, &ncall) != 0) return 1;
        usleep(20000);
        __atomic_store_n(&sh->phase, 0, __ATOMIC_RELEASE);
        usleep(20000);

        char label_pub[32], label_e2e[32];
        snprintf(label_pub, sizeof(label_pub), "paced-pub %uB", nbytes);
        snprintf(label_e2e, sizeof(label_e2e), "paced-e2e %uB", nbytes);
        printf("  -- payload %u bytes --\n", nbytes);
        report_ns(label_pub, calls, ncall);
        uint32_t e2e_n = __atomic_load_n(&sh->e2e_n, __ATOMIC_RELAXED);
        if (e2e_n > 2048) e2e_n = 2048;
        uint64_t* e2e_copy = (uint64_t*)malloc((size_t)(e2e_n ? e2e_n : 1) * sizeof(uint64_t));
        if (e2e_copy && e2e_n)
            memcpy(e2e_copy, sh->e2e_ns, (size_t)e2e_n * sizeof(uint64_t));
        report_ns(label_e2e, e2e_copy, e2e_n);
        uint32_t paced_bad = __atomic_load_n(&sh->bad, __ATOMIC_RELAXED);

        uint64_t delivered_before = __atomic_load_n(&sh->delivered, __ATOMIC_RELAXED);
        uint64_t t0 = now_ns();
        const int blast = 2000;
        if (publish_n(pub, buf, nbytes, &seq, blast, 0, NULL, 0, &ncall) != 0) return 1;
        uint64_t pub_ns = now_ns() - t0;
        usleep(100000);
        __atomic_store_n(&sh->stop, 1, __ATOMIC_RELEASE);
        int status = 0;
        waitpid(child, &status, 0);
        uint64_t delivered_after = __atomic_load_n(&sh->delivered, __ATOMIC_RELAXED);
        uint64_t delivered = delivered_after - delivered_before;
        double sec = pub_ns / 1e9;
        double payload_mib = (delivered * (double)nbytes) / (1024.0 * 1024.0);
        printf("  blast %uB  publish_loop=%.3f s  (%.0f publish/s)  delivered=%" PRIu64
               " (%.1f MiB payload)  session_drop_count=%" PRIu64
               "  bad=%u  child_status=%d\n",
               nbytes, sec, sec > 0 ? blast / sec : 0, delivered, payload_mib,
               sh->drops, paced_bad, status);

        free(e2e_copy);
        free(calls);
        free(buf);
        ipc_channel_close(pub);
        munmap(sh, sizeof(*sh));
        shm_unlink(shm_name);
    }
    return 0;
}

static int run_demo(void) {
    const char* name = "ch04_demo";
    shm_unlink("/ch04_demo_shm");
    pid_t sub = fork();
    if (sub < 0) return 1;
    if (sub == 0) {
        int rc = run_sub(name, 5);
        _exit(rc);
    }
    usleep(200000);
    pid_t pub = fork();
    if (pub < 0) return 1;
    if (pub == 0) {
        int rc = run_pub(name, 5);
        _exit(rc);
    }
    int st_sub = 1, st_pub = 1;
    waitpid(sub, &st_sub, 0);
    waitpid(pub, &st_pub, 0);
    shm_unlink("/ch04_demo_shm");
    printf("[demo] sub_status=%d pub_status=%d\n",
           WIFEXITED(st_sub) ? WEXITSTATUS(st_sub) : -1,
           WIFEXITED(st_pub) ? WEXITSTATUS(st_pub) : -1);
    return (WIFEXITED(st_sub) && WEXITSTATUS(st_sub) == 0 &&
            WIFEXITED(st_pub) && WEXITSTATUS(st_pub) == 0) ? 0 : 1;
}

#endif /* __linux__ second block */

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }
    /* 管道里 stdout 是全缓冲，fork 出去的子进程若直接 _exit 会把 printf 丢掉。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int count = -1;
    if (argc >= 3) count = atoi(argv[2]);

    if (strcmp(argv[1], "layout") == 0) return run_layout();
    if (strcmp(argv[1], "pub") == 0) return run_pub("ch04_walk", count);
    if (strcmp(argv[1], "sub") == 0) return run_sub("ch04_walk", count);
    if (strcmp(argv[1], "fault-slow") == 0) return run_fault_slow();
    if (strcmp(argv[1], "fault-late") == 0) return run_fault_late();

#if defined(__linux__)
    if (strcmp(argv[1], "demo") == 0) return run_demo();
    if (strcmp(argv[1], "bench") == 0) return run_bench_real();
    if (strcmp(argv[1], "fault-orphan") == 0) return run_fault_orphan();
    if (strcmp(argv[1], "fault-kill") == 0) return run_fault_kill();
#else
    if (strcmp(argv[1], "demo") == 0 || strcmp(argv[1], "bench") == 0 ||
        strcmp(argv[1], "fault-orphan") == 0 || strcmp(argv[1], "fault-kill") == 0) {
        fprintf(stderr, "%s 只在 Linux 上运行\n", argv[1]);
        return 2;
    }
#endif
    print_usage(argv[0]);
    return 2;
}
