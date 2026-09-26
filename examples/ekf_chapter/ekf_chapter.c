/**
 * ekf_chapter.c — 第 14 章配套程序。
 *
 * 离线子命令直接调用 ekf_fusion / ekf_slam。
 * fault-live 先作为 sensor/lidar 与 sensor/pose 的发布端，再拉起
 * flow_node_host 里的 fusion 节点，订阅 fusion/localization。
 * 不修改节点源码。位姿负载默认按 C 结构体原样发送（与 sizeof 一致），
 * serialize 子命令改用 Pose2D_serialize，用来对照 slam_node 的字节数。
 */

#include "ekf_fusion.h"

#ifdef EKF_STATE_DIM
#undef EKF_STATE_DIM
#endif
#include "ekf_slam.h"
#include "slam_math.h"
#include "adas_msgs_gen.h"

#include "message_bus.h"
#include "transport.h"
#include "discovery.h"
#include "clock_service.h"
#include "logger.h"
#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void usage(void) {
    fprintf(stderr,
            "usage: ekf_chapter <layout|demo|bench|fault-gate|fault-wrap|stamp|fault-live>\n"
            "       ekf_chapter fault-live <a|b|c-near|c-far|stale|rate|serialize|gps-native|gps-ser|all>\n");
}

static int cmd_layout(void) {
    Pose2D pose;
    LidarFrame lidar;
    GpsData gps;
    uint8_t buf[128];
    size_t pose_len = 0, lidar_len = 0, gps_len = 0;

    memset(&pose, 0, sizeof(pose));
    memset(&lidar, 0, sizeof(lidar));
    memset(&gps, 0, sizeof(gps));
    pose.converged = true;
    Pose2D_serialize(&pose, buf, &pose_len);
    LidarFrame_serialize(&lidar, buf, &lidar_len);
    GpsData_serialize(&gps, buf, &gps_len);

    printf("sizeof(EkfFusion)=%zu\n", sizeof(EkfFusion));
    printf("offsetof(EkfFusion,x)=%zu\n", offsetof(EkfFusion, x));
    printf("offsetof(EkfFusion,P)=%zu\n", offsetof(EkfFusion, P));
    printf("offsetof(EkfFusion,Q)=%zu\n", offsetof(EkfFusion, Q));
    printf("offsetof(EkfFusion,dt)=%zu\n", offsetof(EkfFusion, dt));
    printf("offsetof(EkfFusion,predict_count)=%zu\n", offsetof(EkfFusion, predict_count));
    printf("offsetof(EkfFusion,update_count)=%zu\n", offsetof(EkfFusion, update_count));
    printf("offsetof(EkfFusion,last_innovation)=%zu\n", offsetof(EkfFusion, last_innovation));
    printf("offsetof(EkfFusion,diverged)=%zu\n", offsetof(EkfFusion, diverged));
    printf("offsetof(EkfFusion,chi2_fail_count)=%zu\n", offsetof(EkfFusion, chi2_fail_count));
    printf("offsetof(EkfFusion,gated_count)=%zu\n", offsetof(EkfFusion, gated_count));
    printf("sizeof(EkfSlam)=%zu\n", sizeof(EkfSlam));
    printf("offsetof(EkfSlam,x)=%zu\n", offsetof(EkfSlam, x));
    printf("offsetof(EkfSlam,P)=%zu\n", offsetof(EkfSlam, P));
    printf("offsetof(EkfSlam,last_time_us)=%zu\n", offsetof(EkfSlam, last_time_us));
    printf("offsetof(EkfSlam,initialized)=%zu\n", offsetof(EkfSlam, initialized));
    printf("offsetof(EkfSlam,process_noise)=%zu\n", offsetof(EkfSlam, process_noise));
    printf("offsetof(EkfSlam,measurement_noise)=%zu\n", offsetof(EkfSlam, measurement_noise));
    printf("sizeof(EkfState)=%zu\n", sizeof(EkfState));
    printf("sizeof(Pose2D)=%zu\n", sizeof(Pose2D));
    printf("Pose2D_serialize_len=%zu\n", pose_len);
    printf("offsetof(Pose2D,converged)=%zu\n", offsetof(Pose2D, converged));
    printf("offsetof(Pose2D,source)=%zu\n", offsetof(Pose2D, source));
    printf("sizeof(LidarFrame)=%zu\n", sizeof(LidarFrame));
    printf("LidarFrame_serialize_len=%zu\n", lidar_len);
    printf("sizeof(GpsData)=%zu\n", sizeof(GpsData));
    printf("GpsData_serialize_len=%zu\n", gps_len);
    printf("EKF_STATE_DIM_fusion_header=5\n");
    return 0;
}

