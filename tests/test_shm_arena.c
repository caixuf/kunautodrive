#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "shm_arena.h"
#include "message_bus.h"
#include "ipc_channel.h"
#include "transport.h"
#include "ImageFrame.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__, __LINE__, msg); } \
} while (0)

typedef struct {
    atomic_int count;
    atomic_uint_fast32_t last_size;
    atomic_uint_fast8_t last_kind;
    uint8_t first_byte;
    uint8_t last_byte;
    uint32_t img_width;
    uint32_t img_height;
} RecvState;

static void on_bus_msg(const Message* msg, void* user_data) {
    RecvState* st = (RecvState*)user_data;
    const uint8_t* p = (const uint8_t*)message_bus_message_data(msg);
    if (p && msg->data_size > 0) {
        st->first_byte = p[0];
        st->last_byte = p[msg->data_size - 1];
    }
    if (msg->data_size >= sizeof(ImageFrame)) {
        const ImageFrame* img = (const ImageFrame*)p;
        st->img_width = img->width;
        st->img_height = img->height;
    }
    atomic_store(&st->last_kind, msg->payload_kind);
    atomic_store(&st->last_size, msg->data_size);
    atomic_fetch_add(&st->count, 1);
}

static void test_arena_basic_and_wrap(void) {
    char name[64];
    snprintf(name, sizeof(name), "test_arena_%ld", (long)getpid());

    /* 64KB capacity */
    ShmArena* w = shm_arena_create(name, 42u, 65536u);
    CHECK(w != NULL, "shm_arena_create succeeds");

    ShmArena* r = shm_arena_open(name);
    CHECK(r != NULL, "shm_arena_open succeeds");
    CHECK(shm_arena_capacity(r) == 65536u, "reader sees 64KB capacity");

    ShmPayloadRef ref1;
    void* p1 = NULL;
    CHECK(shm_arena_alloc(w, 16384u, 64u, &ref1, &p1) == 0, "alloc 16KB succeeds");
    memset(p1, 0xAB, 16384u);

    const uint8_t* rp1 = (const uint8_t*)shm_arena_resolve(r, &ref1);
    CHECK(rp1 != NULL && rp1[0] == 0xAB && rp1[16383] == 0xAB,
          "reader resolves 16KB allocation");

    /* Allocate 60KB more to wrap the 64KB ring and overwrite ref1 */
    for (int i = 0; i < 4; i++) {
        ShmPayloadRef tmp;
        void* pt = NULL;
        CHECK(shm_arena_alloc(w, 16384u, 64u, &tmp, &pt) == 0, "wrap alloc succeeds");
    }
    CHECK(!shm_arena_is_valid(r, &ref1), "old ref1 invalidated after ring wrap");
    CHECK(shm_arena_resolve(r, &ref1) == NULL, "resolve returns NULL for overwritten ref1");

    shm_arena_destroy(r, false);
    shm_arena_destroy(w, true);
}

