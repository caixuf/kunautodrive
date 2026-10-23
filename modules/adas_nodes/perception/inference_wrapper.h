/**
 * @file inference_wrapper.h
 * @brief Unified Apollo-aligned 3D obstacle and lane inference wrapper.
 */

#ifndef MODULES_ADAS_NODES_PERCEPTION_INFERENCE_WRAPPER_H_
#define MODULES_ADAS_NODES_PERCEPTION_INFERENCE_WRAPPER_H_

#include <memory>
#include "model_inference_backend.h"
#include "obstacle_detection_postprocess.h"

namespace apollo {
namespace perception {

/**
 * @brief Unified Perception Inference Wrapper
 *
 * Couples inference backend (with ONNX Runtime / CPU Mock fallback)
 * with 3D postprocessing (confidence filtering, NMS, ego coordinate alignment,
 * lane association, and ObstacleList serialization).
 */
class InferenceWrapper {
public:
    InferenceWrapper() = default;
    ~InferenceWrapper() = default;

    /**
     * @brief Initialize inference backend and postprocessor
     */
    bool Init(const inference::ModelConfig& model_config,
              const postprocess::PostprocessConfig& post_config) {
        backend_ = inference::InferenceBackend::Create(model_config);
        if (!backend_ || !backend_->IsLoaded()) {
            return false;
        }
        if (!postprocess_.Init(post_config)) {
            return false;
        }
        return true;
    }

    /**
     * @brief Process input tensors end-to-end and directly output ObstacleList
     */
    bool Process(const inference::TensorInput& input,
                 ObstacleList& out_list,
                 uint32_t frame_id = 0,
                 uint64_t timestamp_us = 0) {
        if (!backend_) return false;

        inference::TensorOutput model_output;
        if (!backend_->Forward(input, model_output)) {
            return false;
        }

        if (!postprocess_.Process(model_output, out_list, frame_id, timestamp_us)) {
            return false;
        }

        total_latency_ms_ = backend_->GetLatencyMs() + postprocess_.GetLatencyMs();
        return true;
    }

    /**
     * @brief Process with explicit lane geometry and ego lateral coordinate
     */
    bool Process(const inference::TensorInput& input,
                 const postprocess::LaneGeometry& lane_geo,
                 double ego_y,
                 ObstacleList& out_list,
                 uint32_t frame_id = 0,
                 uint64_t timestamp_us = 0) {
        if (!backend_) return false;

        inference::TensorOutput model_output;
        if (!backend_->Forward(input, model_output)) {
            return false;
        }

        if (!postprocess_.Process(model_output, lane_geo, ego_y, out_list, frame_id, timestamp_us)) {
            return false;
        }

        total_latency_ms_ = backend_->GetLatencyMs() + postprocess_.GetLatencyMs();
        return true;
    }

    inference::InferenceBackend* GetBackend() const { return backend_.get(); }
    postprocess::ObstacleDetectionPostprocess* GetPostprocess() { return &postprocess_; }

    double GetTotalLatencyMs() const { return total_latency_ms_; }
    double GetInferenceLatencyMs() const { return backend_ ? backend_->GetLatencyMs() : 0.0; }
    double GetPostprocessLatencyMs() const { return postprocess_.GetLatencyMs(); }

private:
    std::unique_ptr<inference::InferenceBackend> backend_;
    postprocess::ObstacleDetectionPostprocess postprocess_;
    double total_latency_ms_{0.0};
};

} // namespace perception
} // namespace apollo

namespace flow {
namespace perception {
using InferenceWrapper = ::apollo::perception::InferenceWrapper;
} // namespace perception
} // namespace flow

#endif // MODULES_ADAS_NODES_PERCEPTION_INFERENCE_WRAPPER_H_