static void ctrv_step(double* x, double* y, double* h, double v, double yr, double dt) {
    *x += v * cos(*h) * dt;
    *y += v * sin(*h) * dt;
    *h += yr * dt;
    while (*h > M_PI) *h -= 2.0 * M_PI;
    while (*h < -M_PI) *h += 2.0 * M_PI;
}

static int cmd_demo(void) {
    const double dt = 0.05;
    const double v = 8.0;
    const double yr = 0.2;
    const int steps = 40;
    double tx = 0.0, ty = 0.0, th = 0.0;
    double x0[5] = {0.0, 0.0, v, 0.0, yr};
    EkfFusion ekf;
    EkfSlam slam;
    uint64_t t_us = 1000000ULL;

    ekf_fusion_init(&ekf, dt, x0);
    ekf_slam_init(&slam, 0.0f, 0.0f, 0.0f);

    printf("demo fusion_dt=%.2f slam_dt_us=50000 steps=%d v=%.1f yaw_rate=%.1f\n",
           dt, steps, v, yr);
    printf("step fusion_x fusion_y fusion_v fusion_hdg_deg fusion_yr fusion_covxx "
           "slam_x slam_y slam_hdg_deg truth_x truth_y\n");

    for (int i = 0; i <= steps; i++) {
        double fx, fy, fv, fh, fyr, diag[5];
        float sx, sy, sh, cxx, cyy, chh;
        if (i > 0) {
            ctrv_step(&tx, &ty, &th, v, yr, dt);
            ekf_fusion_predict(&ekf);
            ekf_fusion_update_lidar(&ekf, tx, ty, NULL);
            ekf_fusion_update_gps(&ekf, v, th, NULL);
            t_us += 50000ULL;
            ekf_slam_predict(&slam, 0.0f, (float)yr, t_us);
            ekf_slam_update(&slam, (float)tx, (float)ty, (float)th);
        }
        ekf_fusion_get_state(&ekf, &fx, &fy, &fv, &fh, &fyr);
        ekf_fusion_get_covariance_diag(&ekf, diag);
        ekf_slam_get_pose(&slam, &sx, &sy, &sh, &cxx, &cyy, &chh);
        if (i == 0 || i == 1 || i == 10 || i == 40) {
            printf("%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
                   i, fx, fy, fv, fh * 180.0 / M_PI, fyr, diag[0],
                   (double)sx, (double)sy, (double)sh * 180.0 / M_PI, tx, ty);
        }
    }
    printf("fusion_predict_count=%d fusion_update_count=%d slam_initialized=%d\n",
           ekf.predict_count, ekf.update_count, slam.initialized ? 1 : 0);
    return 0;
}

static double ns_per_call(void (*fn)(void*), void* ctx, int n) {
    uint64_t t0, t1;
    for (int i = 0; i < 1000; i++) fn(ctx);
    t0 = mono_us();
    for (int i = 0; i < n; i++) fn(ctx);
    t1 = mono_us();
    return (double)(t1 - t0) * 1000.0 / (double)n;
}

static EkfFusion g_bench;
static void bench_predict(void* p) { (void)p; ekf_fusion_predict(&g_bench); }
static void bench_lidar(void* p) {
    (void)p;
    ekf_fusion_update_lidar(&g_bench, g_bench.x[0], g_bench.x[1], NULL);
}
static void bench_gps(void* p) {
    (void)p;
    ekf_fusion_update_gps(&g_bench, g_bench.x[2], g_bench.x[3], NULL);
}

static int cmd_bench(void) {
    double x0[5] = {0.0, 0.0, 8.0, 0.1, 0.05};
    const int n = 200000;
    ekf_fusion_init(&g_bench, 0.05, x0);
    printf("bench n=%d clock=CLOCK_MONOTONIC warmup=1000\n", n);
    printf("predict_ns_per_call=%.1f\n", ns_per_call(bench_predict, NULL, n));
    ekf_fusion_init(&g_bench, 0.05, x0);
    printf("update_lidar_ns_per_call=%.1f\n", ns_per_call(bench_lidar, NULL, n));
    ekf_fusion_init(&g_bench, 0.05, x0);
    printf("update_gps_ns_per_call=%.1f\n", ns_per_call(bench_gps, NULL, n));
    return 0;
}

