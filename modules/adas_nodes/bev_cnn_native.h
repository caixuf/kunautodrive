/**
 * bev_cnn_native.h — 纯 C CenterPoint-Lite BEV 卷积检测与解码核
 *
 * 零外部重依赖（无 ONNXRuntime / TensorRT / CUDA / libtorch 动态库链接）。
 * 直接加载 models/bev_centernet_v1.bin 紧凑二进制权重执行全卷积前向推理与
 * 3x3 极大值抑制多头解码，产出完全独立的 BevPostDet[]。
 */
#ifndef BEV_CNN_NATIVE_H
#define BEV_CNN_NATIVE_H

#include <stdint.h>
#include <stddef.h>
#include "bev_pre.h"
#include "bev_post.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BEV_CNN_MAGIC 0x4E4E4342u  /* "BCNN" (0x42, 0x43, 0x4E, 0x4E) in Little-Endian */
#define BEV_CNN_MAX_LAYERS 8

typedef struct BevCnnLayer {
    uint32_t in_channels;
    uint32_t out_channels;
    uint32_t kernel_size;
    uint32_t padding;
    uint32_t stride;
    uint32_t activation; /* 0=Linear, 1=ReLU, 2=Sigmoid(ch 0..3)+Linear(ch 4..9) */
    float*   weights;    /* [out_channels, in_channels, k, k] */
    float*   bias;       /* [out_channels] */
} BevCnnLayer;

typedef struct BevCnnModel {
    int          loaded;
    uint32_t     version;
    uint32_t     n_layers;
    uint32_t     in_c, in_h, in_w;
    uint32_t     out_c, out_h, out_w;
    BevCnnLayer  layers[BEV_CNN_MAX_LAYERS];
    /* 中间计算工作缓冲 */
    float*       buf_a;
    float*       buf_b;
    size_t       buf_elems;
} BevCnnModel;

/**
 * 加载 .bin 权重文件
 * @return 0 成功，-1 失败
 */
int bev_cnn_load(BevCnnModel* model, const char* bin_path);

/**
 * 释放模型内存
 */
void bev_cnn_free(BevCnnModel* model);

/**
 * 前向推理：输入 [6, 88, 88] → 输出 [10, 88, 88]
 * @return 0 成功，-1 失败
 */
int bev_cnn_forward(const BevCnnModel* model, const float* in_feat, float* out_feat);

/**
 * 多头解码：[10, 88, 88] 特征图 → BevPostDet 目标列表
 *   - 3x3 局部极大值 NMS
 *   - 阈值过滤 (score >= 0.35)
 *   - 亚像素坐标 (dx, dy) 还原
 *   - 尺寸 (w, l) 与速度 (vx, vy) 头部直接读取
 *   - 100% 独立，绝不读取 vehicle/state 真值
 *
 * @return 解码出的目标数
 */
int bev_cnn_decode(const BevCnnModel* model,
                   const float* out_feat,
                   const BevPreConfig* precfg,
                   double ego_heading,
                   double ego_y,
                   BevPostDet* dets,
                   int max_dets);

#ifdef __cplusplus
}
#endif

#endif /* BEV_CNN_NATIVE_H */
