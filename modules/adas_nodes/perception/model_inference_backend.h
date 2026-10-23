/**
 * @file model_inference_backend.h
 * @brief Apollo-aligned on-device perception model inference backend interface.
 *
 * Provides a lightweight, modular inference backend abstraction supporting
 * ONNX Runtime (C/C++ API) with seamless CPU Mock fallback for 3D obstacle
 * and lane detection models.
 */

#ifndef MODULES_ADAS_NODES_PERCEPTION_MODEL_INFERENCE_BACKEND_H_
#define MODULES_ADAS_NODES_PERCEPTION_MODEL_INFERENCE_BACKEND_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <chrono>

namespace apollo {
namespace perception {
namespace inference {

/**
 * @brief Inference backend types
 */
enum class BackendType {
    AUTO = 0,         ///< Auto-fallback: try ONNX Runtime, fall back to CPU Mock
    ONNX_RUNTIME = 1, ///< ONNX Runtime execution engine (or simulated if library absent)
    CPU_MOCK = 2      ///< Deterministic CPU Mock backend (zero heavy deps, guaranteed 100% CI)
};

/**
 * @brief Device execution target
 */
enum class DeviceType {
    CPU = 0,
    GPU = 1
};

/**
 * @brief Model initialization and runtime configuration
 */
struct ModelConfig {
    std::string model_path;
    BackendType backend_type{BackendType::AUTO};
    DeviceType device_type{DeviceType::CPU};
    int num_threads{1};
    int gpu_id{0};

    // Named tensor signatures
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<std::vector<int64_t>> input_shapes;

    // Simulation / Mock parameters
    bool simulate_if_no_ort{true};
    bool deterministic_obstacles{true};

    // Extra runtime parameters
    std::unordered_map<std::string, std::string> extra_params;
};

/**
 * @brief Multidimensional floating-point tensor container
 */
class Tensor {
public:
    Tensor() = default;
    Tensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data = {});

    const std::string& Name() const { return name_; }
    void SetName(const std::string& name) { name_ = name; }

    const std::vector<int64_t>& Shape() const { return shape_; }
    int64_t Dim(size_t index) const { return index < shape_.size() ? shape_[index] : 1; }
    size_t Rank() const { return shape_.size(); }

    int64_t NumElements() const;
    void Reshape(const std::vector<int64_t>& new_shape);

    const std::vector<float>& Data() const { return data_; }
    std::vector<float>& MutableData() { return data_; }
    const float* RawData() const { return data_.data(); }
    float* RawMutableData() { return data_.data(); }

    void SetData(const std::vector<float>& data);
    void SetData(const float* src, size_t count);

private:
    std::string name_;
    std::vector<int64_t> shape_;
    std::vector<float> data_;
};

/**
 * @brief Multi-input tensor bundle
 */
class TensorInput {
public:
    TensorInput() = default;

    void AddTensor(const Tensor& tensor);
    void AddTensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data = {});

    const Tensor* GetTensor(const std::string& name) const;
    Tensor* GetTensor(const std::string& name);
    const Tensor* GetTensor(size_t index) const;
    Tensor* GetTensor(size_t index);

    size_t Size() const { return tensors_.size(); }
    bool Empty() const { return tensors_.empty(); }
    void Clear();

    const std::vector<Tensor>& Tensors() const { return tensors_; }

private:
    std::vector<Tensor> tensors_;
    std::unordered_map<std::string, size_t> name_map_;
};

/**
 * @brief Multi-output tensor bundle
 */
class TensorOutput {
public:
    TensorOutput() = default;

    void AddTensor(const Tensor& tensor);
    void AddTensor(const std::string& name, const std::vector<int64_t>& shape, const std::vector<float>& data = {});

    const Tensor* GetTensor(const std::string& name) const;
    Tensor* GetTensor(const std::string& name);
    const Tensor* GetTensor(size_t index) const;
    Tensor* GetTensor(size_t index);

    size_t Size() const { return tensors_.size(); }
    bool Empty() const { return tensors_.empty(); }
    void Clear();

