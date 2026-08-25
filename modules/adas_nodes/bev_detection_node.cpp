/**
 * bev_detection_node.cpp — BEV 多模态感知检测节点 (FlowCoro 协程版, 影子模式)
 *
 * 架构升级 Phase 4 的主节点：把「DBSCAN + 最近邻」目标级感知，替换/并行为
 * 「BEV 检测头」数据驱动感知。整条链路：
 *
 *   vehicle/state (真值阱物 oxN/oyN/...) + road/geometry (车道数/宽)
 *        │  下落 ego_x/y/heading + 阱物数组                 
 *        ▼
 *   bev_pre_rasterize  →  NCHW 特征图 [1,C,H,W]（占据/速度/类型/存在度）
 *        │
 *        ▼
 *   bev_onnx_backend_forward  →  模型输出张量（heatmap / 回归…，多入多出，保形）
 *        │
 *        ▼
 *   解码 → BevPostDet[]   （真 head 网络的输出语义由具体模型决定，
 *                           此处提供 heatmap 峰值解码占位 + 无模型回退真值直通）
 *        │
 *        ▼
 *   bev_post_to_obstacle_list  →  ObstacleList（老协议，lane_id 公式与
 *                                perception_node 一致）→ 发布 bev/obstacles（shadow）
 *
 * 设计要点（对齐 CLAUDE.md 影子模式纪律）：
 *   1. 影子模式：发布到独立 topic bev/obstacles（非 perception/obstacles），
 *      不接入 control/safety 链路。安全永远由原 perception → planning → control →
 *      safety_control 兜底。模型哪怕随机权重/未训练，也绝不干扰真实控制。
 *   2. 零重型依赖回退：model_path 指向 .onnx 且编译了 HAVE_ONNXRUNTIME 时加载
 *      BEV 检测头；否则走「真值直通」基线（把 vehicle/state 的阱物当作"完美
 *      检测"直接转换成 ObstacleList）。这样．链路始终可运行，模型 drop-in 后
 *      只改「解码」一步。
 *   3. 多模态扩展点：bev_onnx_backend 支持 8 路输入/8 路输出、任意 rank。当前
 *      只喂 1 路（LiDAR 真值 NCHW），相机/IMU 分支后续按 bev_pre 同约定追加。
 *   4. OTA 热重载：接收 model_ota/active 的 "reload" 信号，加锁原子替换模型。
 *
 * 采用 CoroutineTask（线程池 resume）：前向 + 解码为重计算，同步 resume 会阻塞
 * 消息总线分发线程导致 drops，故沿用 inference_node 的线程池 resume 模式。
 * flowcoro 为 header-only（INTERFACE），只需 FLOWCORO_INTEGRATION + -fcoroutines。
 */

#include "node_plugin.h"
#include "fp_env.h"          /* FTZ/DAZ 防 denormal → strtod 断言崩溃 */
#include "state_machine.h"
#include "adas_msgs_gen.h"
#include "coroutine_task.h"
#include "clock_service.h"
#include "bev_onnx_backend.h"
#include "bev_pre.h"
#include "bev_post.h"
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL
#include "logger.h"
#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

namespace {

/* ── 节点本地状态 ───────────────────────────────────────────── */

struct BevDetectionContext {
    Transport*        transport{nullptr};
    DiscoveryManager* discovery{nullptr};
    Scheduler*        scheduler{nullptr};

    ReflectiveStateMachine sm{};

    /* BEV 检测头（可选）。未加载/未编译 ORT 时 loaded=0 → 真值直通回退。 */
    BevOnnxBackend  onnx{};
    pthread_mutex_t model_mutex{};
    char    model_path[256]{};

    int     enable_shadow{1};   /* 1=发布 bev/obstacles shadow 输出 */
    double  cfg_frequency_hz{10.0};

    /* OTA 热重载：model_ota/active 收到 "reload" 信号时置 1 */
    volatile int reload_flag{0};

    /* ego（来自 vehicle/state：x/y/hdg/spd） */
    double  ego_x{0}, ego_y{0}, ego_heading{0}, ego_speed{0};
    volatile int has_ego{0};

    /* 阱物真值（来自 vehicle/state：oxN/oyN/ovN/ovyN/otN/olN/owN） */
    BevPreObs obs[128]{};
    int     n_obs{0};

    /* 车道（来自 road/geometry） */
    int     lane_count{2};
    double  lane_width{3.5};
    volatile int has_road_geometry{0};

