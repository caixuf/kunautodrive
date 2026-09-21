/**
 * benchmark_tcp.c — localhost loopback TCP 传输基准
 *
 * 同一进程内两个 NetworkTransport 端点，在 127.0.0.1 上桥接 topic：
 *   sender bus  --bridge-->  TCP loopback  --recv publish-->  receiver bus
 *
 * 这测的是 **localhost loopback TCP**（本机内核回环），不是：
 *   - 网卡 / 线缆 / 跨主机 RTT
 *   - 进程内 MessageBus（见 src/benchmark.c，通常 100k+ msg/s）
 *
 * 线上帧 = 4 字节长度前缀 + Message 固定头 + data_size 字节有效负载
 * （不再把整份 64KB Message 送上线）。recv 用 poll + 64KB 排空读。
 *
 * 构建:
 *   cmake --build build --target benchmark_tcp
 *   # 或 bash build.sh bench（同时跑进程内 + TCP 回环）
 *
 * 运行:
 *   ./build/bin/benchmark_tcp
 *   ./build/bin/benchmark_tcp 8000          # 吞吐消息数（默认 4096）
 *   ./build/bin/benchmark_tcp --count 8000 --port 19771
 *
 * 不在默认 ctest 里跑（LABELS=benchmark;manual），与 benchmark_coro 相同。
 */

#include "network_transport.h"
#include "message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
int main(void) {
    fprintf(stderr, "benchmark_tcp: NetworkTransport is a stub on Windows.\n");
    return 0;
}
#else

/* ── 计时 / 统计（与 src/benchmark.c 同形）──────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

typedef struct {
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t avg_ns;
    uint64_t p50_ns;
    uint64_t p99_ns;
} Stats;

static Stats calc_stats(uint64_t* samples, int n) {
    Stats empty = {0};
    if (n <= 0) return empty;
    uint64_t* sorted = malloc((size_t)n * sizeof(uint64_t));
    if (!sorted) {
        fprintf(stderr, "calc_stats: malloc failed\n");
        return empty;
    }
    memcpy(sorted, samples, (size_t)n * sizeof(uint64_t));
    qsort(sorted, (size_t)n, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += sorted[i];
    Stats s = {
        .min_ns = sorted[0],
        .max_ns = sorted[n - 1],
        .avg_ns = sum / (uint64_t)n,
        .p50_ns = sorted[n / 2],
        .p99_ns = sorted[(int)((n - 1) * 0.99)],
    };
    free(sorted);
    return s;
}

static void print_sep(void) {
    printf("─────────────────────────────────────────────────────────────────────\n");
}

static void print_bench_header(const char* title) {
    printf("\n");
    print_sep();
    printf("  %s\n", title);
    print_sep();
}

static void print_latency_row(const char* label, Stats s) {
    printf("  %-38s min=%6.1f  avg=%6.1f  p50=%6.1f  p99=%6.1f  max=%6.1f  (µs)\n",
           label,
           s.min_ns / 1e3, s.avg_ns / 1e3,
           s.p50_ns / 1e3, s.p99_ns / 1e3, s.max_ns / 1e3);
}

/* ── 载荷 / 接收侧 ──────────────────────────────────────── */

#define BENCH_PAYLOAD_BYTES 64
#define BENCH_TOPIC         "bench/tcp_loopback"
#define DEFAULT_COUNT       4096
#define DEFAULT_RECV_PORT   19771
#define SERIAL_LATENCY_N    50
#define THRU_IN_FLIGHT      32
#define CONNECT_TIMEOUT_MS  2000
#define DELIVER_TIMEOUT_MS  20000

static size_t bench_wire_frame_bytes(void) {
    return 4u + (size_t)NET_WIRE_HEADER_SIZE + (size_t)BENCH_PAYLOAD_BYTES;
}

typedef struct {
    uint64_t seq;
    uint64_t send_ns;
} BenchPayload;

typedef struct {
    atomic_uint_fast64_t recv_count;
    uint64_t*            lat_ns;     /* 按下标 seq 写入；可为 NULL */
    int                  lat_cap;
} RecvState;

static void recv_cb(const Message* msg, void* user) {
    RecvState* st = (RecvState*)user;
    uint64_t t = now_ns();
    const void* data = message_bus_message_data(msg);
    if (data && msg->data_size >= sizeof(BenchPayload) && st->lat_ns) {
        BenchPayload p;
        memcpy(&p, data, sizeof(p));
        if (p.seq < (uint64_t)st->lat_cap && p.send_ns != 0)
            st->lat_ns[p.seq] = t - p.send_ns;
    }
    atomic_fetch_add(&st->recv_count, 1);
}

