/**
 * @file model_inference_backend.cpp
 * @brief Implementation of inference backend classes.
 */

#include "model_inference_backend.h"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

namespace {
struct OrtSessionHolder {
    Ort::Env env;
    Ort::Session session;
    OrtSessionHolder(const char* path, int num_threads)
        : env(ORT_LOGGING_LEVEL_WARNING, "flow-inference-onnx"),
          session(nullptr) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(num_threads > 0 ? num_threads : 1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = Ort::Session(env, path, opts);
    }
};
} // namespace
#endif

namespace apollo {
namespace perception {
namespace inference {

// ============================================================================
// Tensor Implementation
// ============================================================================

Tensor::Tensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data)
    : name_(name), shape_(shape) {
    if (!data.empty()) {
        data_ = data;
    } else {
        int64_t count = NumElements();
        if (count > 0) {
            data_.resize(static_cast<size_t>(count), 0.0f);
        }
    }
}

int64_t Tensor::NumElements() const {
    if (shape_.empty()) {
        return 0;
    }
    int64_t total = 1;
    for (int64_t dim : shape_) {
        total *= (dim > 0 ? dim : 1);
    }
    return total;
}

void Tensor::Reshape(const std::vector<int64_t>& new_shape) {
    shape_ = new_shape;
    int64_t count = NumElements();
    data_.resize(static_cast<size_t>(count), 0.0f);
}

void Tensor::SetData(const std::vector<float>& data) {
    data_ = data;
}

void Tensor::SetData(const float* src, size_t count) {
    if (!src || count == 0) {
        data_.clear();
        return;
    }
    data_.resize(count);
    std::memcpy(data_.data(), src, count * sizeof(float));
}

// ============================================================================
// TensorInput Implementation
// ============================================================================

void TensorInput::AddTensor(const Tensor& tensor) {
    name_map_[tensor.Name()] = tensors_.size();
    tensors_.push_back(tensor);
}

void TensorInput::AddTensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data) {
    name_map_[name] = tensors_.size();
    tensors_.emplace_back(name, shape, data);
}

const Tensor* TensorInput::GetTensor(const std::string& name) const {
    auto it = name_map_.find(name);
    if (it != name_map_.end() && it->second < tensors_.size()) {
        return &tensors_[it->second];
    }
    return nullptr;
}

Tensor* TensorInput::GetTensor(const std::string& name) {
    auto it = name_map_.find(name);
    if (it != name_map_.end() && it->second < tensors_.size()) {
        return &tensors_[it->second];
    }
    return nullptr;
}

const Tensor* TensorInput::GetTensor(size_t index) const {
    if (index < tensors_.size()) {
        return &tensors_[index];
    }
    return nullptr;
}

Tensor* TensorInput::GetTensor(size_t index) {
    if (index < tensors_.size()) {
        return &tensors_[index];
    }
    return nullptr;
}

void TensorInput::Clear() {
    tensors_.clear();
    name_map_.clear();
}

// ============================================================================
// TensorOutput Implementation
// ============================================================================

void TensorOutput::AddTensor(const Tensor& tensor) {
    name_map_[tensor.Name()] = tensors_.size();
    tensors_.push_back(tensor);
}

void TensorOutput::AddTensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data) {
    name_map_[name] = tensors_.size();
    tensors_.emplace_back(name, shape, data);
}

const Tensor* TensorOutput::GetTensor(const std::string& name) const {
    auto it = name_map_.find(name);
    if (it != name_map_.end() && it->second < tensors_.size()) {
        return &tensors_[it->second];
    }
    return nullptr;
}

Tensor* TensorOutput::GetTensor(const std::string& name) {
    auto it = name_map_.find(name);
    if (it != name_map_.end() && it->second < tensors_.size()) {
        return &tensors_[it->second];
    }
    return nullptr;
}

const Tensor* TensorOutput::GetTensor(size_t index) const {
    if (index < tensors_.size()) {
        return &tensors_[index];
    }
    return nullptr;
}

