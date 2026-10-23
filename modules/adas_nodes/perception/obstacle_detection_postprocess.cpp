/**
 * @file obstacle_detection_postprocess.cpp
 * @brief Implementation of 3D obstacle post-processing.
 */

#include "obstacle_detection_postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace apollo {
namespace perception {
namespace postprocess {

namespace {

struct Point2D {
    float x{0.0f};
    float y{0.0f};
};

inline float Cross2D(const Point2D& o, const Point2D& a, const Point2D& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

inline float PolygonArea(const std::vector<Point2D>& poly) {
    if (poly.size() < 3) return 0.0f;
    float area = 0.0f;
    for (size_t i = 0; i < poly.size(); ++i) {
        size_t j = (i + 1) % poly.size();
        area += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
    }
    return std::abs(area) * 0.5f;
}

// Line segment intersection for polygon clipping
Point2D ComputeIntersection(const Point2D& p1, const Point2D& p2,
                            const Point2D& c1, const Point2D& c2) {
    float a1 = p2.y - p1.y;
    float b1 = p1.x - p2.x;
    float c_val1 = a1 * p1.x + b1 * p1.y;

    float a2 = c2.y - c1.y;
    float b2 = c1.x - c2.x;
    float c_val2 = a2 * c1.x + b2 * c1.y;

    float det = a1 * b2 - a2 * b1;
    if (std::abs(det) < 1e-7f) {
        return p1;
    }
    return {(b2 * c_val1 - b1 * c_val2) / det, (a1 * c_val2 - a2 * c_val1) / det};
}

// Sutherland-Hodgman convex polygon clipping
std::vector<Point2D> ClipConvexPolygon(const std::vector<Point2D>& subject,
                                      const std::vector<Point2D>& clip) {
    std::vector<Point2D> output = subject;
    for (size_t i = 0; i < clip.size(); ++i) {
        if (output.empty()) break;
        std::vector<Point2D> input = output;
        output.clear();

        Point2D c1 = clip[i];
        Point2D c2 = clip[(i + 1) % clip.size()];

        for (size_t j = 0; j < input.size(); ++j) {
            Point2D p1 = input[j];
            Point2D p2 = input[(j + 1) % input.size()];

            float cross_p1 = Cross2D(c1, c2, p1);
            float cross_p2 = Cross2D(c1, c2, p2);

            bool p1_in = (cross_p1 >= -1e-6f);
            bool p2_in = (cross_p2 >= -1e-6f);

            if (p1_in && p2_in) {
                output.push_back(p2);
            } else if (p1_in && !p2_in) {
                output.push_back(ComputeIntersection(p1, p2, c1, c2));
            } else if (!p1_in && p2_in) {
                output.push_back(ComputeIntersection(p1, p2, c1, c2));
                output.push_back(p2);
            }
        }
    }
    return output;
}

std::vector<Point2D> GetBoxVertices2D(const BoundingBox3D& box) {
    float cos_h = std::cos(box.heading);
    float sin_h = std::sin(box.heading);
    float hl = box.length * 0.5f;
    float hw = box.width * 0.5f;

    std::vector<Point2D> pts(4);
    // CCW vertices
    pts[0] = {box.x - hl * cos_h + hw * sin_h, box.y - hl * sin_h - hw * cos_h};
    pts[1] = {box.x + hl * cos_h + hw * sin_h, box.y + hl * sin_h - hw * cos_h};
    pts[2] = {box.x + hl * cos_h - hw * sin_h, box.y + hl * sin_h + hw * cos_h};
    pts[3] = {box.x - hl * cos_h - hw * sin_h, box.y - hl * sin_h + hw * cos_h};
    return pts;
}

} // namespace

ObstacleDetectionPostprocess::ObstacleDetectionPostprocess() = default;

bool ObstacleDetectionPostprocess::Init(const PostprocessConfig& config) {
    config_ = config;
    if (config_.track_match_dist_m <= 0.0f) {
        config_.track_match_dist_m = 2.5f;
    }
    active_tracks_.clear();
    next_track_id_ = 1;
    latency_ms_ = 0.0;
    return true;
}

float ObstacleDetectionPostprocess::ComputeBEVIoU(const BoundingBox3D& a, const BoundingBox3D& b) {
    float area_a = a.length * a.width;
    float area_b = b.length * b.width;
    if (area_a <= 1e-6f || area_b <= 1e-6f) return 0.0f;

    // Fast path: axis-aligned boxes
    if (std::abs(a.heading) < 1e-4f && std::abs(b.heading) < 1e-4f) {
        float x_overlap = std::max(0.0f,
            std::min(a.x + a.length * 0.5f, b.x + b.length * 0.5f) -
            std::max(a.x - a.length * 0.5f, b.x - b.length * 0.5f));
        float y_overlap = std::max(0.0f,
            std::min(a.y + a.width * 0.5f, b.y + b.width * 0.5f) -
            std::max(a.y - a.width * 0.5f, b.y - b.width * 0.5f));
        float inter = x_overlap * y_overlap;
        float union_area = area_a + area_b - inter;
        return (union_area > 1e-6f) ? (inter / union_area) : 0.0f;
    }

    // General oriented 2D polygon intersection
    auto poly_a = GetBoxVertices2D(a);
    auto poly_b = GetBoxVertices2D(b);

    auto inter_poly = ClipConvexPolygon(poly_a, poly_b);
    float inter_area = PolygonArea(inter_poly);

    float union_area = area_a + area_b - inter_area;
    if (union_area <= 1e-6f) return 0.0f;
    return inter_area / union_area;
}

float ObstacleDetectionPostprocess::Compute3DIoU(const BoundingBox3D& a, const BoundingBox3D& b) {
    float bev_iou = ComputeBEVIoU(a, b);
    if (bev_iou <= 1e-6f) return 0.0f;

    // If heights are zero or negligible, treat as 2D BEV IoU
    if (a.height <= 1e-4f || b.height <= 1e-4f) {
        return bev_iou;
    }

    float z_min_a = a.z - a.height * 0.5f;
    float z_max_a = a.z + a.height * 0.5f;
    float z_min_b = b.z - b.height * 0.5f;
    float z_max_b = b.z + b.height * 0.5f;

    float z_overlap = std::max(0.0f, std::min(z_max_a, z_max_b) - std::max(z_min_a, z_min_b));
    if (z_overlap <= 1e-6f) return 0.0f;

    float vol_a = a.length * a.width * a.height;
    float vol_b = b.length * b.width * b.height;

    // Estimate intersection volume from BEV intersection area * z_overlap
    float area_a = a.length * a.width;
    float area_b = b.length * b.width;
    float inter_area = bev_iou * (area_a + area_b) / (1.0f + bev_iou);
    float vol_inter = inter_area * z_overlap;

    float vol_union = vol_a + vol_b - vol_inter;
    if (vol_union <= 1e-6f) return 0.0f;
    return vol_inter / vol_union;
}

void ObstacleDetectionPostprocess::ApplyNMS(std::vector<BoundingBox3D>& candidates,
                                           float iou_threshold,
                                           bool use_3d_iou,
                                           bool class_independent) {
    if (candidates.size() <= 1) return;

    // Sort descending by score
    std::sort(candidates.begin(), candidates.end(),
              [](const BoundingBox3D& a, const BoundingBox3D& b) {
                  return a.score > b.score;
              });

    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<BoundingBox3D> selected;

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        selected.push_back(candidates[i]);

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j]) continue;