    /* 前处理配置 + 特征缓冲 */
    BevPreConfig precfg{};
    float*  feat{nullptr};          /* channels*h*w，init 时按 precfg 分配 */
    size_t  feat_elems{0};

    /* 解码目标（复用，避免每帧 realloc） */
    BevPostDet dets[BEV_POST_MAX_DET]{};
    int     n_dets{0};

    /* shadow 输出 ObstacleList（固定 4368B） */
    ObstacleList out_list{};

    uint32_t frame_id{0};
    int     infer_count{0};
    int     reload_count{0};

    /* TaskBase 包装器 */
    struct bev_detection_Wrapper* task_wrapper{nullptr};
};

BevDetectionContext g;

/* ── 订阅回调 ────────────────────────────────────────────────── */

static void on_vehicle_state(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* j;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "x")) && cJSON_IsNumber(j))
        g.ego_x = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "y")) && cJSON_IsNumber(j))
        g.ego_y = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "hdg")) && cJSON_IsNumber(j))
        g.ego_heading = j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "spd")) && cJSON_IsNumber(j))
        g.ego_speed = j->valuedouble;

    /* 阱物真值（字段名与 perception_node 完全一致） */
    cJSON* n_obs_j = cJSON_GetObjectItemCaseSensitive(root, "n_obs");
    int no = cJSON_IsNumber(n_obs_j) ? (int)n_obs_j->valuedouble : 0;
    if (no > BEV_POST_MAX_DET) no = BEV_POST_MAX_DET;
    for (int i = 0; i < no; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ox%d", i);
        cJSON* jx = cJSON_GetObjectItemCaseSensitive(root, key);
        snprintf(key, sizeof(key), "oy%d", i);
        cJSON* jy = cJSON_GetObjectItemCaseSensitive(root, key);
        snprintf(key, sizeof(key), "ov%d", i);
        cJSON* jvx = cJSON_GetObjectItemCaseSensitive(root, key);
        snprintf(key, sizeof(key), "ovy%d", i);
        cJSON* jvy = cJSON_GetObjectItemCaseSensitive(root, key);
        snprintf(key, sizeof(key), "ot%d", i);
        cJSON* jt = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(jx) && cJSON_IsNumber(jy)) {
            g.obs[i].x  = jx->valuedouble;
            g.obs[i].y  = jy->valuedouble;
            g.obs[i].vx = cJSON_IsNumber(jvx) ? jvx->valuedouble : 0.0;
            g.obs[i].vy = cJSON_IsNumber(jvy) ? jvy->valuedouble : 0.0;
            g.obs[i].type = BEV_OBJ_VEHICLE;  /* 默认按车 */
            if (cJSON_IsString(jt) && jt->valuestring) {
                const char* t = jt->valuestring;
                if (strcmp(t, "pedestrian") == 0)   g.obs[i].type = BEV_OBJ_PEDESTRIAN;
                else if (strcmp(t, "cyclist") == 0) g.obs[i].type = BEV_OBJ_CYCLIST;
                else if (strcmp(t, "construction") == 0) g.obs[i].type = BEV_OBJ_CONSTRUCTION;
            }
        } else {
            g.obs[i].x = g.obs[i].y = 0.0;
        }
    }
    g.n_obs = no;
    g.has_ego = 1;
    cJSON_Delete(root);
}

static void on_road_geometry(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (!root) return;
    cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "lane_count");
    if (cJSON_IsNumber(j)) g.lane_count = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(root, "lane_width");
    if (cJSON_IsNumber(j)) g.lane_width = j->valuedouble;
    g.has_road_geometry = 1;
    cJSON_Delete(root);
}

static void on_model_ota_active(const Message* msg, void* user_data) {
    (void)user_data;
    if (!msg) return;
    cJSON* root = cJSON_Parse((const char*)msg->data);
    if (root) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "reload");
        if (cJSON_IsBool(j) && cJSON_IsTrue(j))
            g.reload_flag = 1;
        cJSON_Delete(root);
    }
}

/* ── 解码：模型输出 → BevPostDet[] ───────────────────────────── */