Tensor* TensorOutput::GetTensor(size_t index) {
    if (index < tensors_.size()) {
        return &tensors_[index];
    }
    return nullptr;
}

void TensorOutput::Clear() {
    tensors_.clear();
    name_map_.clear();
}

// ============================================================================
// InferenceBackend Factory
// ============================================================================

std::unique_ptr<InferenceBackend> InferenceBackend::Create(const ModelConfig& config) {
    if (config.backend_type == BackendType::CPU_MOCK) {
        auto backend = std::make_unique<MockInferenceBackend>();
        if (backend->Init(config)) {
            return backend;
        }
        return nullptr;
    }

    if (config.backend_type == BackendType::ONNX_RUNTIME) {
        auto backend = std::make_unique<OnnxRuntimeBackend>();
        if (backend->Init(config)) {
            return backend;
        }
        return nullptr;
    }

    // AUTO mode: Try ONNX Runtime first, fall back to Mock on failure
    auto ort_backend = std::make_unique<OnnxRuntimeBackend>();
    if (ort_backend->Init(config)) {
        return ort_backend;
    }

    // Fallback to CPU Mock
    auto mock_backend = std::make_unique<MockInferenceBackend>();
    ModelConfig mock_cfg = config;
    mock_cfg.backend_type = BackendType::CPU_MOCK;
    if (mock_backend->Init(mock_cfg)) {
        return mock_backend;
    }

    return nullptr;
}

// ============================================================================
// OnnxRuntimeBackend Implementation
// ============================================================================

OnnxRuntimeBackend::OnnxRuntimeBackend() = default;

OnnxRuntimeBackend::~OnnxRuntimeBackend() {
    Cleanup();
}

void OnnxRuntimeBackend::Cleanup() {
#ifdef HAVE_ONNXRUNTIME
    if (session_) {
        delete static_cast<OrtSessionHolder*>(session_);
        session_ = nullptr;
    }
#endif
    is_loaded_ = false;
    is_simulated_ = false;
}

bool OnnxRuntimeBackend::Init(const ModelConfig& config) {
    Cleanup();
    config_ = config;

#ifdef HAVE_ONNXRUNTIME
    if (!config_.model_path.empty()) {
        if (InitOrtSession()) {
            is_loaded_ = true;
            is_simulated_ = false;
            return true;
        }
    }
#endif

    // If ORT is not available or model file does not exist, check simulation flag
    if (config_.simulate_if_no_ort) {
        is_simulated_ = true;
        is_loaded_ = true;
        return true;
    }

    return false;
}