static void print_gate_state(const char* tag, const EkfFusion* ekf) {
    printf("%s update_count=%d gated_count=%d chi2_fail_count=%d diverged=%d "
           "x=%.6f last_innovation=%.6f\n",
           tag, ekf->update_count, ekf->gated_count, ekf->chi2_fail_count,
           ekf->diverged, ekf->x[0], ekf->last_innovation);
}

static int cmd_fault_gate(void) {
    double x0[5] = {0.0, 0.0, 5.0, 0.0, 0.0};
    EkfFusion ekf;
    ekf_fusion_init(&ekf, 0.05, x0);
    for (int i = 0; i < 100; i++) {
        ekf_fusion_predict(&ekf);
        ekf_fusion_update_lidar(&ekf, 0.0, 0.0, NULL);
    }
    print_gate_state("warmup", &ekf);
    ekf_fusion_predict(&ekf);
    ekf_fusion_update_lidar(&ekf, 1000.0, 0.0, NULL);
    print_gate_state("after_1_outlier", &ekf);
    for (int i = 0; i < 10; i++) {
        ekf_fusion_predict(&ekf);
        ekf_fusion_update_lidar(&ekf, 1000.0, 0.0, NULL);
    }
    print_gate_state("after_11_outliers", &ekf);
    ekf_fusion_reset(&ekf, 10.0, 0.0);
    print_gate_state("after_reset_seed_10_0", &ekf);
    return 0;
}

static int cmd_fault_wrap(void) {
    double h179 = 179.0 * M_PI / 180.0;
    double h179n = -179.0 * M_PI / 180.0;
    double x0[5] = {0.0, 0.0, 5.0, h179, 0.0};
    double before, after, naive, wrapped;
    EkfFusion ekf;
    ekf_fusion_init(&ekf, 0.05, x0);
    ekf_fusion_predict(&ekf);
    before = ekf.x[3];
    ekf_fusion_update_gps(&ekf, 5.0, h179n, NULL);
    after = ekf.x[3];
    naive = (h179n - before) * 180.0 / M_PI;
    wrapped = h179n - before;
    while (wrapped > M_PI) wrapped -= 2.0 * M_PI;
    while (wrapped < -M_PI) wrapped += 2.0 * M_PI;
    printf("before_heading_deg=%.6f\n", before * 180.0 / M_PI);
    printf("meas_heading_deg=-179\n");
    printf("after_heading_deg=%.6f\n", after * 180.0 / M_PI);
    printf("naive_delta_deg=%.6f\n", naive);
    printf("wrapped_delta_deg=%.6f\n", wrapped * 180.0 / M_PI);
    printf("last_innovation=%.6f update_count=%d\n",
           ekf.last_innovation, ekf.update_count);
    return 0;
}

static uint64_t g_stamp_ts[4];
static uint32_t g_stamp_sz[4];
static uint32_t g_stamp_tid[4];
static int g_stamp_n;

static void on_stamp(const Message* msg, void* user) {
    (void)user;
    if (!msg || g_stamp_n >= 4) return;
    g_stamp_ts[g_stamp_n] = msg->timestamp_us;
    g_stamp_sz[g_stamp_n] = msg->data_size;
    g_stamp_tid[g_stamp_n] = msg->type_id;
    g_stamp_n++;
}

