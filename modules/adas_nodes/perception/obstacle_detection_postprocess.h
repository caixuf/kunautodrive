/**
 * @file obstacle_detection_postprocess.h
 * @brief Apollo-aligned 3D obstacle detection post-processing pipeline.
 *
 * Handles:
 *  - Confidence threshold filtering
 *  - 3D / BEV oriented bounding box NMS (Non-Maximum Suppression)
 *  - Sensor to vehicle body coordinate frame transformation
 *  - Lane assignment and ego/adjacent lane match hints
 *  - Temporal tracking ID assignment
 *  - Serialization into `ObstacleList` struct (aligned with adas_msgs_gen.h)
 */

#ifndef MODULES_ADAS_NODES_PERCEPTION_OBSTACLE_DETECTION_POSTPROCESS_H_
#define MODULES_ADAS_NODES_PERCEPTION_OBSTACLE_DETECTION_POSTPROCESS_H_

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <chrono>

#include "model_inference_backend.h"
#include "adas_msgs_gen.h"

namespace apollo {
namespace perception {
namespace postprocess {

/**
 * @brief Internal representation of a 3D bounding box detection
 */
struct BoundingBox3D {
    float x{0.0f};       ///< Center x (m, longitudinal: +forward)
    float y{0.0f};       ///< Center y (m, lateral: +left)
    float z{0.0f};       ///< Center z (m, vertical: +up)
    float width{0.0f};   ///< Box lateral extent (m)
    float length{0.0f};  ///< Box longitudinal extent (m)
    float height{0.0f};  ///< Box vertical extent (m)
    float heading{0.0f}; ///< Yaw angle (rad, CCW from +x)
    float vx{0.0f};      ///< Longitudinal relative speed (m/s)
    float vy{0.0f};      ///< Lateral relative speed (m/s)
    float score{0.0f};   ///< Detection confidence [0.0, 1.0]
    int class_id{0};     ///< 0: Vehicle, 1: Pedestrian, 2: Cyclist, 3: Construction
    uint32_t track_id{0};///< Tracking ID across frames
    int8_t lane_id{-1};  ///< Lane index: 0=leftmost, N-1=rightmost, -1=unassigned
    bool obs_lane_match_hint{false}; ///< True if in ego or adjacent lane (±1 lane)
};

/**
 * @brief Extrinsic transform from sensor coordinate frame to ego vehicle body frame
 */
struct ExtrinsicParams {
    float sensor_dx{0.0f};  ///< Sensor mounting x-offset in ego frame (m, +forward)
    float sensor_dy{0.0f};  ///< Sensor mounting y-offset in ego frame (m, +left)
    float sensor_dz{0.0f};  ///< Sensor mounting z-offset in ego frame (m, +up)
    float sensor_yaw{0.0f}; ///< Sensor mounting yaw rotation relative to ego frame (rad)
};

/**
 * @brief Lane geometry specification for lane assignment
 */
struct LaneGeometry {
    int num_lanes{2};
    double lane_width{3.5};
    int ego_lane_idx{0};
    // Optional polynomial coefficients: y(x) = c0 + c1*x + c2*x^2 + c3*x^3
    std::vector<std::vector<float>> lane_polynomials;
};

/**
 * @brief Configuration for obstacle detection postprocessing
 */
struct PostprocessConfig {
    float score_threshold{0.3f};
    float nms_iou_threshold{0.5f};
    int max_candidates{256};
    int max_output_boxes{128};

    bool use_3d_iou{true};
    bool class_independent_nms{false};

    ExtrinsicParams extrinsic;
    LaneGeometry default_lane_geo;

    // Tracking association parameter
    float track_match_dist_m{2.5f};
};

/**
 * @brief Post-processor class for model inference outputs
 */
class ObstacleDetectionPostprocess {
public:
    ObstacleDetectionPostprocess();
    ~ObstacleDetectionPostprocess() = default;

    /**
     * @brief Initialize postprocessor with configuration parameters
     */
    bool Init(const PostprocessConfig& config);

    /**
     * @brief Postprocess model output tensor and fill `ObstacleList`
     *
     * @param model_output Model output tensors from InferenceBackend
     * @param out_list Output ObstacleList struct
     * @param frame_id Frame sequence identifier
     * @param timestamp_us CLOCK_MONOTONIC timestamp in microseconds
     * @return true on success
     */
    bool Process(const inference::TensorOutput& model_output,
                 ObstacleList& out_list,
                 uint32_t frame_id = 0,
                 uint64_t timestamp_us = 0);

    /**
     * @brief Process with explicit ego pose and lane geometry
     */
    bool Process(const inference::TensorOutput& model_output,
                 const LaneGeometry& lane_geo,
                 double ego_y,
                 ObstacleList& out_list,
                 uint32_t frame_id = 0,
                 uint64_t timestamp_us = 0);

    /**
     * @brief Get latency of most recent postprocessing run in milliseconds
     */
    double GetLatencyMs() const { return latency_ms_; }

    /**
     * @brief Access config
     */
    const PostprocessConfig& GetConfig() const { return config_; }

    /**
     * @brief Static utility: Calculate 2D oriented BEV IoU between two boxes
     */
    static float ComputeBEVIoU(const BoundingBox3D& a, const BoundingBox3D& b);

    /**
     * @brief Static utility: Calculate 3D oriented IoU between two boxes
     */
    static float Compute3DIoU(const BoundingBox3D& a, const BoundingBox3D& b);

    /**
     * @brief Static utility: Perform NMS on a candidate vector of boxes
     */
    static void ApplyNMS(std::vector<BoundingBox3D>& candidates,
                         float iou_threshold,
                         bool use_3d_iou = true,
                         bool class_independent = false);

private:
    void ExtractCandidates(const inference::TensorOutput& output,
                           std::vector<BoundingBox3D>& candidates,
                           LaneGeometry& extracted_lane_geo);

    void FilterByScore(std::vector<BoundingBox3D>& candidates, float threshold);

    void TransformToEgoFrame(std::vector<BoundingBox3D>& boxes, const ExtrinsicParams& ext);

    void AssignLaneAndHint(std::vector<BoundingBox3D>& boxes,
                           const LaneGeometry& lane_geo,
                           double ego_y);

    void UpdateTracking(std::vector<BoundingBox3D>& boxes);

    void PopulateObstacleList(const std::vector<BoundingBox3D>& boxes,
                              ObstacleList& out_list,
                              uint32_t frame_id,
                              uint64_t timestamp_us);

    PostprocessConfig config_;
    double latency_ms_{0.0};

    // Tracking state
    struct TrackedObject {
        uint32_t track_id{0};
        float last_x{0.0f};
        float last_y{0.0f};
        int class_id{0};
        int missing_frames{0};
    };
    std::vector<TrackedObject> active_tracks_;
    uint32_t next_track_id_{1};
};

} // namespace postprocess
} // namespace perception
} // namespace apollo

// FlowEngine compatibility alias
namespace flow {
namespace perception {
namespace postprocess {
using namespace ::apollo::perception::postprocess;
} // namespace postprocess
} // namespace perception
} // namespace flow

#endif // MODULES_ADAS_NODES_PERCEPTION_OBSTACLE_DETECTION_POSTPROCESS_H_