bool OnnxRuntimeBackend::InitOrtSession() {
#ifdef HAVE_ONNXRUNTIME
    try {
        auto* holder = new OrtSessionHolder(config_.model_path.c_str(), config_.num_threads);

        size_t n_in = holder->session.GetInputCount();
        size_t n_out = holder->session.GetOutputCount();

        Ort::AllocatorWithDefaultOptions alloc;
        input_names_.clear();
        input_shapes_.clear();
        for (size_t i = 0; i < n_in; ++i) {
            Ort::AllocatedStringPtr nm = holder->session.GetInputNameAllocated(i, alloc);
            input_names_.emplace_back(nm.get());
            Ort::TypeInfo tti = holder->session.GetInputTypeInfo(i);
            auto ti = tti.GetTensorTypeAndShapeInfo();
            input_shapes_.push_back(ti.GetShape());
        }

        output_names_.clear();
        output_shapes_.clear();
        for (size_t i = 0; i < n_out; ++i) {
            Ort::AllocatedStringPtr nm = holder->session.GetOutputNameAllocated(i, alloc);
            output_names_.emplace_back(nm.get());
            Ort::TypeInfo tti = holder->session.GetOutputTypeInfo(i);
            auto ti = tti.GetTensorTypeAndShapeInfo();
            output_shapes_.push_back(ti.GetShape());
        }

        session_ = holder;
        return true;
    } catch (const std::exception& e) {
        return false;
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

bool OnnxRuntimeBackend::Forward(const TensorInput& input, TensorOutput& output) {
    if (!is_loaded_) {
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    bool success = false;
    if (is_simulated_) {
        success = ForwardSimulated(input, output);
    } else {
        success = ForwardOrt(input, output);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    latency_ms_ = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return success;
}

bool OnnxRuntimeBackend::ForwardOrt(const TensorInput& input, TensorOutput& output) {
#ifdef HAVE_ONNXRUNTIME
    if (!session_) return false;
    auto* holder = static_cast<OrtSessionHolder*>(session_);

    try {
        Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> in_tensors;
        std::vector<const char*> in_name_ptrs;

        for (size_t i = 0; i < input_names_.size(); ++i) {
            const Tensor* t = input.GetTensor(input_names_[i]);
            if (!t && i < input.Size()) {
                t = input.GetTensor(i);
            }
            if (!t) return false;

            in_tensors.push_back(Ort::Value::CreateTensor<float>(
                mem_info,
                const_cast<float*>(t->RawData()),
                static_cast<size_t>(t->NumElements()),
                t->Shape().data(),
                t->Shape().size()
            ));
            in_name_ptrs.push_back(input_names_[i].c_str());
        }

        std::vector<const char*> out_name_ptrs;
        for (const auto& name : output_names_) {
            out_name_ptrs.push_back(name.c_str());
        }

        auto ort_outs = holder->session.Run(
            Ort::RunOptions{nullptr},
            in_name_ptrs.data(), in_tensors.data(), in_tensors.size(),
            out_name_ptrs.data(), out_name_ptrs.size()
        );

        output.Clear();
        for (size_t i = 0; i < ort_outs.size(); ++i) {
            if (!ort_outs[i].IsTensor()) continue;
            auto info = ort_outs[i].GetTensorTypeAndShapeInfo();
            std::vector<int64_t> shape = info.GetShape();
            size_t count = info.GetElementCount();
            const float* data = ort_outs[i].GetTensorData<float>();

            std::string out_name = (i < output_names_.size()) ? output_names_[i] : ("output_" + std::to_string(i));
            std::vector<float> vec_data(data, data + count);
            output.AddTensor(out_name, shape, vec_data);
        }

        return true;
    } catch (...) {
        return false;
    }
#else
    (void)input;
    (void)output;
    return false;
#endif
}

bool OnnxRuntimeBackend::ForwardSimulated(const TensorInput& input, TensorOutput& output) {
    (void)input;
    output.Clear();

    // Generate simulated 3D obstacle bounding boxes [N, 11]
    // Layout: [x, y, z, width, length, height, heading, vx, vy, score, class_id]
    const int num_boxes = 5;
    const int box_dim = 11;
    std::vector<int64_t> boxes_shape = {num_boxes, box_dim};
    std::vector<float> boxes_data = {
        // Box 0: Vehicle ahead in ego lane
        15.0f,  0.0f, 0.0f,  2.0f, 4.5f, 1.6f,  0.0f,  5.0f, 0.0f,  0.92f, 0.0f,
        // Box 1: Vehicle duplicate (overlapping with Box 0, to test NMS suppression)
        15.2f,  0.1f, 0.0f,  2.0f, 4.5f, 1.6f,  0.02f, 5.0f, 0.0f,  0.85f, 0.0f,
        // Box 2: Pedestrian crossing to the left
        8.0f,   3.0f, 0.0f,  0.6f, 0.6f, 1.7f,  1.57f, 0.0f, -0.5f, 0.88f, 1.0f,
        // Box 3: Cyclist moving ahead to the right
        12.0f, -3.2f, 0.0f,  0.8f, 1.8f, 1.5f,  0.0f,  3.0f, 0.0f,  0.78f, 2.0f,
        // Box 4: Low confidence ghost detection (to test threshold filtering)
        25.0f,  1.0f, 0.0f,  1.8f, 4.0f, 1.5f,  0.0f,  0.0f, 0.0f,  0.15f, 0.0f,
    };
    output.AddTensor("boxes_3d", boxes_shape, boxes_data);

    // Lane geometry tensor: polynomial coefficients [2, 4] (left and right lane lines)
    // Left: y = 1.75; Right: y = -1.75
    std::vector<int64_t> lane_shape = {2, 4};
    std::vector<float> lane_data = {
        1.75f, 0.0f, 0.0f, 0.0f,
       -1.75f, 0.0f, 0.0f, 0.0f
    };
    output.AddTensor("lane_geometry", lane_shape, lane_data);

    return true;
}

// ============================================================================
// MockInferenceBackend Implementation
// ============================================================================

MockInferenceBackend::MockInferenceBackend() = default;

bool MockInferenceBackend::Init(const ModelConfig& config) {
    config_ = config;
    deterministic_obstacles_ = config.deterministic_obstacles;
    call_count_ = 0;
    latency_ms_ = 0.0;
    is_loaded_ = true;
    return true;
}

bool MockInferenceBackend::Forward(const TensorInput& input, TensorOutput& output) {
    if (!is_loaded_) {
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    output.Clear();

    // 1. Generate 3D obstacle boxes
    // Format: [x, y, z, width, length, height, heading, vx, vy, score, class_id]
    // Classes: 0: Vehicle, 1: Pedestrian, 2: Cyclist, 3: Construction
    const int num_boxes = 5;
    const int box_dim = 11;
    std::vector<int64_t> boxes_shape = {num_boxes, box_dim};

    // Slight deterministic variation based on call_count if needed
    float x_offset = deterministic_obstacles_ ? 0.0f : static_cast<float>(call_count_ % 10) * 0.1f;

    std::vector<float> boxes_data = {
        // Box 0: Vehicle ahead in ego lane
        15.0f + x_offset,  0.0f, 0.0f,  2.0f, 4.5f, 1.6f,  0.0f,  5.0f, 0.0f,  0.92f, 0.0f,
        // Box 1: Vehicle duplicate (overlapping with Box 0, IoU > 0.8, to verify NMS)
        15.2f + x_offset,  0.1f, 0.0f,  2.0f, 4.5f, 1.6f,  0.02f, 5.0f, 0.0f,  0.85f, 0.0f,
        // Box 2: Pedestrian on the left
        8.0f,   3.0f, 0.0f,  0.6f, 0.6f, 1.7f,  1.57f, 0.0f, -0.5f, 0.88f, 1.0f,
        // Box 3: Cyclist ahead on the right
        12.0f, -3.2f, 0.0f,  0.8f, 1.8f, 1.5f,  0.0f,  3.0f, 0.0f,  0.78f, 2.0f,
        // Box 4: Noise detection below confidence threshold (< 0.3)
        25.0f,  1.0f, 0.0f,  1.8f, 4.0f, 1.5f,  0.0f,  0.0f, 0.0f,  0.15f, 0.0f,
    };
    output.AddTensor("boxes_3d", boxes_shape, boxes_data);

    // 2. Lane geometry tensor: [num_lanes=2, 4 coeffs (c0, c1, c2, c3)]
    std::vector<int64_t> lane_shape = {2, 4};
    std::vector<float> lane_data = {
        1.75f, 0.0f, 0.0f, 0.0f,   // Left lane line at y = +1.75m
       -1.75f, 0.0f, 0.0f, 0.0f    // Right lane line at y = -1.75m
    };
    output.AddTensor("lane_geometry", lane_shape, lane_data);

    // Optional: Echo or inspect input features to maintain pipeline continuity
    if (!input.Empty()) {
        const Tensor* feature_t = input.GetTensor(0);
        if (feature_t && feature_t->NumElements() > 0) {
            // Echo input tensor count or feature dimension
            output.AddTensor("feature_summary", {1}, {static_cast<float>(feature_t->NumElements())});
        }
    }

    call_count_++;
    auto end_time = std::chrono::high_resolution_clock::now();
    latency_ms_ = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    // Guarantee non-zero latency measurement for statistical profiling
    if (latency_ms_ <= 0.0) {
        latency_ms_ = 0.01;
    }
    return true;
}

} // namespace inference
} // namespace perception
} // namespace apollo
