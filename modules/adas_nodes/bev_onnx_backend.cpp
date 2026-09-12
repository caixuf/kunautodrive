/**
 * bev_onnx_backend.cpp — ONNX Runtime 多模态 BEV 张量后端实现
 *
 * 编译路径（与 onnx_backend.cpp 对称）：
 *   - HAVE_ONNXRUNTIME 定义时：read 图各输入输出的形状/元素类型，建 NCHW 等任意
 *     rank 张量、多输入多输出 Session::Run、按图布局拷回各输出 float 缓冲。
 *   - 未定义时：load→-1、forward→0，等价「无 BEV 模型」。
 *
 * 与 onnx_backend.cpp 的差异（为什么同一项目要第二套后端）：
 *   - onnx_backend 面向 1×in_dim 特征→控制，rank 仅 1/2、单入单出，forward 语义
 *     对齐 tiny_mlp_forward。
 *   - 本后端面向 BEV 检测头，需保留每路张量真实形状（NCHW 特征图 / heatmap /
 *     回归输出），且天然多输入（LiDAR + 相机多模态）。两者按图驱动，互不复用，
 *     避免把「保形」语义塞进一维后端的 switch 分支里。
 *
 *  必读踩坑（2026-08-05 onnx_backend.cpp 修复的同类问题）：
 *    GetTensorTypeAndShapeInfo() 返回的 info 持有指向 GetInputTypeInfo() 临时
 *    TypeInfo 内部数据的指针，临时对象语句结束即析构 → info 悬空 → 读野内存。
 *    必须把 TypeInfo 存成具名变量保活，再取 TensorTypeAndShapeInfo，最后原地拷贝
 *    出 shape（std::vector<int64_t> 独立持有，不引用临时量）。
 */
#include "bev_onnx_backend.h"

#include <string.h>
#include <stdio.h>

#ifdef HAVE_ONNXRUNTIME

#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <memory>

/* session/env 擦除为 void*，此处按类型还原。 */
namespace {
struct BevOrtHolder {
    Ort::Env     env;
    Ort::Session session;
    std::vector<std::string> in_names;   /* 按 in_desc 索引对齐 */
    std::vector<std::string> out_names;
    BevOrtHolder(const char* path)
        : env(ORT_LOGGING_LEVEL_WARNING, "flow-bev-onnx"),
          session(nullptr) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = Ort::Session(env, path, opts);
    }
};
}  /* namespace */

/* 上限检查 + 把 shape 归入描述。rank 越界/元素类型非 float 返回非 0。 */
static int capture_desc(BevOnnxTensorDesc* d, const std::vector<int64_t>& shape,
                        int elem_type) {
    if ((int)shape.size() > BEV_ONNX_MAX_RANK) {
        fprintf(stderr, "[bev-onnx] rank %zu 超上限 %d\n",
                shape.size(), BEV_ONNX_MAX_RANK);
        return -1;
    }
    if (elem_type != BEV_ONNX_ELEM_FLOAT) {
        fprintf(stderr, "[bev-onnx] 仅支持 float32 张量 (type=%d)\n", elem_type);
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->rank  = (int32_t)shape.size();
    d->elems = 1;
    for (size_t i = 0; i < shape.size(); ++i) {
        d->shape[i] = shape[i];
        /* 动态维（<0）按 1 估，用于预分配上界；forward 由实际 shape 重算。 */
        if (shape[i] > 0) d->elems *= shape[i];
    }
    d->elem_type = elem_type;
    return 0;
}

extern "C" int bev_onnx_backend_load(BevOnnxBackend* b, const char* path) {
    if (!b || !path) return -1;
    memset(b, 0, sizeof(*b));

    try {
        BevOrtHolder* h = new BevOrtHolder(path);

        size_t n_in  = h->session.GetInputCount();
        size_t n_out = h->session.GetOutputCount();
        if (n_in > BEV_ONNX_MAX_IN || n_out > BEV_ONNX_MAX_OUT) {
            fprintf(stderr, "[bev-onnx] %s: 输入/输出路数超上限 "
                    "(in=%zu/%d out=%zu/%d)\n",
                    path, n_in, BEV_ONNX_MAX_IN, n_out, BEV_ONNX_MAX_OUT);
            delete h;
            return -1;
        }

        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < n_in; ++i) {
            Ort::AllocatedStringPtr nm = h->session.GetInputNameAllocated(i, alloc);
            h->in_names.emplace_back(nm.get());
            /* 保活临时 TypeInfo（见文件头踩坑） */
            Ort::TypeInfo tti   = h->session.GetInputTypeInfo(i);
            auto ti             = tti.GetTensorTypeAndShapeInfo();
            std::vector<int64_t> shape = ti.GetShape();
            if (capture_desc(&b->in_desc[i], shape,
                             (int)ti.GetElementType()) != 0) {
                delete h;
                return -1;
            }
        }
        for (size_t i = 0; i < n_out; ++i) {
            Ort::AllocatedStringPtr nm = h->session.GetOutputNameAllocated(i, alloc);
            h->out_names.emplace_back(nm.get());
            Ort::TypeInfo tti   = h->session.GetOutputTypeInfo(i);
            auto ti             = tti.GetTensorTypeAndShapeInfo();
            std::vector<int64_t> shape = ti.GetShape();
            if (capture_desc(&b->out_desc[i], shape,
                             (int)ti.GetElementType()) != 0) {
                delete h;
                return -1;
            }
        }

        b->session = h;
        b->env     = &h->env;
        b->n_in    = (int)n_in;
        b->n_out   = (int)n_out;
        b->loaded  = 1;
        return 0;
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[bev-onnx] load %s failed: %s\n", path, e.what());
        return -1;
    } catch (...) {
        fprintf(stderr, "[bev-onnx] load %s failed: unknown\n", path);
        return -1;
    }
}

