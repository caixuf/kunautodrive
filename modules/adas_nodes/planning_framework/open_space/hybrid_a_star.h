#pragma once

#include <vector>
#include <memory>
#include <queue>
#include <unordered_map>
#include <chrono>
#include "box2d.h"
#include "reeds_shepp_path.h"
#include "grid_search.h"

namespace flowengine {
namespace planning {

/**
 * @brief Vehicle geometric and kinematic parameters
 */
struct VehicleParam {
    double wheelbase{2.85};              // 车辆轴距 (m)
    double length{4.8};                  // 车长 (m)
    double width{2.0};                   // 车宽 (m)
    double back_edge_to_center{1.0};     // 后轴到车尾边缘的距离 (m)
    double max_steer{0.6};               // 最大前轮转向角 (rad, ~34.4 deg)
    int steer_slices{2};                 // 转向角离散级数 (单侧级数，共 2*slices+1 档)
    double step_size{1.0};               // 搜索单步前进弧长 (m)

    // Derived helpers
    double min_turn_radius() const { return wheelbase / std::tan(max_steer); }
    double rear_axle_to_center() const { return length * 0.5 - back_edge_to_center; }
};

/**
 * @brief Hybrid A* planner configuration
 */
struct HybridAStarConfig {
    double xy_grid_resolution{0.3};       // 3D 搜索 XY 网格桶分辨率 (m)
    double phi_grid_resolution{0.0872665};// 3D 搜索航向网格桶分辨率 (rad, ~5 deg)

    // 成本函数权重
    double weight_forward{1.0};           // 前进距离代价系数
    double weight_reverse{2.5};           // 倒车距离惩罚系数
    double weight_gear_switch{12.0};      // 换向/换挡 (Gear switch) 惩罚
    double weight_steer{1.2};             // 大角度转向惩罚
    double weight_steer_change{2.5};      // 转向变化率惩罚

    // 启发函数权重
    double weight_heuristic{1.05};        // 启发函数整体权重 (>= 1.0)

    // 分析扩展 (Analytic Expansion)
    double analytic_expansion_max_dist{15.0}; // 触发 Reeds-Shepp 分析扩展的最大目标距离 (m)
    int analytic_expansion_interval{2};        // 分析扩展调用频率 (每 N 个节点尝试一次)

    // 碰撞检测与安全边界
    double vehicle_safety_margin{0.10};        // 车辆 OBB 额外安全膨胀距离 (m)
    double check_step_size{0.2};               // 运动展开插值碰撞检测步长 (m)

    // 终止与搜索保护
    size_t max_iterations{40000};              // 最大扩展节点数
    double time_limit_ms{150.0};               // 超时软上限 (ms)
};

/**
 * @brief Planning result containing trajectory and statistics
 */
struct HybridAStarResult {
    bool success{false};
    std::vector<PathPoint> path;
    double total_length{0.0};
    int gear_switches{0};
    size_t expanded_nodes{0};
    double search_time_ms{0.0};
};

/**
 * @brief Obstacle environment container for planning
 */
struct Environment {
    std::vector<Box2d> obstacle_boxes;
    std::vector<LineSegment2d> obstacle_lines;
    std::vector<Vec2d> obstacle_points;
    double min_x{-50.0};
    double max_x{50.0};
    double min_y{-50.0};
    double max_y{50.0};
};

/**
 * @brief Hybrid A* Path Planner
 */
class HybridAStar {
public:
    HybridAStar() : HybridAStar(VehicleParam(), HybridAStarConfig()) {}
    HybridAStar(const VehicleParam& veh_param, const HybridAStarConfig& config);

    const VehicleParam& vehicle_param() const { return veh_param_; }
    const HybridAStarConfig& config() const { return config_; }

    void SetVehicleParam(const VehicleParam& veh_param);
    void SetConfig(const HybridAStarConfig& config);

    /**
     * @brief Plan a kinematically feasible, collision-free path.
     */
    bool Plan(double start_x, double start_y, double start_phi,
              double goal_x, double goal_y, double goal_phi,
              const Environment& env,
              HybridAStarResult* result);

    /**
     * @brief Check whether ego vehicle at (x, y, phi) collides with environment.
     */
    bool CheckVehicleCollision(double x, double y, double phi, const Environment& env) const;

    /**
     * @brief Build Ego vehicle Box2d at given rear axle pose (x, y, phi).
     */
    Box2d GetEgoVehicleBox(double x, double y, double phi, double safety_margin = 0.0) const;

private:
    struct SearchNode {
        double x{0.0};
        double y{0.0};
        double phi{0.0};
        double steer{0.0};
        int direction{1};       // 1 = forward, -1 = reverse

        double g_cost{0.0};
        double h_cost{0.0};
        double f_cost{0.0};

        int grid_x{0};
        int grid_y{0};
        int grid_phi{0};

        int index{0};
        int parent_index{-1};
        bool in_closed{false};

        std::vector<PathPoint> intermediate_points;
    };

    VehicleParam veh_param_;
    HybridAStarConfig config_;
    std::unique_ptr<ReedsShepp> reeds_shepp_;
    std::unique_ptr<GridSearch> grid_search_;

    static uint64_t CalcIndexKey(int gx, int gy, int gphi);
    static double Mod2Pi(double angle);
    static double NormalizeAngle2Pi(double angle);

    double CalculateEdgeCost(const SearchNode& parent, double step_len, int dir, double steer) const;
    double CalculateHeuristic(double x, double y, double phi, double goal_x, double goal_y, double goal_phi) const;

    bool TryAnalyticExpansion(const SearchNode& current,
                              double goal_x, double goal_y, double goal_phi,
                              const Environment& env,
                              std::vector<PathPoint>* analytic_path) const;
};

}  // namespace planning
}  // namespace flowengine