static int cmd_stamp_run(void) {
    MessageBus* bus;
    LidarFrame frame;
    uint64_t t0, t1;
    g_stamp_n = 0;
    bus = message_bus_create("ekf_stamp");
    if (!bus) {
        fprintf(stderr, "stamp: message_bus_create failed\n");
        return 1;
    }
    if (message_bus_subscribe(bus, "sensor/lidar", on_stamp, NULL) != 0) {
        fprintf(stderr, "stamp: subscribe failed\n");
        return 1;
    }
    memset(&frame, 0, sizeof(frame));
    frame.x = 1.0f;
    t0 = clock_now_monotonic_wall_us();
    message_bus_publish(bus, "sensor/lidar", "ekf_chapter", &frame, (uint32_t)sizeof(frame));
    usleep(50000);
    frame.x = 2.0f;
    message_bus_publish(bus, "sensor/lidar", "ekf_chapter", &frame, (uint32_t)sizeof(frame));
    usleep(20000);
    t1 = clock_now_monotonic_wall_us();
    printf("stamp_n=%d\n", g_stamp_n);
    for (int i = 0; i < g_stamp_n; i++) {
        printf("msg%d timestamp_us=%llu data_size=%u type_id=%u\n",
               i, (unsigned long long)g_stamp_ts[i], g_stamp_sz[i], g_stamp_tid[i]);
    }
    if (g_stamp_n >= 2) {
        printf("delta_us=%lld wall_span_us=%lld\n",
               (long long)(g_stamp_ts[1] - g_stamp_ts[0]),
               (long long)(t1 - t0));
    }
    printf("publisher_t0=%llu\n", (unsigned long long)t0);
    message_bus_destroy(bus);
    return g_stamp_n >= 2 ? 0 : 1;
}

typedef struct {
    double t_rel;
    double x, y, v, raw_x, cov_xx, innov;
    int diverged;
    int has_raw;
    uint64_t timestamp_us;
} Sample;

#define SAMPLE_CAP 4096

static Sample g_samples[SAMPLE_CAP];
static int g_sample_n;
static char g_first_json[1024];
static int g_have_json;
static uint64_t g_first_loc_us;
static uint64_t g_case_us;
static int g_loc_n;
static int g_dropped_old;
static pthread_mutex_t g_sample_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_loc(const Message* msg, void* user) {
    cJSON* root;
    cJSON* j;
    Sample* s;
    (void)user;
    if (!msg || msg->data_size == 0) return;
    pthread_mutex_lock(&g_sample_mu);
    /* 上一轮 bus 分发线程偶发把已盖戳的旧帧送进本轮。时间戳早于本轮 fork 的丢掉。 */
    if (g_case_us != 0 && msg->timestamp_us < g_case_us) {
        g_dropped_old++;
        pthread_mutex_unlock(&g_sample_mu);
        return;
    }
    g_loc_n++;
    if (g_sample_n >= SAMPLE_CAP) {
        pthread_mutex_unlock(&g_sample_mu);
        return;
    }
    if (!g_have_json) {
        size_t n = msg->data_size;
        if (n >= sizeof(g_first_json)) n = sizeof(g_first_json) - 1;
        memcpy(g_first_json, msg->data, n);
        g_first_json[n] = '\0';
        g_have_json = 1;
        g_first_loc_us = mono_us();
    }
    root = cJSON_Parse((const char*)msg->data);
    if (!root) {
        pthread_mutex_unlock(&g_sample_mu);
        return;
    }
    s = &g_samples[g_sample_n];
    memset(s, 0, sizeof(*s));
    s->timestamp_us = msg->timestamp_us;
    s->t_rel = (double)(mono_us() - g_first_loc_us) / 1e6;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "x")) && cJSON_IsNumber(j))
        s->x = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "y")) && cJSON_IsNumber(j))
        s->y = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "v")) && cJSON_IsNumber(j))
        s->v = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "cov_xx")) && cJSON_IsNumber(j))
        s->cov_xx = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "innovation")) && cJSON_IsNumber(j))
        s->innov = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "diverged")) && cJSON_IsBool(j))
        s->diverged = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "raw_pos_x")) && cJSON_IsNumber(j)) {
        s->raw_x = j->valuedouble;
        s->has_raw = 1;
    }
    g_sample_n++;
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_sample_mu);
}

/* fusion 退出后 POSIX 共享内存名字还在。下一轮若直接 subscribe，
 * 读到的是上一轮环里的残留，而且子进程 advertise 时会 unlink 重建，
 * 把已经映射的订阅端变成孤儿。每次拉起前先删掉名字。 */
static const char k_loc_shm[] = "/dev/shm/flow_fusion_localization_shm";

static pid_t g_child;

static void stop_child(void) {
    if (g_child <= 0) return;
    kill(g_child, SIGTERM);
    /* fusion_start 里 node_announce_self 与协程里的 subscribe 可能互锁，
     * SIGTERM 到不了 sleep 循环。最多等 3 秒，然后 SIGKILL。 */
    for (int i = 0; i < 30; i++) {
        int st = 0;
        pid_t r = waitpid(g_child, &st, WNOHANG);
        if (r != 0) {
            g_child = 0;
            return;
        }
        usleep(100000);
    }
    kill(g_child, SIGKILL);
    waitpid(g_child, NULL, 0);
    g_child = 0;
}