/*
 * 真头 BEV 网络（如 CenterPoint / PointPillars / TransFusion）的输出语义是
 * 模型相关的（heatmap + densify/regression 头各不相同），无法在本仓库无模型
 * 前提下硬编码。这里提供两层解码，接口都是「填 g.dets / g.n_dets」：
 *
 *   decode_heatmap()  模型已加载：把 model 首个输出当作 [1,1,H,W] 占据热图，
 *                    扫描峰值 → 每个峰值用最近爪物真值补速度/类型/尺寸（模型
 *                    只给定位，属性由真值挂靠，作为可运行占位；换真模型后
 *                    重写此处即可，下游 ObstacleList 契约不变）。
 *   decode_passthrough() 无模型：真值直通（"完美检测"基线），同时验证了
 *                    bev_pre→bev_post 全链路在无 ORT 情况下也自洽。
 */
static void decode_passthrough(void) {
    const double ch = cos(g.ego_heading), sh = sin(g.ego_heading);
    int n = 0;
    for (int i = 0; i < g.n_obs; i++) {
        const BevPreObs* o = &g.obs[i];
        double dx = o->x - g.ego_x, dy = o->y - g.ego_y;
        /* 车体系：x 前 / y 左（世界→车体旋转，与 bev_pre 一致） */
        double xb =  dx * ch + dy * sh;
        double yb = -dx * sh + dy * ch;
        /* 剪掉图外的（BEV 只在覆盖范围内"看得见"） */
        if (fabs(xb) > g.precfg.range_x || fabs(yb) > g.precfg.range_y_half) continue;
        BevPostDet* d = &g.dets[n];
        d->x = (float)xb;
        d->y = (float)yb;
        d->vx = (float)(o->vx * ch + o->vy * sh);
        d->vy = (float)(-o->vx * sh + o->vy * ch);
        d->width  = (o->type == BEV_OBJ_VEHICLE) ? 2.0f : 0.8f;
        d->length = (o->type == BEV_OBJ_VEHICLE) ? 4.6f : 1.8f;
        d->type = o->type;
        d->confidence = 0.99f;
        d->world_y = (float)(g.ego_y + xb * sh + yb * ch);
        n++;
    }
    g.n_dets = n;
}

/* 模型输出热图峰值解码。out_seq[0] 视为 [1,1,H,W] 占据热图（H/W 与 precfg 对齐）。 */
static void decode_heatmap(const BevOnnxBackend* b, float** out_seq) {
    (void)b;  /* 解码仅依赖 precfg/obs 与输出张量，后端句柄暂未使用 */
    const int H = g.precfg.h, W = g.precfg.w;
    const float* hm = out_seq[0];   /* 展平 NCHW，取 [0,0,:,:] → 索引 (r*W+c) */
    const double ch = cos(g.ego_heading), sh = sin(g.ego_heading);
    const double res_x = (2.0 * g.precfg.range_x) / H;
    const double res_y = (2.0 * g.precfg.range_y_half) / W;

    int n = 0;
    for (int r = 0; r < H && n < BEV_POST_MAX_DET; r++) {
        for (int c = 0; c < W && n < BEV_POST_MAX_DET; c++) {
            float v = hm[r * W + c];
            /* 局部极大 + 阈值：高于 4-邻域且 >0.5 */
            if (v < 0.5f) continue;
            if (r > 0 && hm[(r - 1) * W + c] > v) continue;
            if (r < H - 1 && hm[(r + 1) * W + c] > v) continue;
            if (c > 0 && hm[r * W + c - 1] > v) continue;
            if (c < W - 1 && hm[r * W + c + 1] > v) continue;
            /* 热图 cell 中心 → 车体系 */
            double xb = -g.precfg.range_x + ((double)c + 0.5) * res_x;
            double yb =  g.precfg.range_y_half - ((double)r + 0.5) * res_y;

            BevPostDet* d = &g.dets[n];
            d->x = (float)xb;
            d->y = (float)yb;
            /* 属性（速度/类型/尺寸）挂靠最近爪物真值 —— 占位，真模型自带回归头后去掉 */
            int bi = -1; double bd = 1e9;
            for (int i = 0; i < g.n_obs; i++) {
                double dx = g.obs[i].x - g.ego_x, dy = g.obs[i].y - g.ego_y;
                double obx = dx * ch + dy * sh, oby = -dx * sh + dy * ch;
                double dd = (obx - xb) * (obx - xb) + (oby - yb) * (oby - yb);
                if (dd < bd) { bd = dd; bi = i; }
            }
            if (bi >= 0 && bd < 4.0 * res_x * res_y) {
                const BevPreObs* o = &g.obs[bi];
                d->vx = (float)(o->vx * ch + o->vy * sh);
                d->vy = (float)(-o->vx * sh + o->vy * ch);
                d->type = o->type;
                d->width  = (o->type == BEV_OBJ_VEHICLE) ? 2.0f : 0.8f;
                d->length = (o->type == BEV_OBJ_VEHICLE) ? 4.6f : 1.8f;
            } else {
                d->vx = 0.f; d->vy = 0.f; d->type = BEV_OBJ_VEHICLE;
                d->width = 2.0f; d->length = 4.6f;
            }
            d->confidence = v;
            d->world_y = (float)(g.ego_y + xb * sh + yb * ch);
            n++;
        }
    }
    g.n_dets = n;
}

