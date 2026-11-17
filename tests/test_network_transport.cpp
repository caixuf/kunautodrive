/**
 * test_network_transport.cpp — NetworkTransport integration test.
 *
 * Regression: decode/drain used sizeof(Message) as the frame limit after
 * Message.data shrank to 4 KB, so any >4 KB bus payload killed the peer.
 */
#include "network_transport.h"
#include "message_bus.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

static std::atomic<int> g_received{0};
static std::atomic<int> g_bad{0};

static void on_large(const Message* msg, void* user_data) {
    (void)user_data;
    const uint8_t* data = (const uint8_t*)message_bus_message_data(msg);
    if (!msg || !data || msg->data_size != 10000) {
        g_bad.fetch_add(1);
        return;
    }
    for (uint32_t i = 0; i < msg->data_size; ++i) {
        if (data[i] != (uint8_t)('A' + (i % 26))) {
            g_bad.fetch_add(1);
            return;
        }
    }
    g_received.fetch_add(1);
}

int main(void) {
    MessageBus* bus_a = message_bus_create("net_test_a");
    MessageBus* bus_b = message_bus_create("net_test_b");
    if (!bus_a || !bus_b) return 1;

    NetworkTransport* a = net_transport_create("127.0.0.1", 17800, bus_a, nullptr);
    NetworkTransport* b = net_transport_create("127.0.0.1", 17801, bus_b, nullptr);
    if (!a || !b) return 2;

    if (net_transport_bridge_topic(a, "net/large") != 0) return 3;
    message_bus_subscribe(bus_b, "net/large", on_large, nullptr);

    if (net_transport_start(a) != 0 || net_transport_start(b) != 0) return 4;
    if (net_transport_connect(a, "127.0.0.1", 17801) != 0) return 5;

    for (int i = 0; i < 200 && net_transport_connection_count(a) <= 0; ++i) {
        usleep(10000);
    }
    if (net_transport_connection_count(a) <= 0) {
        std::fprintf(stderr, "no TCP connection\n");
        return 6;
    }

    uint8_t* payload = (uint8_t*)malloc(10000);
    if (!payload) return 7;
    for (uint32_t i = 0; i < 10000; ++i) payload[i] = (uint8_t)('A' + (i % 26));
    int rc = message_bus_publish(bus_a, "net/large", "net_test", payload, 10000);
    free(payload);
    if (rc != 0) return 8;

    for (int i = 0; i < 300 && g_received.load() == 0; ++i) usleep(10000);

    const int received = g_received.load();
    const int bad = g_bad.load();
    net_transport_stop(a);
    net_transport_stop(b);
    net_transport_destroy(a);
    net_transport_destroy(b);
    message_bus_destroy(bus_a);
    message_bus_destroy(bus_b);

    if (received != 1 || bad != 0) {
        std::fprintf(stderr, "large payload failed: received=%d bad=%d\n", received, bad);
        return 9;
    }
    std::printf("network transport >4KB roundtrip: PASS\n");
    return 0;
}