static int publish_lidar(Transport* t, float x, float y) {
    LidarFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x = x;
    frame.y = y;
    frame.point_count = 1;
    return transport_publish(t, "sensor/lidar", &frame, (uint32_t)sizeof(frame));
}

static int publish_pose(Transport* t, float x, float y, float cov, int use_serialize) {
    Pose2D pose;
    uint8_t buf[64];
    size_t len = 0;
    memset(&pose, 0, sizeof(pose));
    pose.x = x;
    pose.y = y;
    pose.cov_xx = cov;
    pose.cov_yy = cov;
    pose.cov_hh = 0.01f;
    pose.converged = true;
    pose.source = 2;
    if (!use_serialize) {
        return transport_publish(t, "sensor/pose", &pose, (uint32_t)sizeof(pose));
    }
    if (Pose2D_serialize(&pose, buf, &len) != 0) return -1;
    return transport_publish(t, "sensor/pose", buf, (uint32_t)len);
}

static int publish_gps(Transport* t, float speed, float heading_deg, int use_serialize) {
    GpsData gps;
    uint8_t buf[64];
    size_t len = 0;
    memset(&gps, 0, sizeof(gps));
    gps.speed_mps = speed;
    gps.heading_deg = heading_deg;
    gps.latitude = 39.9;
    gps.longitude = 116.4;
    if (!use_serialize) {
        return transport_publish(t, "sensor/gps", &gps, (uint32_t)sizeof(gps));
    }
    if (GpsData_serialize(&gps, buf, &len) != 0) return -1;
    return transport_publish(t, "sensor/gps", buf, (uint32_t)len);
}

static void print_sample(const char* tag, int idx) {
    const Sample* s;
    if (idx < 0 || idx >= g_sample_n) return;
    s = &g_samples[idx];
    printf("%s idx=%d t_rel=%.3f x=%.6f y=%.6f v=%.6f raw_x=%.6f has_raw=%d "
           "cov_xx=%.6f innovation=%.6f diverged=%d timestamp_us=%llu\n",
           tag, idx, s->t_rel, s->x, s->y, s->v,
           s->has_raw ? s->raw_x : 0.0, s->has_raw, s->cov_xx, s->innov, s->diverged,
           (unsigned long long)s->timestamp_us);
}