/* ── 协程任务 ────────────────────────────────────────────────── */

class BevDetectionTask : public CoroutineTask {
public:
    BevDetectionTask(MessageBus* bus) : CoroutineTask(bus) {}

    void set_params(Transport* transport, double frequency_hz) {
        transport_ = transport;
        period_us_ = (long)(1e6 / (frequency_hz > 0.0 ? frequency_hz : 10.0));
    }

protected:
    Task run() override {
        fp_env_init();  /* FTZ/DAZ：防 denormal 进 JSON 触发 glibc strtod 断言 */

        while (!should_stop()) {
            co_await sleep_us(period_us_);
            if (should_stop()) break;

            /* OTA 热重载：model_ota_node 发来 reload 信号时原子替换模型 */
            if (g.reload_flag) {
                g.reload_flag = 0;
                pthread_mutex_lock(&g.model_mutex);
                BevOnnxBackend nb{};
                if (bev_onnx_backend_load(&nb, g.model_path) == 0) {
                    bev_onnx_backend_free(&g.onnx);
                    g.onnx = nb;
                    g.reload_count++;
                    LOG_INFO("bev_detection", "OTA hot-reload #%d ONNX from %s",
                             g.reload_count, g.model_path);
                } else {
                    bev_onnx_backend_free(&nb);
                    LOG_WARN("bev_detection", "OTA ONNX hot-reload failed: %s", g.model_path);
                }
                pthread_mutex_unlock(&g.model_mutex);
            }

            if (!g.has_ego) continue;

            /* ── 1) 前处理：真值 → NCHW 特征图 ── */
            size_t n_f = bev_pre_rasterize(&g.precfg, g.ego_x, g.ego_y, g.ego_heading,
                                           g.obs, g.n_obs, g.feat);
            if (n_f != g.feat_elems) {
                LOG_ERROR("bev_detection", "rasterize elems mismatch (%zu != %zu)", n_f, g.feat_elems);
                continue;
            }

            /* ── 2) 模型前向（若加载）→ 解码；否则真值直通 ── */
            int loaded = 0;
            pthread_mutex_lock(&g.model_mutex);
            loaded = g.onnx.loaded;
            pthread_mutex_unlock(&g.model_mutex);

            if (g.enable_shadow) {
                int use_model = loaded && (g.onnx.n_out >= 1);
                if (use_model) {
                    float* in_bufs[BEV_ONNX_MAX_IN]  = { g.feat };
                    const int64_t in_shape[4] = { 1, g.precfg.channels, g.precfg.h, g.precfg.w };
                    const int64_t* in_shp[BEV_ONNX_MAX_IN] = { in_shape };
                    int in_rk[BEV_ONNX_MAX_IN] = { 4 };

                    /* 输出缓冲：transient，仅解码后用。512KB 进 static 段避免爆栈 */
                    static const int MAX_OUT_FLOATS = 128 * 128 * 8;
                    static float out_buf[MAX_OUT_FLOATS];
                    float* out_seq[BEV_ONNX_MAX_OUT];
                    for (int i = 0; i < BEV_ONNX_MAX_OUT; i++) out_seq[i] = out_buf;

                    int n_out = bev_onnx_backend_forward(&g.onnx, in_bufs, in_shp, in_rk, out_seq);
                    if (n_out >= 1) {
                        decode_heatmap(&g.onnx, out_seq);
                    } else {
                        decode_passthrough();  /* 前向失败 → 安全回退真值基线 */
                    }
                } else {
                    decode_passthrough();
                }

                /* ── 3) 后处理：解码目标 → ObstacleList（老协议）── */
                ObstacleList list;
                memset(&list, 0, sizeof(list));
                bev_post_to_obstacle_list(&list, g.dets, g.n_dets, g.frame_id,
                                          g.lane_count, g.lane_width);
                list.timestamp_us = clock_now_realtime_us();

                /* 影子发布（独立 topic，不覆盖 perception/obstacles） */
                uint8_t buf[sizeof(ObstacleList)];
                size_t  len = sizeof(buf);
                ObstacleList_serialize(&list, buf, &len);
                transport_publish(transport_, "bev/obstacles", buf, (uint32_t)len);
            }

#if 0  /* 调试用：每 25 帧打一条链路摘要 */
            if (g.infer_count % 25 == 1) {
                LOG_INFO("bev_detection",
                         "#%d model=%s n_obs=%d n_dets=%d ego=(%.1f,%.1f) h=%.2f feat=%zu",
                         g.infer_count, loaded ? "onnx" : "passthrough",
                         g.n_obs, g.n_dets, g.ego_x, g.ego_y, g.ego_heading, n_f);
            }
#endif

            g.frame_id++;
            g.infer_count++;
        }

        statem_send_event(&g.sm, SM_EVENT_STOP, NULL);
        statem_send_event(&g.sm, SM_EVENT_DONE, NULL);
    }

private:
    Transport* transport_;
    long       period_us_;
};