            if (!class_independent && candidates[i].class_id != candidates[j].class_id) {
                continue;
            }

            float iou = use_3d_iou ? Compute3DIoU(candidates[i], candidates[j])
                                   : ComputeBEVIoU(candidates[i], candidates[j]);

            if (iou > iou_threshold) {
                suppressed[j] = true;
            }
        }
    }

    candidates = std::move(selected);
}

void ObstacleDetectionPostprocess::ExtractCandidates(const inference::TensorOutput& output,
                                                   std::vector<BoundingBox3D>& candidates,
                                                   LaneGeometry& extracted_lane_geo) {
    candidates.clear();
    extracted_lane_geo = config_.default_lane_geo;

    // 1. Check for standard 3D bounding box tensor ("boxes_3d", "detections", or "boxes")
    const inference::Tensor* boxes_tensor = output.GetTensor("boxes_3d");
    if (!boxes_tensor) boxes_tensor = output.GetTensor("detections");
    if (!boxes_tensor) boxes_tensor = output.GetTensor("boxes");
    if (!boxes_tensor && output.Size() > 0) boxes_tensor = output.GetTensor(0);

    if (boxes_tensor && boxes_tensor->Rank() >= 2) {
        const auto& shape = boxes_tensor->Shape();
        int64_t n_boxes = shape[shape.size() - 2];
        int64_t n_feats = shape.back();
        const float* data = boxes_tensor->RawData();

        for (int64_t i = 0; i < n_boxes; ++i) {
            const float* row = data + i * n_feats;
            BoundingBox3D box;
            if (n_feats >= 11) {
                // Layout: [x, y, z, width, length, height, heading, vx, vy, score, class_id]
                box.x = row[0];
                box.y = row[1];
                box.z = row[2];
                box.width = row[3];
                box.length = row[4];
                box.height = row[5];
                box.heading = row[6];
                box.vx = row[7];
                box.vy = row[8];
                box.score = row[9];
                box.class_id = static_cast<int>(std::round(row[10]));
            } else if (n_feats >= 9) {
                // Layout: [x, y, z, width, length, height, heading, score, class_id]
                box.x = row[0];
                box.y = row[1];
                box.z = row[2];
                box.width = row[3];
                box.length = row[4];
                box.height = row[5];
                box.heading = row[6];
                box.score = row[7];
                box.class_id = static_cast<int>(std::round(row[8]));
            } else if (n_feats >= 7) {
                box.x = row[0];
                box.y = row[1];
                box.z = row[2];
                box.width = row[3];
                box.length = row[4];
                box.height = row[5];
                box.score = row[6];
                box.class_id = 0;
            }
            candidates.push_back(box);
        }
    }

    // 2. Extract lane geometry tensor if available ("lane_geometry", "lanes")
    const inference::Tensor* lane_t = output.GetTensor("lane_geometry");
    if (!lane_t) lane_t = output.GetTensor("lanes");
    if (lane_t && lane_t->Rank() >= 2) {
        const auto& shape = lane_t->Shape();
        int64_t num_lanes = shape[shape.size() - 2];
        int64_t num_coeffs = shape.back();
        extracted_lane_geo.num_lanes = static_cast<int>(num_lanes);
        extracted_lane_geo.lane_polynomials.clear();

        const float* l_data = lane_t->RawData();
        for (int64_t i = 0; i < num_lanes; ++i) {
            std::vector<float> coeffs(l_data + i * num_coeffs, l_data + (i + 1) * num_coeffs);
            extracted_lane_geo.lane_polynomials.push_back(coeffs);
        }

        // Infer lane width from distance between lane lines if 2 lanes
        if (num_lanes >= 2 && num_coeffs >= 1) {
            float c0_left = extracted_lane_geo.lane_polynomials[0][0];
            float c0_right = extracted_lane_geo.lane_polynomials[1][0];
            double dist = std::abs(c0_left - c0_right);
            if (dist > 1.0 && dist < 10.0) {
                extracted_lane_geo.lane_width = dist;
            }
        }
    }
}