static int run_case(const char* name) {
    Transport* transport = NULL;
    MessageBus* bus = NULL;
    DiscoveryManager* discovery = NULL;
    int use_serialize = 0;
    int use_gps_serialize = 0;
    int send_pose = 1;
    int send_lidar = 1;
    int send_gps = 0;
    int lidar_hz = 20;
    int pose_hz = 20;
    int gps_hz = 10;
    float lidar_x = 80.0f;
    float pose_x = 50.0f;
    float cov = 0.2f;
    double pose_off_after = 1e9;
    double lidar_off_after = 1e9;
    double cov_hi_after = 1e9;
    double run_after_loc = 2.5;
    float cov_hi = 60.0f;
    int lidar_pub = 0, pose_pub = 0, gps_pub = 0;
    uint64_t t_start, next_lidar = 0, next_pose = 0, next_gps = 0;
    int subscribed = 0;
    int rc = 0;
    char errpath[256];

    g_sample_n = 0;
    g_have_json = 0;
    g_loc_n = 0;
    g_dropped_old = 0;
    g_case_us = 0;
    g_first_loc_us = 0;
    g_first_json[0] = '\0';

    if (strcmp(name, "a") == 0) {
        run_after_loc = 1.5;
    } else if (strcmp(name, "b") == 0) {
        cov = 60.0f;
        cov_hi = 60.0f;
        run_after_loc = 1.5;
    } else if (strcmp(name, "c-near") == 0) {
        lidar_x = 100.0f;
        pose_x = 99.7f;
        pose_off_after = 1.2;
        run_after_loc = 2.4;
    } else if (strcmp(name, "c-far") == 0) {
        lidar_x = 103.0f;
        pose_x = 100.0f;
        pose_off_after = 1.6;
        run_after_loc = 3.2;
    } else if (strcmp(name, "stale") == 0) {
        lidar_x = 100.0f;
        pose_x = 40.0f;
        lidar_off_after = 1.2;
        run_after_loc = 2.6;
    } else if (strcmp(name, "rate") == 0) {
        send_pose = 0;
        send_gps = 1;
        lidar_x = 20.0f;
        run_after_loc = 2.0;
    } else if (strcmp(name, "serialize") == 0) {
        use_serialize = 1;
        lidar_x = 80.0f;
        pose_x = 50.0f;
        run_after_loc = 1.5;
    } else if (strcmp(name, "gps-native") == 0) {
        send_pose = 0;
        send_gps = 1;
        lidar_x = 10.0f;
        run_after_loc = 1.5;
    } else if (strcmp(name, "gps-ser") == 0) {
        send_pose = 0;
        send_gps = 1;
        use_gps_serialize = 1;
        lidar_x = 10.0f;
        run_after_loc = 1.5;
    } else {
        fprintf(stderr, "unknown case %s\n", name);
        return 2;
    }

    mkdir("/tmp/ekf_chapter_logs", 0755);
    snprintf(errpath, sizeof(errpath), "/tmp/ekf_chapter_logs/host_%s.err", name);
    setenv("FLOW_LOG_DIR", "/tmp/ekf_chapter_logs", 1);

    log_init(LOG_WARN, "/tmp/ekf_chapter_logs/publisher.log");
    adas_msgs_register_all();
    bus = message_bus_create("ekf_chapter");
    discovery = discovery_create("ekf_chapter", CAP_PUBLISHER | CAP_SUBSCRIBER);
    if (!bus || !discovery) {
        fprintf(stderr, "case %s: bus/discovery create failed\n", name);
        rc = 1;
        goto done;
    }
    discovery_start(discovery);
    transport = transport_create(bus, discovery, TRANSPORT_IPC);
    if (!transport || transport_start(transport) != 0) {
        fprintf(stderr, "case %s: transport start failed\n", name);
        rc = 1;
        goto done;
    }
    /* 三条输入都先以发布者身份打开。ipc_channel_open 会 unlink 再建，
     * 清掉上一轮留在环里的帧。否则本轮 fusion 作为新订阅者会把旧窗口重放一遍
     * （例如没发 GPS 的用例读到上一轮的 speed）。不发的话题保持空环。 */
    if (transport_advertise(transport, "sensor/lidar", LIDARFRAME_TYPE_ID) != 0 ||
        transport_advertise(transport, "sensor/pose", POSE2D_TYPE_ID) != 0 ||
        transport_advertise(transport, "sensor/gps", GPSDATA_TYPE_ID) != 0) {
        fprintf(stderr, "case %s: advertise failed\n", name);
        rc = 1;
        goto done;
    }
    if (unlink(k_loc_shm) != 0 && errno != ENOENT) {
        fprintf(stderr, "case %s: unlink %s: %s\n", name, k_loc_shm, strerror(errno));
    }

    g_child = fork();
    if (g_child < 0) {
        fprintf(stderr, "case %s: fork failed\n", name);
        rc = 1;
        goto done;
    }
    if (g_child == 0) {
        int fd = open(errpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execl("./build/bin/flow_node_host", "flow_node_host",
              "config/pipeline.json", "fusion", "20", (char*)NULL);
        _exit(127);
    }

    t_start = mono_us();
    g_case_us = t_start;
    while (1) {
        uint64_t now = mono_us();
        int have_json;
        uint64_t first_loc;
        pthread_mutex_lock(&g_sample_mu);
        have_json = g_have_json;
        first_loc = g_first_loc_us;
        pthread_mutex_unlock(&g_sample_mu);
        double since_loc = have_json ? (double)(now - first_loc) / 1e6 : -1.0;
        int pose_on = send_pose && (since_loc < 0.0 || since_loc < pose_off_after);
        int lidar_on = send_lidar && (since_loc < 0.0 || since_loc < lidar_off_after);
        float cov_now = cov;
        if (since_loc >= cov_hi_after) cov_now = cov_hi;

        if (lidar_on && now >= next_lidar) {
            publish_lidar(transport, lidar_x, 0.0f);
            lidar_pub++;
            next_lidar = now + (uint64_t)(1000000 / lidar_hz);
        }
        if (pose_on && now >= next_pose) {
            publish_pose(transport, pose_x, 0.0f, cov_now, use_serialize);
            pose_pub++;
            next_pose = now + (uint64_t)(1000000 / pose_hz);
        }
        if (send_gps && now >= next_gps) {
            publish_gps(transport, strcmp(name, "rate") == 0 ? 5.0f : 12.0f,
                        0.0f, use_gps_serialize);
            gps_pub++;
            next_gps = now + (uint64_t)(1000000 / gps_hz);
        }

        if (!subscribed && access(k_loc_shm, F_OK) == 0) {
            if (transport_subscribe(transport, "fusion/localization", on_loc, NULL) != 0) {
                fprintf(stderr, "case %s: subscribe localization failed\n", name);
                rc = 1;
                break;
            }
            subscribed = 1;
        }
        if (have_json && since_loc >= run_after_loc) break;
        if (!have_json && (now - t_start) > 12000000ULL) break;
        if (g_child > 0) {
            int st = 0;
            pid_t r = waitpid(g_child, &st, WNOHANG);
            if (r != 0) {
                g_child = 0;
                break;
            }
        }
        usleep(2000);
    }

    printf("case=%s wire=%s lidar_x=%.3f pose_x=%.3f\n",
           name, use_serialize ? "serialize" : "native", lidar_x, pose_x);
    pthread_mutex_lock(&g_sample_mu);
    int raw_mismatch = 0;
    for (int i = 0; i < g_sample_n; i++) {
        if (!g_samples[i].has_raw) continue;
        if (fabs(g_samples[i].raw_x - (double)lidar_x) > 1.0) raw_mismatch++;
    }
    printf("lidar_pub=%d pose_pub=%d gps_pub=%d loc_n=%d sample_n=%d subscribed=%d elapsed_s=%.3f dropped_old=%d raw_mismatch=%d\n",
           lidar_pub, pose_pub, gps_pub, g_loc_n, g_sample_n, subscribed,
           (double)(mono_us() - t_start) / 1e6, g_dropped_old, raw_mismatch);
    if (g_sample_n > 0) {
        double span = g_samples[g_sample_n - 1].t_rel - g_samples[0].t_rel;
        printf("loc_span_s=%.3f loc_hz=%.3f lidar_hz_nominal=%d pose_hz_nominal=%d gps_hz_nominal=%d\n",
               span, span > 0.0 ? (double)(g_sample_n - 1) / span : 0.0,
               lidar_hz, send_pose ? pose_hz : 0, send_gps ? gps_hz : 0);
        print_sample("first", 0);
        if (g_sample_n > 2) print_sample("mid", g_sample_n / 2);
        print_sample("last", g_sample_n - 1);
        printf("raw_json_first=%s\n", g_first_json);
    } else {
        printf("no_localization\n");
        rc = 1;
    }
    pthread_mutex_unlock(&g_sample_mu);
    fflush(stdout);

done:
    stop_child();
    if (transport) transport_destroy(transport);
    if (discovery) {
        discovery_stop(discovery);
        discovery_destroy(discovery);
    }
    if (bus) message_bus_destroy(bus);
    return rc;
}

static int cmd_fault_live(int argc, char** argv) {
    const char* which;
    if (argc < 3) {
        usage();
        return 2;
    }
    which = argv[2];
    if (strcmp(which, "all") == 0) {
        const char* cases[] = {
            "a", "b", "c-near", "c-far", "stale", "rate", "serialize",
            "gps-native", "gps-ser"
        };
        int rc = 0;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            printf("----\n");
            fflush(stdout);
            if (run_case(cases[i]) != 0) {
                fprintf(stderr, "retry case %s\n", cases[i]);
                printf("----\n");
                fflush(stdout);
                if (run_case(cases[i]) != 0) rc = 1;
            }
        }
        return rc;
    }
    return run_case(which);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "layout") == 0) return cmd_layout();
    if (strcmp(argv[1], "demo") == 0) return cmd_demo();
    if (strcmp(argv[1], "bench") == 0) return cmd_bench();
    if (strcmp(argv[1], "fault-gate") == 0) return cmd_fault_gate();
    if (strcmp(argv[1], "fault-wrap") == 0) return cmd_fault_wrap();
    if (strcmp(argv[1], "stamp") == 0) return cmd_stamp_run();
    if (strcmp(argv[1], "fault-live") == 0) return cmd_fault_live(argc, argv);
    usage();
    return 2;
}