/* ── TaskBase 包装器（宏生成） ───────────────────────────────── */
EXPORT_COROUTINE_TASK(BevDetectionTask, bev_detection)

/* ── NodePlugin 实现 ─────────────────────────────────────────── */

static const char* s_inputs[]  = {
    "vehicle/state",
    "road/geometry",
    "model_ota/active",       /* OTA 热重载信号 */
    nullptr
};
static const char* s_outputs[] = {
    "bev/obstacles",          /* shadow（不覆盖 perception/obstacles） */
    nullptr
};

extern NodePlugin s_plugin;

static int bev_detection_init(MessageBus* bus, Transport* transport,
                              DiscoveryManager* discovery, Scheduler* scheduler,
                              const char* params_json) {
    g.transport = transport;
    g.discovery = discovery;
    g.scheduler = scheduler;

    g.reload_flag = 0;
    g.ego_x = g.ego_y = g.ego_heading = g.ego_speed = 0.0;
    g.has_ego = 0;
    g.n_obs = 0;
    g.lane_count = 2;
    g.lane_width = 3.5;
    g.has_road_geometry = 0;
    g.frame_id = 0;
    g.infer_count = 0;
    g.reload_count = 0;
    g.n_dets = 0;
    g.enable_shadow = 1;

    /* 默认参数 */
    g.cfg_frequency_hz = 10.0;
    strncpy(g.model_path, "", sizeof(g.model_path) - 1);

    if (params_json) {
        cJSON* p = cJSON_Parse(params_json);
        if (p) {
            cJSON* j;
            j = cJSON_GetObjectItemCaseSensitive(p, "frequency_hz");
            if (cJSON_IsNumber(j)) g.cfg_frequency_hz = j->valuedouble;
            j = cJSON_GetObjectItemCaseSensitive(p, "model_path");
            if (cJSON_IsString(j) && j->valuestring) {
                strncpy(g.model_path, j->valuestring, sizeof(g.model_path) - 1);
                g.model_path[sizeof(g.model_path) - 1] = '\0';
            }
            j = cJSON_GetObjectItemCaseSensitive(p, "enable");
            if (cJSON_IsBool(j)) g.enable_shadow = cJSON_IsTrue(j) ? 1 : 0;
            else if (cJSON_IsNumber(j)) g.enable_shadow = (j->valuedouble != 0.0) ? 1 : 0;
            cJSON_Delete(p);
        }
    }

    /* 前处理配置 + 特征缓冲预分配 */
    bev_pre_config_default(&g.precfg);
    g.precfg.h = 88;
    g.precfg.w = 88;
    /* 允许通过环境变量覆盖栅格尺寸（与大 BEV 模型对齐） */
    const char* env_w = getenv("BEV_GRID_W");
    const char* env_h = getenv("BEV_GRID_H");
    if (env_w && atoi(env_w) > 0) g.precfg.w  = atoi(env_w);
    if (env_h && atoi(env_h) > 0) g.precfg.h  = atoi(env_h);
    g.feat_elems = (size_t)g.precfg.channels * g.precfg.h * g.precfg.w;
    g.feat = (float*)calloc(g.feat_elems, sizeof(float));
    if (!g.feat) {
        LOG_ERROR("bev_detection", "feat alloc failed (%zu floats)", g.feat_elems);
        return -1;
    }

    pthread_mutex_init(&g.model_mutex, nullptr);

    /* 加载 .onnx 检测头（若配置且可编译）。失败/未编译 → loaded=0 → 真值直通回退 */
    if (g.model_path[0]) {
        if (bev_onnx_backend_load(&g.onnx, g.model_path) == 0) {
            LOG_INFO("bev_detection", "BEV ONNX loaded from %s (in=%d out=%d)",
                     g.model_path, g.onnx.n_in, g.onnx.n_out);
            for (int i = 0; i < g.onnx.n_in; i++)
                LOG_INFO("bev_detection", "  in[%d] rank=%d", i, (int)g.onnx.in_desc[i].rank);
        } else {
            LOG_INFO("bev_detection",
                     "no BEV model at %s (or ORT not compiled) — passthrough baseline",
                     g.model_path);
        }
    } else {
        LOG_INFO("bev_detection", "no model_path configured — passthrough baseline");
    }

    transport_subscribe(transport, "vehicle/state", on_vehicle_state, nullptr);
    transport_subscribe(transport, "road/geometry", on_road_geometry, nullptr);
    transport_subscribe(transport, "model_ota/active", on_model_ota_active, nullptr);
    if (g.enable_shadow)
        transport_advertise(transport, "bev/obstacles", 0x4BE1DEa3u);

    discovery_advertise(discovery, "vehicle/state", 0xF0ED1110u, CAP_SUBSCRIBER, 0);
    discovery_advertise(discovery, "bev/obstacles", 0x4BE1DEa3u,
                        CAP_PUBLISHER, g.cfg_frequency_hz);

    statem_init(&g.sm, nullptr, SM_STATE_INITIALIZED, "bev_detection");
    statem_send_event(&g.sm, SM_EVENT_START, nullptr);

    TaskConfig tcfg = {};
    snprintf(tcfg.name, sizeof(tcfg.name), "bev_detection");
    tcfg.priority = TASK_PRIORITY_NORMAL;
    g.task_wrapper = bev_detection_create(&tcfg, bus);
    if (!g.task_wrapper) {
        LOG_ERROR("bev_detection", "bev_detection_create failed");
        return -1;
    }
    g.task_wrapper->impl->set_params(transport, g.cfg_frequency_hz);
    s_plugin.taskbase = bev_detection_get_base(g.task_wrapper);

    LOG_INFO("bev_detection",
             "initialized (FlowCoro, shadow=%s, %.0f Hz, grid=%dx%d C=%d, %s)",
             g.enable_shadow ? "on" : "off", g.cfg_frequency_hz,
             g.precfg.w, g.precfg.h, g.precfg.channels,
             g.onnx.loaded ? "onnx loaded" : "passthrough baseline");
    return 0;
}

