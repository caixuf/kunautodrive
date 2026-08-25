/**
 * bev_onnx_backend.h — ONNX Runtime 张量后端（BEV 感知·可选）· 纯接口
 *
 * 与 onnx_backend.h（一维向量后端，供 inference_node 控制环用）并存，但**互不干扰**：
 *   - onnx_backend.h  面向「特征向量 → 控制量」的 1×in_dim 前向，rank 1/2，单入单出。
 *   - 本后端面向「多模态 BEV 检测头」：多输入（LiDAR 分支 + 相机分支等）、多输出、
 *     任意 rank（NCHW 特征图 / heatmap / 回归张量），保留每路张量的真实形状（保形）。
 *
 * 本头文件不依赖 onnxruntime，任何编译单元都可 include；真正的 ORT 调用集中在
 * bev_onnx_backend.cpp 的 `#ifdef HAVE_ONNXRUNTIME` 块内。未编译 ORT 时所有函数走
 * 降级路径（load 返回 -1、forward 返回 0），行为等价「无 BEV 模型」。
 *
 * 设计要点（与 CLAUDE.md 张量契约一致）：
 *   - all_shape[i] 元素 <0 表示该维动态（-1=unknown），load 时读图推断并按需记录。
 *   - forward 以「每输入一个 {主机指针, shape 数组, rank}」驱动，填充到预分配的
 *     out_host[i] 数组；元素顺序按 ONNX 内存布局（NCHW 连续，row-major）。
 *   - in_elem_bytes/out_elem_bytes 让调用方知道每个元素占多少字节（float32=4），
 *     便于以字节搬运到 CPU/GPU 张量容器。load 时校验统一为 float32，非 float 拒绝。
 */
#ifndef FLOW_BEV_ONNX_BACKEND_H
#define FLOW_BEV_ONNX_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEV_ONNX_MAX_IN   8   /* 输入张量路数上限（多模态：LiDAR/相机/IMU...） */
#define BEV_ONNX_MAX_OUT  8   /* 输出张量路数上限（heatmap/回归/向量场...）    */
#define BEV_ONNX_MAX_RANK 8   /* 维度数上限（NCHW / 更高秩时序均可覆盖）      */

/**
 * onnx tensor 元素类型枚举（与 onnxruntime_cxx_api.h 一致，仅取本项目用到的常量）。
 * load 时按此校验；forward 目前只支持 FLOAT(1)，其余类型在 forward 返回 0（即不支持）。
 */
typedef enum {
    BEV_ONNX_ELEM_UNDEFINED = 0,
    BEV_ONNX_ELEM_FLOAT     = 1,   /* float32，唯一支持的前向类型 */
    BEV_ONNX_ELEM_INT64     = 7,   /* 某些模型分类分支用 int64 输出 */
    BEV_ONNX_ELEM_INT32     = 6,
} BevOnnxElemType;

/** 单路张量的形状与元素类型描述（load 时从模型图读出）。 */
typedef struct BevOnnxTensorDesc {
    int64_t  shape[BEV_ONNX_MAX_RANK]; /* 各维长度；<0 表示动态维 */
    int32_t  rank;                     /* 实际有效维数 1..BEV_ONNX_MAX_RANK */
    int64_t  elems;                    /* 全部静态维之积（动态维按 1 估），用于预分配 */
    int32_t  elem_type;                /* BevOnnxElemType */
} BevOnnxTensorDesc;

typedef struct BevOnnxBackend {
    void*  session;   /* Ort::Session 擦除指针，NULL=未加载 */
    void*  env;       /* Ort::Env 擦除指针 */

    int    n_in;      /* 输入路数 */
    int    n_out;     /* 输入路数 */
    BevOnnxTensorDesc in_desc[BEV_ONNX_MAX_IN];
    BevOnnxTensorDesc out_desc[BEV_ONNX_MAX_OUT];
    int    loaded;    /* 1=已加载可推理 */
} BevOnnxBackend;

/**
 * 加载 .onnx 模型。成功返回 0（填满 in/out 描述与 loaded=1）；
 * 失败——文件不存在 / 未编译 HAVE_ONNXRUNTIME / 输入输出路数超上限 /
 * 元素类型非 float32 / 存在 rank>BEV_ONNX_MAX_RANK 的张量 / 动态维导致
 * elems 无法静态估算——返回 -1 且 loaded=0。
 * 注意：允许动态批维（首维 -1），forward 时由调用方以实际 batch 提供形状。
 */
int bev_onnx_backend_load(BevOnnxBackend* b, const char* path);

/**
 * 前向。以「每输入一个 {数据指针, shape 数组, rank}」的形式喂张量，
 * 输出写入预分配数组 out_host[i]（按 out_desc[i].elems × 4 字节）。
 *
 * @param b          已加载后端
 * @param inputs    inputs_seq[i] = 第 i 个输入张量的主机数据（float32，按 shape 展平）
 * @param in_shapes inputs_seq 对应 shape（可为 NULL=用图原始静态形状）；
 *                  传入形状的 rank 必须与 in_desc[i].rank 一致
 * @param in_ranks  inputs_seq 对应 rank 数组
 * @param out_seq   输出主机缓冲数组，out_seq[i] 必须足够容纳 out_desc[i].elems 个 float
 * @return 成功返回已填充输出路数 n_out；未加载/不支持类型/形状不匹配等返回 0
 */
int bev_onnx_backend_forward(const BevOnnxBackend* b,
                             const float* const* inputs,
                             const int64_t* const* in_shapes,
                             const int* in_ranks,
                             float* const* out_seq);

/** 释放底层 session/env，loaded 置 0（幂等，可重复调用）。 */
void bev_onnx_backend_free(BevOnnxBackend* b);

#ifdef __cplusplus
}
#endif

#endif /* FLOW_BEV_ONNX_BACKEND_H */