static int wait_until_count(RecvState* st, uint64_t target, int timeout_ms) {
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    while (atomic_load(&st->recv_count) < target) {
        if (now_ns() >= deadline) return -1;
        usleep(1000);
    }
    return 0;
}

static int wait_connected(NetworkTransport* a, NetworkTransport* b, int timeout_ms) {
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    while (now_ns() < deadline) {
        int ca = net_transport_connection_count(a);
        int cb = net_transport_connection_count(b);
        if (ca >= 1 && cb >= 1) return 0;
        usleep(2000);
    }
    return -1;
}

static void fill_payload(uint8_t* buf, size_t len, uint64_t seq, uint64_t send_ns) {
    memset(buf, 0xAB, len);
    BenchPayload p = { .seq = seq, .send_ns = send_ns };
    memcpy(buf, &p, sizeof(p));
}

/* ── 端点搭建 ───────────────────────────────────────────── */

typedef struct {
    MessageBus*        sender_bus;
    MessageBus*        receiver_bus;
    NetworkTransport*  sender;
    NetworkTransport*  receiver;
    uint16_t           recv_port;
} LoopbackPair;

static void pair_destroy(LoopbackPair* p) {
    if (!p) return;
    if (p->sender) net_transport_destroy(p->sender);
    if (p->receiver) net_transport_destroy(p->receiver);
    if (p->sender_bus) message_bus_destroy(p->sender_bus);
    if (p->receiver_bus) message_bus_destroy(p->receiver_bus);
    memset(p, 0, sizeof(*p));
}

static int pair_start(LoopbackPair* p, uint16_t base_port) {
    memset(p, 0, sizeof(*p));
    for (int attempt = 0; attempt < 8; attempt++) {
        uint16_t recv_port = (uint16_t)(base_port + (uint16_t)attempt * 2);
        uint16_t send_port = (uint16_t)(recv_port + 1);

        p->sender_bus = message_bus_create("tcp_bench_tx");
        p->receiver_bus = message_bus_create("tcp_bench_rx");
        if (!p->sender_bus || !p->receiver_bus) {
            pair_destroy(p);
            return -1;
        }

        p->receiver = net_transport_create("127.0.0.1", recv_port, p->receiver_bus, NULL);
        p->sender = net_transport_create("127.0.0.1", send_port, p->sender_bus, NULL);
        if (!p->receiver || !p->sender) {
            pair_destroy(p);
            return -1;
        }

        if (net_transport_bridge_topic(p->sender, BENCH_TOPIC) != 0) {
            pair_destroy(p);
            return -1;
        }
        if (net_transport_start(p->receiver) != 0 ||
            net_transport_start(p->sender) != 0) {
            pair_destroy(p);
            continue;
        }
        if (net_transport_connect(p->sender, "127.0.0.1", recv_port) != 0) {
            pair_destroy(p);
            continue;
        }
        if (wait_connected(p->sender, p->receiver, CONNECT_TIMEOUT_MS) != 0) {
            pair_destroy(p);
            continue;
        }
        p->recv_port = recv_port;
        return 0;
    }
    return -1;
}

static int pair_alive(const LoopbackPair* p) {
    return net_transport_connection_count(p->sender) >= 1
        && net_transport_connection_count(p->receiver) >= 1;
}

/* ── 吞吐：有界 in-flight，避免瞬时撑满非阻塞 send buffer。── */

