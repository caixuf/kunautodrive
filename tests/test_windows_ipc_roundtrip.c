/**
 * test_windows_ipc_roundtrip.c — dashboard IPC 65KB roundtrip.
 *
 * The dashboard bridge chunks JSON into ~65KB DashboardChunk messages. On
 * Win32 Message.data is a full 64KB inline buffer; publishing/subscribing must
 * preserve all 65,532 bytes. This test is built and run on windows-latest and
 * also runs on POSIX (where the same payload goes through the shm arena).
 */
#include "dashboard_bridge.h"
#include "ipc_channel.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((unsigned)ms * 1000u); }
#endif

#define PAYLOAD_LEN 70000

static char g_payload[PAYLOAD_LEN];
static char g_received[PAYLOAD_LEN + 1];
static atomic_int g_got;
static atomic_int g_bad;
static atomic_size_t g_len;
static size_t g_expected_len;

static void on_json(const char* json, size_t len, void* user_data) {
    (void)user_data;
    if (!json || len != g_expected_len) {
        atomic_store(&g_bad, 1);
        return;
    }
    memcpy(g_received, json, len);
    g_received[len] = '\0';
    atomic_store(&g_len, len);
    atomic_store(&g_got, 1);
}

int main(void) {
    /* Valid JSON-ish payload with a unique head/tail; bridge does not parse it,
     * it only chunk-reassembles the bytes. */
    snprintf(g_payload, sizeof(g_payload), "{\"head\":\"");
    size_t pos = strlen(g_payload);
    while (pos < PAYLOAD_LEN - 32) {
        g_payload[pos] = (char)('A' + (pos % 26));
        pos++;
    }
    memcpy(g_payload + pos, "\",\"tail\":\"ok_70kb\"}", 19);
    g_payload[PAYLOAD_LEN - 1] = '\0';
    const size_t payload_len = strlen(g_payload);

    IpcChannel* pub = dashboard_bridge_publisher_open();
    if (!pub) {
        fprintf(stderr, "publisher open failed\n");
        return 1;
    }

    IpcChannel* sub = NULL;
    for (int i = 0; i < 40 && !sub; ++i) {
        sub = dashboard_bridge_subscriber_open(on_json, NULL);
        if (!sub) sleep_ms(50);
    }
    if (!sub) {
        fprintf(stderr, "subscriber open failed\n");
        return 2;
    }
    ipc_channel_start(sub);
    g_expected_len = payload_len;

    if (dashboard_bridge_publish(pub, g_payload, payload_len) != 0) {
        fprintf(stderr, "publish failed (len=%zu)\n", payload_len);
        return 3;
    }

    for (int i = 0; i < 200 && !atomic_load(&g_got); ++i) sleep_ms(25);
    const int got = atomic_load(&g_got);
    const int bad = atomic_load(&g_bad);
    const size_t got_len = atomic_load(&g_len);

    ipc_channel_stop(sub);
    dashboard_bridge_subscriber_close(sub);
    ipc_channel_close(pub);

    if (!got || bad || got_len != payload_len ||
        memcmp(g_received, g_payload, payload_len) != 0) {
        fprintf(stderr, "65KB dashboard roundtrip FAILED: got=%d bad=%d len=%zu/%zu\n",
                got, bad, got_len, payload_len);
        return 4;
    }
    printf("65KB dashboard IPC roundtrip: PASS (len=%zu)\n", got_len);
    return 0;
}