static int bev_detection_start(void) {
    if (!g.task_wrapper) return -1;
    int rc = node_start_managed(&s_plugin, g.scheduler);
    if (rc != 0) LOG_WARN("bev_detection", "node_start_managed failed: %d", rc);
    node_announce_self(g.transport, &s_plugin);
    return 0;
}

static void bev_detection_stop(void) {
    if (g.task_wrapper)
        bev_detection_stop(&g.task_wrapper->base);
}

static void bev_detection_cleanup(void) {
    if (g.task_wrapper) {
        bev_detection_destroy(g.task_wrapper);
        g.task_wrapper = nullptr;
    }
    s_plugin.taskbase = nullptr;
    bev_onnx_backend_free(&g.onnx);
    if (g.feat) { free(g.feat); g.feat = nullptr; }
    pthread_mutex_destroy(&g.model_mutex);
    statem_cleanup(&g.sm);
}

static int bev_detection_health(void) { return 0; }

/* ── 导出入口 ────────────────────────────────────────────────── */

NodePlugin s_plugin = {
    NODE_PLUGIN_API_VERSION,
    "bev_detection",
    "0.1.0",
    "BEV multi-modal perception detection head (shadow mode) [FlowCoro]",
    s_inputs,
    s_outputs,
    bev_detection_init,
    bev_detection_start,
    bev_detection_stop,
    bev_detection_cleanup,
    bev_detection_health,
    nullptr,  /* taskbase: 在 init() 中设置 */
};

} // namespace

extern "C" NodePlugin* node_get_plugin(void) { return &s_plugin; }