extern "C" int bev_onnx_backend_forward(const BevOnnxBackend* b,
                                        const float* const* inputs,
                                        const int64_t* const* in_shapes,
                                        const int* in_ranks,
                                        float* const* out_seq) {
    if (!b || !b->loaded || !b->session || !inputs || !out_seq) return 0;
    BevOrtHolder* h = static_cast<BevOrtHolder*>(b->session);

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> in_tensors;
        in_tensors.reserve((size_t)b->n_in);
        std::vector<const char*> in_names;  in_names.reserve((size_t)b->n_in);
        std::vector<const char*> out_names; out_names.reserve((size_t)b->n_out);

        for (int i = 0; i < b->n_in; ++i) {
            const BevOnnxTensorDesc& d = b->in_desc[i];
            /* 用调用方 shape（若给）校验 rank、重算元素数；否则用图静态形状。 */
            int      rank = in_ranks ? in_ranks[i] : (int)d.rank;
            int64_t  elems = 1;
            std::unique_ptr<int64_t[]> shape;
            const int64_t* sptr = d.shape;
            if (in_shapes) {
                shape.reset(new int64_t[rank]);
                elems = 1;
                for (int r = 0; r < rank; ++r) {
                    shape[r] = in_shapes[i][r];
                    if (shape[r] > 0) elems *= shape[r];
                }
                sptr = shape.get();
            } else {
                for (int r = 0; r < rank; ++r)
                    if (d.shape[r] > 0) elems *= d.shape[r];
            }
            in_tensors.emplace_back(Ort::Value::CreateTensor<float>(
                mem, const_cast<float*>(inputs[i]), (size_t)elems,
                sptr, rank));
            in_names.push_back(h->in_names[i].c_str());
            out_names.push_back(h->out_names[i].c_str());
        }

        std::vector<Ort::Value> outs = h->session.Run(
            Ort::RunOptions{nullptr}, in_names.data(), in_tensors.data(),
            (size_t)b->n_in, out_names.data(), (size_t)b->n_out);

        if (outs.size() != (size_t)b->n_out) return 0;
        for (int i = 0; i < b->n_out; ++i) {
            if (!outs[i].IsTensor()) return 0;
            const float* data   = outs[i].GetTensorData<float>();
            size_t       elems  = outs[i].GetTensorTypeAndShapeInfo().GetElementCount();
            size_t       cap    = (size_t)b->out_desc[i].elems;
            size_t       ncopy  = elems < cap ? elems : cap;
            memcpy(out_seq[i], data, ncopy * sizeof(float));
        }
        return b->n_out;
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[bev-onnx] forward failed: %s\n", e.what());
        return 0;
    } catch (...) {
        return 0;
    }
}

extern "C" void bev_onnx_backend_free(BevOnnxBackend* b) {
    if (!b) return;
    if (b->session) {
        delete static_cast<BevOrtHolder*>(b->session);
    }
    memset(b, 0, sizeof(*b));
}

#else  /* !HAVE_ONNXRUNTIME —— 降级：无 BEV 后端 */

extern "C" int bev_onnx_backend_load(BevOnnxBackend* b, const char* path) {
    (void)path;
    if (b) memset(b, 0, sizeof(*b));
    return -1;
}

extern "C" int bev_onnx_backend_forward(const BevOnnxBackend* b,
                                        const float* const* inputs,
                                        const int64_t* const* in_shapes,
                                        const int* in_ranks,
                                        float* const* out_seq) {
    (void)b; (void)inputs; (void)in_shapes; (void)in_ranks; (void)out_seq;
    return 0;
}

extern "C" void bev_onnx_backend_free(BevOnnxBackend* b) {
    if (b) memset(b, 0, sizeof(*b));
}

#endif /* HAVE_ONNXRUNTIME */