static int bench_throughput(LoopbackPair* pair, int total) {
    print_bench_header("localhost loopback TCP — 持续吞吐量 (bounded in-flight)");

    RecvState st;
    memset(&st, 0, sizeof(st));
    atomic_store(&st.recv_count, 0);
    st.lat_cap = total;
    st.lat_ns = calloc((size_t)total, sizeof(uint64_t));
    if (!st.lat_ns) {
        fprintf(stderr, "throughput: malloc failed\n");
        return -1;
    }

    if (message_bus_subscribe(pair->receiver_bus, BENCH_TOPIC, recv_cb, &st) != 0) {
        fprintf(stderr, "throughput: subscribe failed\n");
        free(st.lat_ns);
        return -1;
    }

    uint8_t payload[BENCH_PAYLOAD_BYTES];
    {
        fill_payload(payload, sizeof(payload), 0, now_ns());
        message_bus_publish(pair->sender_bus, BENCH_TOPIC, "bench_tcp",
                            payload, sizeof(payload));
        if (wait_until_count(&st, 1, DELIVER_TIMEOUT_MS) != 0) {
            fprintf(stderr, "throughput: warmup delivery timeout\n");
            free(st.lat_ns);
            return -1;
        }
        atomic_store(&st.recv_count, 0);
        memset(st.lat_ns, 0, (size_t)total * sizeof(uint64_t));
    }

    uint64_t t0 = now_ns();
    for (int seq = 0; seq < total; seq++) {
        fill_payload(payload, sizeof(payload), (uint64_t)seq, now_ns());
        if (message_bus_publish(pair->sender_bus, BENCH_TOPIC, "bench_tcp",
                                payload, sizeof(payload)) != 0) {
            fprintf(stderr, "throughput: publish failed at seq=%d\n", seq);
            free(st.lat_ns);
            return -1;
        }
        /* 保持至多 THRU_IN_FLIGHT 条在途，避免撑死非阻塞 send。 */
        uint64_t min_recv = (uint64_t)(seq + 1 - THRU_IN_FLIGHT);
        if (seq + 1 >= THRU_IN_FLIGHT &&
            wait_until_count(&st, min_recv, DELIVER_TIMEOUT_MS) != 0) {
            fprintf(stderr, "throughput: delivery timeout at %llu / %d (peers tx=%d rx=%d)\n",
                    (unsigned long long)atomic_load(&st.recv_count), total,
                    net_transport_connection_count(pair->sender),
                    net_transport_connection_count(pair->receiver));
            if (!pair_alive(pair))
                fprintf(stderr, "throughput: connection dropped "
                        "(NetworkTransport closes on send-backpressure / 100ms poll)\n");
            free(st.lat_ns);
            return -1;
        }
    }
    if (wait_until_count(&st, (uint64_t)total, DELIVER_TIMEOUT_MS) != 0) {
        fprintf(stderr, "throughput: drain timeout at %llu / %d\n",
                (unsigned long long)atomic_load(&st.recv_count), total);
        free(st.lat_ns);
        return -1;
    }
    uint64_t t1 = now_ns();

    double elapsed_s = (t1 - t0) / 1e9;
    double throughput = total / elapsed_s;

    int lat_n = 0;
    uint64_t* lat_ok = malloc((size_t)total * sizeof(uint64_t));
    if (!lat_ok) {
        fprintf(stderr, "throughput: malloc failed\n");
        free(st.lat_ns);
        return -1;
    }
    for (int i = 0; i < total; i++) {
        if (st.lat_ns[i] > 0) lat_ok[lat_n++] = st.lat_ns[i];
    }
    Stats lat = calc_stats(lat_ok, lat_n);

    printf("  路径:            127.0.0.1 NetworkTransport 桥接（同进程双端）\n");
    printf("  不是:            NIC/线缆，也不是进程内 MessageBus\n");
    printf("  载荷:            %d B 用户数据 / 线帧 = %zu B（头 + data_size，非整份 Message）\n",
           BENCH_PAYLOAD_BYTES, bench_wire_frame_bytes());
    printf("  消息数:          %d（in-flight≤%d）\n", total, THRU_IN_FLIGHT);
    printf("  总耗时:          %.3f ms\n", elapsed_s * 1000.0);
    printf("  吞吐:            %.0f msg/s   (%.1f MB/s wire frames)\n",
           throughput,
           throughput * (double)bench_wire_frame_bytes() / 1e6);
    print_latency_row("in-flight 单向延迟 (嵌入时间戳)", lat);
    printf("  注: localhost loopback；recv 空闲 200µs tick + 总线拷贝，不是 NIC/线缆。\n");

    NetTransportStats tx = {0}, rx = {0};
    net_transport_get_stats(pair->sender, &tx);
    net_transport_get_stats(pair->receiver, &rx);
    printf("  sender stats:    sent=%llu  recv=%llu  send_err=%llu  peers=%d\n",
           (unsigned long long)tx.msgs_sent, (unsigned long long)tx.msgs_received,
           (unsigned long long)tx.send_errors, net_transport_connection_count(pair->sender));
    printf("  receiver stats:  sent=%llu  recv=%llu  peers=%d\n",
           (unsigned long long)rx.msgs_sent, (unsigned long long)rx.msgs_received,
           net_transport_connection_count(pair->receiver));

    free(lat_ok);
    free(st.lat_ns);
    return 0;
}

/* ── 串行 ping：一条等一条，暴露 recv poll ──────────────── */