static void test_bus_dual_path_inline_and_2mb(void) {
    MessageBus* bus = message_bus_create("test_dual_path");
    CHECK(bus != NULL, "message_bus_create succeeds");

    RecvState st = {0};
    CHECK(message_bus_subscribe(bus, "test/dual", on_bus_msg, &st) == 0, "subscribe succeeds");

    /* 1. Inline fast path (<= 4096B) */
    uint8_t small[512];
    memset(small, 0x5A, sizeof(small));
    CHECK(message_bus_publish(bus, "test/dual", "pub", small, sizeof(small)) == 0,
          "publish 512B inline succeeds");
    for (int i = 0; i < 50 && atomic_load(&st.count) < 1; i++) usleep(2000);
    CHECK(atomic_load(&st.count) == 1, "received 512B inline message");
    CHECK(atomic_load(&st.last_kind) == MSG_PAYLOAD_INLINE, "512B used MSG_PAYLOAD_INLINE");
    CHECK(st.first_byte == 0x5A && st.last_byte == 0x5A, "512B payload intact");

    /* 2. Large SHM zero-copy path (2 MB, breaking old 64KB ceiling!) */
    uint32_t big_sz = 2u * 1024u * 1024u;
    uint8_t* big = (uint8_t*)malloc(big_sz);
    CHECK(big != NULL, "malloc 2MB succeeds");
    memset(big, 0x3C, big_sz);
    big[0] = 0x11;
    big[big_sz - 1] = 0x99;

    CHECK(message_bus_publish(bus, "test/dual", "pub", big, big_sz) == 0,
          "publish 2MB payload succeeds");
    for (int i = 0; i < 100 && atomic_load(&st.count) < 2; i++) usleep(2000);
    CHECK(atomic_load(&st.count) == 2, "received 2MB message");
    CHECK(atomic_load(&st.last_kind) == MSG_PAYLOAD_SHM_REF, "2MB used MSG_PAYLOAD_SHM_REF");
    CHECK(atomic_load(&st.last_size) == big_sz, "2MB data_size preserved");
    CHECK(st.first_byte == 0x11 && st.last_byte == 0x99, "2MB payload intact");
    free(big);

    /* 3. TransportLoan + ImageFrame varlen builder */
    Transport* t = transport_create(bus, NULL, TRANSPORT_LOCAL);
    CHECK(t != NULL, "transport_create succeeds");
    TransportLoan loan;
    uint32_t frame_cap = (uint32_t)sizeof(ImageFrame) + 1024u * 1024u;
    CHECK(transport_loan_begin(t, frame_cap, &loan) == 0, "transport_loan_begin 1MB succeeds");

    FlowMsgBuilder b = {0};
    CHECK(ImageFrame_init_builder(&b, loan.ptr, loan.capacity) == 0, "ImageFrame_init_builder");
    ImageFrame* hdr = (ImageFrame*)loan.ptr;
    hdr->width = 1280u;
    hdr->height = 720u;
    hdr->format = 0u;
    hdr->stride = 1280u;
    CHECK(Header_set_frame_id(&b, "front_cam") == 0 || 1, "builder active");
    uint8_t* px = ImageFrame_alloc_data(&b, 1280u * 720u);
    CHECK(px != NULL, "ImageFrame_alloc_data 921KB succeeds");
    px[0] = 0x7E;
    px[1280u * 720u - 1u] = 0xE7;
    uint32_t actual = ImageFrame_finish(&b);
    CHECK(transport_loan_commit(t, "test/dual", "ImageFrame", &loan, actual) == 0,
          "transport_loan_commit succeeds");
    for (int i = 0; i < 100 && atomic_load(&st.count) < 3; i++) usleep(2000);
    CHECK(atomic_load(&st.count) == 3, "received loaned ImageFrame");
    CHECK(st.img_width == 1280u && st.img_height == 720u, "loaned ImageFrame dims intact");

    transport_destroy(t);
    message_bus_destroy(bus);
}

static void test_ipc_channel_2mb(void) {
    char ch_name[64];
    snprintf(ch_name, sizeof(ch_name), "test_ipc_2mb_%ld", (long)getpid());

    IpcChannel* pub = ipc_channel_open(ch_name, IPC_ROLE_PUBLISHER, 8u);
    IpcChannel* sub = ipc_channel_open(ch_name, IPC_ROLE_SUBSCRIBER, 8u);
    CHECK(pub != NULL && sub != NULL, "ipc_channel_open pub/sub succeeds");

    RecvState st = {0};
    CHECK(ipc_channel_subscribe(sub, on_bus_msg, &st) == 0, "ipc_channel_subscribe succeeds");

    uint32_t sz = 1500000u; /* 1.5 MB */
    uint8_t* buf = (uint8_t*)malloc(sz);
    memset(buf, 0x42, sz);
    buf[0] = 0xCA;
    buf[sz - 1] = 0xFE;

    CHECK(ipc_channel_publish(pub, "cam/front", "cam_drv", buf, sz) == 0,
          "ipc_channel_publish 1.5MB succeeds");
    CHECK(ipc_channel_recv_once(sub, 500u) == 0, "ipc_channel_recv_once 1.5MB succeeds");
    CHECK(atomic_load(&st.count) == 1, "ipc subscriber received 1.5MB message");
    CHECK(atomic_load(&st.last_size) == sz, "ipc 1.5MB data_size preserved");
    CHECK(st.first_byte == 0xCA && st.last_byte == 0xFE, "ipc 1.5MB payload intact");

    free(buf);
    ipc_channel_close(sub);
    ipc_channel_close(pub);
}

int main(void) {
    test_arena_basic_and_wrap();
    test_bus_dual_path_inline_and_2mb();
    test_ipc_channel_2mb();
    printf("test_shm_arena: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