void ObstacleDetectionPostprocess::FilterByScore(std::vector<BoundingBox3D>& candidates, float threshold) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [threshold](const BoundingBox3D& box) {
            return box.score < threshold;
        }), candidates.end());
}

void ObstacleDetectionPostprocess::TransformToEgoFrame(std::vector<BoundingBox3D>& boxes,
                                                     const ExtrinsicParams& ext) {
    if (std::abs(ext.sensor_dx) < 1e-4f &&
        std::abs(ext.sensor_dy) < 1e-4f &&
        std::abs(ext.sensor_dz) < 1e-4f &&
        std::abs(ext.sensor_yaw) < 1e-4f) {
        return;
    }

    float cos_yaw = std::cos(ext.sensor_yaw);
    float sin_yaw = std::sin(ext.sensor_yaw);

    for (auto& box : boxes) {
        float ego_x = box.x * cos_yaw - box.y * sin_yaw + ext.sensor_dx;
        float ego_y = box.x * sin_yaw + box.y * cos_yaw + ext.sensor_dy;
        float ego_z = box.z + ext.sensor_dz;

        float ego_vx = box.vx * cos_yaw - box.vy * sin_yaw;
        float ego_vy = box.vx * sin_yaw + box.vy * cos_yaw;

        float ego_heading = box.heading + ext.sensor_yaw;
        while (ego_heading > M_PI) ego_heading -= 2.0f * M_PI;
        while (ego_heading < -M_PI) ego_heading += 2.0f * M_PI;

        box.x = ego_x;
        box.y = ego_y;
        box.z = ego_z;
        box.vx = ego_vx;
        box.vy = ego_vy;
        box.heading = ego_heading;
    }
}