static int bench_serial_latency(LoopbackPair* pair, int n) {
    print_bench_header("localhost loopback TCP — 串行单向 ping");

    RecvState st;
    memset(&st, 0, sizeof(st));
    atomic_store(&st.recv_count, 0);
    st.lat_cap = n;
    st.lat_ns = calloc((size_t)n, sizeof(uint64_t));
    if (!st.lat_ns) {
        fprintf(stderr, "serial: malloc failed\n");
        return -1;
    }

    if (message_bus_subscribe(pair->receiver_bus, BENCH_TOPIC, recv_cb, &st) != 0) {
        fprintf(stderr, "serial: subscribe failed\n");
        free(st.lat_ns);
        return -1;
    }

    uint8_t payload[BENCH_PAYLOAD_BYTES];
    const int warmup = 8;
    for (int i = 0; i < warmup; i++) {
        fill_payload(payload, sizeof(payload), 0, now_ns());
        message_bus_publish(pair->sender_bus, BENCH_TOPIC, "bench_tcp",
                            payload, sizeof(payload));
        if (wait_until_count(&st, (uint64_t)(i + 1), DELIVER_TIMEOUT_MS) != 0) {
            fprintf(stderr, "serial: warmup timeout\n");
            free(st.lat_ns);
            return -1;
        }
    }
    atomic_store(&st.recv_count, 0);
    memset(st.lat_ns, 0, (size_t)n * sizeof(uint64_t));

    for (int i = 0; i < n; i++) {
        fill_payload(payload, sizeof(payload), (uint64_t)i, now_ns());
        if (message_bus_publish(pair->sender_bus, BENCH_TOPIC, "bench_tcp",
                                payload, sizeof(payload)) != 0) {
            fprintf(stderr, "serial: publish failed at %d\n", i);
            free(st.lat_ns);
            return -1;
        }
        if (wait_until_count(&st, (uint64_t)(i + 1), DELIVER_TIMEOUT_MS) != 0) {
            fprintf(stderr, "serial: delivery timeout at %d\n", i);
            free(st.lat_ns);
            return -1;
        }
    }

    int lat_n = 0;
    uint64_t* lat_ok = malloc((size_t)n * sizeof(uint64_t));
    if (!lat_ok) {
        fprintf(stderr, "serial: malloc failed\n");
        free(st.lat_ns);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (st.lat_ns[i] > 0) lat_ok[lat_n++] = st.lat_ns[i];
    }
    Stats lat = calc_stats(lat_ok, lat_n);
    char lat_label[64];
    snprintf(lat_label, sizeof(lat_label), "串行 ping 单向 (N=%d)", n);
    print_latency_row(lat_label, lat);
    printf("  等效串行吞吐:    %.0f msg/s\n",
           lat.avg_ns ? 1e9 / (double)lat.avg_ns : 0.0);
    printf("  注: 串行 ping 含 recv 空闲 tick（200µs）+ 总线分发；localhost loopback，不是 NIC RTT。\n");

    free(lat_ok);
    free(st.lat_ns);
    return 0;
}

/* ── CLI ────────────────────────────────────────────────── */

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [throughput_count]\n"
            "       %s [--count N] [--port PORT]\n"
            "\n"
            "  localhost loopback TCP bench via NetworkTransport (not NIC, not in-process bus).\n"
            "  default count=%d  default listen port=%d (retries nearby ports if busy)\n",
            argv0, argv0, DEFAULT_COUNT, DEFAULT_RECV_PORT);
}

int main(int argc, char** argv) {
    int count = DEFAULT_COUNT;
    uint16_t port = DEFAULT_RECV_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "invalid --port\n");
                return 1;
            }
            port = (uint16_t)p;
            continue;
        }
        if (argv[i][0] != '-') {
            count = atoi(argv[i]);
            continue;
        }
        usage(argv[0]);
        return 1;
    }
    if (count < 1) count = DEFAULT_COUNT;

    printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║     NetworkTransport 基准 — localhost loopback TCP                ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n");
    printf("  标签: localhost loopback TCP（127.0.0.1）\n");
    printf("  API:  net_transport_create / bridge_topic / connect / start\n");
    printf("  对比: 进程内总线请跑 ./build/bin/benchmark  （~100k+ msg/s）\n");

    LoopbackPair thru;
    if (pair_start(&thru, port) != 0) {
        fprintf(stderr, "failed to start loopback pair on 127.0.0.1 near port %u\n", port);
        return 1;
    }
    printf("  监听: receiver 127.0.0.1:%u  ← sender connect\n", thru.recv_port);

    uint16_t used_port = thru.recv_port;
    int rc = bench_throughput(&thru, count);
    pair_destroy(&thru);
    if (rc != 0) return 1;

    LoopbackPair ping;
    if (pair_start(&ping, (uint16_t)(used_port + 10)) != 0 &&
        pair_start(&ping, port) != 0) {
        fprintf(stderr, "failed to start loopback pair for serial ping\n");
        return 1;
    }
    rc = bench_serial_latency(&ping, SERIAL_LATENCY_N);
    pair_destroy(&ping);

    printf("\n");
    print_sep();
    printf("  localhost loopback TCP 基准完成\n");
    print_sep();
    printf("\n");
    return rc != 0 ? 1 : 0;
}

#endif /* !_WIN32 */