    const std::vector<Tensor>& Tensors() const { return tensors_; }

private:
    std::vector<Tensor> tensors_;
    std::unordered_map<std::string, size_t> name_map_;
};

/**
 * @brief Abstract base class for model inference backends
 */
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    /**
     * @brief Initialize model engine and load weights/computational graph
     */
    virtual bool Init(const ModelConfig& config) = 0;

    /**
     * @brief Execute forward inference step
     */
    virtual bool Forward(const TensorInput& input, TensorOutput& output) = 0;

    /**
     * @brief Get latency of the most recent forward execution (milliseconds)
     */
    virtual double GetLatencyMs() const = 0;

    /**
     * @brief Access current configuration
     */
    virtual const ModelConfig& GetConfig() const = 0;

    /**
     * @brief Check whether backend is loaded and ready
     */
    virtual bool IsLoaded() const = 0;

    /**
     * @brief Return concrete backend type
     */
    virtual BackendType GetType() const = 0;

    /**
     * @brief Factory method: creates and initializes backend according to config,
     *        handling auto-fallback from ONNX Runtime to CPU Mock if unavailable.
     */
    static std::unique_ptr<InferenceBackend> Create(const ModelConfig& config);
};

/**
 * @brief ONNX Runtime inference backend
 *
 * Uses ONNX Runtime C/C++ API when compiled with HAVE_ONNXRUNTIME; provides
 * simulated inference or fallback when ORT runtime library is absent.
 */
class OnnxRuntimeBackend : public InferenceBackend {
public:
    OnnxRuntimeBackend();
    ~OnnxRuntimeBackend() override;

    bool Init(const ModelConfig& config) override;
    bool Forward(const TensorInput& input, TensorOutput& output) override;
    double GetLatencyMs() const override { return latency_ms_; }
    const ModelConfig& GetConfig() const override { return config_; }
    bool IsLoaded() const override { return is_loaded_; }
    BackendType GetType() const override { return BackendType::ONNX_RUNTIME; }

    bool IsSimulated() const { return is_simulated_; }

private:
    void Cleanup();
    bool InitOrtSession();
    bool ForwardOrt(const TensorInput& input, TensorOutput& output);
    bool ForwardSimulated(const TensorInput& input, TensorOutput& output);

    ModelConfig config_;
    bool is_loaded_{false};
    bool is_simulated_{false};
    double latency_ms_{0.0};

    void* session_{nullptr};
    void* env_{nullptr};
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
    std::vector<std::vector<int64_t>> input_shapes_;
    std::vector<std::vector<int64_t>> output_shapes_;
};

/**
 * @brief CPU Mock inference backend
 *
 * Provides deterministic 3D obstacle and lane geometry inference.
 * Guaranteed 100% CI and cross-platform availability with zero heavy dependencies.
 */
class MockInferenceBackend : public InferenceBackend {
public:
    MockInferenceBackend();
    ~MockInferenceBackend() override = default;

    bool Init(const ModelConfig& config) override;
    bool Forward(const TensorInput& input, TensorOutput& output) override;
    double GetLatencyMs() const override { return latency_ms_; }
    const ModelConfig& GetConfig() const override { return config_; }
    bool IsLoaded() const override { return is_loaded_; }
    BackendType GetType() const override { return BackendType::CPU_MOCK; }

    void SetDeterministicObstacles(bool enable) { deterministic_obstacles_ = enable; }

private:
    ModelConfig config_;
    bool is_loaded_{false};
    bool deterministic_obstacles_{true};
    double latency_ms_{0.0};
    uint64_t call_count_{0};
};

} // namespace inference
} // namespace perception
} // namespace apollo

// FlowEngine compatibility alias
namespace flow {
namespace perception {
namespace inference {
using namespace ::apollo::perception::inference;
} // namespace inference
} // namespace perception
} // namespace flow

#endif // MODULES_ADAS_NODES_PERCEPTION_MODEL_INFERENCE_BACKEND_H_