void ObstacleDetectionPostprocess::AssignLaneAndHint(std::vector<BoundingBox3D>& boxes,
                                                    const LaneGeometry& lane_geo,
                                                    double ego_y) {
    int n_lanes = lane_geo.num_lanes > 0 ? lane_geo.num_lanes : 2;
    double lw = lane_geo.lane_width > 0.0 ? lane_geo.lane_width : 3.5;

    // Ego lane calculation from ego_y
    double ego_offset = (-ego_y) / lw + (n_lanes - 1) * 0.5;
    int ego_idx = static_cast<int>(ego_offset >= 0.0 ? ego_offset + 0.5 : ego_offset - 0.5);
    if (ego_idx < 0) ego_idx = 0;
    if (ego_idx >= n_lanes) ego_idx = n_lanes - 1;

    for (auto& box : boxes) {
        // World lateral position = ego_y + box.y
        double world_y = ego_y + box.y;
        double offset = (-world_y) / lw + (n_lanes - 1) * 0.5;
        int idx = static_cast<int>(offset >= 0.0 ? offset + 0.5 : offset - 0.5);
        if (idx < 0) idx = 0;
        if (idx >= n_lanes) idx = n_lanes - 1;

        box.lane_id = static_cast<int8_t>(idx);
        box.obs_lane_match_hint = (std::abs(idx - ego_idx) <= 1);
    }
}

void ObstacleDetectionPostprocess::UpdateTracking(std::vector<BoundingBox3D>& boxes) {
    float max_dist_sq = config_.track_match_dist_m * config_.track_match_dist_m;
    size_t num_existing = active_tracks_.size();
    std::vector<bool> matched_tracks(num_existing, false);
    std::vector<TrackedObject> new_tracks;

    for (auto& box : boxes) {
        float min_dist_sq = std::numeric_limits<float>::max();
        int best_idx = -1;

        for (size_t t = 0; t < num_existing; ++t) {
            if (matched_tracks[t]) continue;
            if (active_tracks_[t].class_id != box.class_id) continue;

            float dx = box.x - active_tracks_[t].last_x;
            float dy = box.y - active_tracks_[t].last_y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq < min_dist_sq && dist_sq < max_dist_sq) {
                min_dist_sq = dist_sq;
                best_idx = static_cast<int>(t);
            }
        }

        if (best_idx >= 0) {
            box.track_id = active_tracks_[best_idx].track_id;
            active_tracks_[best_idx].last_x = box.x;
            active_tracks_[best_idx].last_y = box.y;
            active_tracks_[best_idx].missing_frames = 0;
            matched_tracks[best_idx] = true;
        } else {
            box.track_id = next_track_id_++;
            TrackedObject new_track;
            new_track.track_id = box.track_id;
            new_track.last_x = box.x;
            new_track.last_y = box.y;
            new_track.class_id = box.class_id;
            new_track.missing_frames = 0;
            new_tracks.push_back(new_track);
        }
    }

    // Age missing tracks among existing tracks
    for (size_t t = 0; t < num_existing; ++t) {
        if (!matched_tracks[t]) {
            active_tracks_[t].missing_frames++;
        }
    }

    // Append newly allocated tracks
    for (const auto& trk : new_tracks) {
        active_tracks_.push_back(trk);
    }

    // Remove tracks absent for > 5 frames
    active_tracks_.erase(std::remove_if(active_tracks_.begin(), active_tracks_.end(),
        [](const TrackedObject& trk) {
            return trk.missing_frames > 5;
        }), active_tracks_.end());
}

void ObstacleDetectionPostprocess::PopulateObstacleList(const std::vector<BoundingBox3D>& boxes,
                                                       ObstacleList& out_list,
                                                       uint32_t frame_id,
                                                       uint64_t timestamp_us) {
    std::memset(&out_list, 0, sizeof(ObstacleList));
    out_list.frame_id = frame_id;
    out_list.timestamp_us = timestamp_us;

    uint32_t count = static_cast<uint32_t>(std::min(
        boxes.size(), static_cast<size_t>(config_.max_output_boxes)));
    out_list.count = count;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& src = boxes[i];
        Obstacle& dst = out_list.obstacles[i];

        dst.id = src.track_id;
        dst.x = src.x;
        dst.y = src.y;
        dst.vx = src.vx;
        dst.vy = src.vy;
        dst.width = src.width;
        dst.length = src.length;
        dst.lane_id = src.lane_id;

        // Map class_id to ObstacleType enum (from adas_msgs_gen.h)
        switch (src.class_id) {
            case 0:
                dst.type = static_cast<int8_t>(OBJ_TYPE_VEHICLE);
                break;
            case 1:
                dst.type = static_cast<int8_t>(OBJ_TYPE_PEDESTRIAN);
                break;
            case 2:
                dst.type = static_cast<int8_t>(OBJ_TYPE_CYCLIST);
                break;
            case 3:
                dst.type = static_cast<int8_t>(OBJ_TYPE_CONSTRUCTION);
                break;
            default:
                dst.type = static_cast<int8_t>(OBJ_TYPE_UNKNOWN);
                break;
        }

        dst.confidence = src.score;
        dst.obs_lane_match_hint = src.obs_lane_match_hint;
    }
}

bool ObstacleDetectionPostprocess::Process(const inference::TensorOutput& model_output,
                                          ObstacleList& out_list,
                                          uint32_t frame_id,
                                          uint64_t timestamp_us) {
    return Process(model_output, config_.default_lane_geo, 0.0, out_list, frame_id, timestamp_us);
}

bool ObstacleDetectionPostprocess::Process(const inference::TensorOutput& model_output,
                                          const LaneGeometry& lane_geo,
                                          double ego_y,
                                          ObstacleList& out_list,
                                          uint32_t frame_id,
                                          uint64_t timestamp_us) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Extract candidates and lane geometry
    std::vector<BoundingBox3D> candidates;
    LaneGeometry active_lane_geo = lane_geo;
    ExtractCandidates(model_output, candidates, active_lane_geo);

    // 2. Confidence threshold filtering
    FilterByScore(candidates, config_.score_threshold);

    // 3. 3D / BEV NMS suppression
    ApplyNMS(candidates, config_.nms_iou_threshold, config_.use_3d_iou, config_.class_independent_nms);

    // 4. Sensor -> Ego vehicle frame coordinate transformation
    TransformToEgoFrame(candidates, config_.extrinsic);

    // 5. Lane calculation and ego/adjacent match hint
    AssignLaneAndHint(candidates, active_lane_geo, ego_y);

    // 6. Multi-frame tracking ID association
    UpdateTracking(candidates);

    // 7. Fill ObstacleList struct
    PopulateObstacleList(candidates, out_list, frame_id, timestamp_us);

    auto end_time = std::chrono::high_resolution_clock::now();
    latency_ms_ = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    if (latency_ms_ <= 0.0) {
        latency_ms_ = 0.01;
    }
    return true;
}

} // namespace postprocess
} // namespace perception
} // namespace apollo
