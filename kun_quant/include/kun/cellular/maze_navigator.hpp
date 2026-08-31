#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include "kun/core/types.hpp"
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>
#include <random>
#include <iostream>

namespace kun {

/**
 * @brief 迷宫地图与连续力学导航环境 (MazeEnvironment)
 * 经典欺骗性死胡同 (Cul-de-sac) 迷宫拓扑，专门检验智能体是否具备：
 * 1. 多路激光测距感知能力 (SENSE Raycasts)
 * 2. 迟滞门控记忆避障能力 (GATE Hysteresis)
 * 3. 目标方向指引决策能力 (Target Beacon Tracking)
 * 4. 碰撞自锁与倒车脱困能力 (Immune Lock & Reversal)
 */
class MazeEnvironment {
public:
    static constexpr int DEFAULT_WIDTH = 21;
    static constexpr int DEFAULT_HEIGHT = 21;

    struct Agent {
        uint64_t id{0};
        float x{1.5f};
        float y{1.5f};
        float theta{0.0f}; // 朝向角度 (弧度)
        float vx{0.0f};
        float vy{0.0f};
        bool reached_goal{false};
        int steps{0};
        float min_dist_to_goal{999.0f};
        int collision_count{0};
        std::vector<std::pair<float, float>> trail;
        float ray_dists[3]{1.0f, 1.0f, 1.0f}; // 前, 左45°, 右45°
        float goal_bearing{0.0f};
    };

    MazeEnvironment(int width = 21, int height = 21, uint32_t seed = 42)
        : width_(width), height_(height), rng_(seed) {
        generate_classic_maze();
    }

    void generate_classic_maze() {
        grid_.assign(width_ * height_, 1); // 1 = 墙, 0 = 通道
        heatmap_.assign(width_ * height_, 0);

        // 生成连通迷宫 (递归回溯/DFS)
        std::vector<std::pair<int, int>> stack;
        grid_[1 * width_ + 1] = 0;
        stack.push_back({1, 1});

        int dx[4] = {0, 0, 2, -2};
        int dy[4] = {2, -2, 0, 0};

        while (!stack.empty()) {
            auto [cx, cy] = stack.back();
            std::vector<int> dirs = {0, 1, 2, 3};
            std::shuffle(dirs.begin(), dirs.end(), rng_);

            bool carved = false;
            for (int d : dirs) {
                int nx = cx + dx[d];
                int ny = cy + dy[d];
                if (nx > 0 && nx < width_ - 1 && ny > 0 && ny < height_ - 1 && grid_[ny * width_ + nx] == 1) {
                    grid_[ny * width_ + nx] = 0;
                    grid_[(cy + dy[d] / 2) * width_ + (cx + dx[d] / 2)] = 0;
                    stack.push_back({nx, ny});
                    carved = true;
                    break;
                }
            }
            if (!carved) {
                stack.pop_back();
            }
        }

        start_x_ = 1.5f;
        start_y_ = 1.5f;
        goal_x_ = static_cast<float>(width_ - 2) + 0.5f;
        goal_y_ = static_cast<float>(height_ - 2) + 0.5f;
        grid_[(height_ - 2) * width_ + (width_ - 2)] = 0;
    }

    bool is_wall(float x, float y) const {
        int gx = static_cast<int>(std::floor(x));
        int gy = static_cast<int>(std::floor(y));
        if (gx < 0 || gx >= width_ || gy < 0 || gy >= height_) return true;
        return grid_[gy * width_ + gx] == 1;
    }

    // 投射光线测距
    float cast_ray(float start_x, float start_y, float angle, float max_range = 6.0f) const {
        float step = 0.05f;
        float cos_a = std::cos(angle);
        float sin_a = std::sin(angle);

        for (float dist = 0.05f; dist < max_range; dist += step) {
            float cx = start_x + cos_a * dist;
            float cy = start_y + sin_a * dist;
            if (is_wall(cx, cy)) {
                return dist / max_range; // 归一化 [0.0 ~ 1.0]
            }
        }
        return 1.0f;
    }

    void update_sensors(Agent& agent) const {
        agent.ray_dists[0] = cast_ray(agent.x, agent.y, agent.theta);
        agent.ray_dists[1] = cast_ray(agent.x, agent.y, agent.theta - 0.785398f); // -45°
        agent.ray_dists[2] = cast_ray(agent.x, agent.y, agent.theta + 0.785398f); // +45°

        float to_goal_x = goal_x_ - agent.x;
        float to_goal_y = goal_y_ - agent.y;
        float goal_angle = std::atan2(to_goal_y, to_goal_x);
        float diff = goal_angle - agent.theta;

        while (diff > 3.14159265f) diff -= 6.2831853f;
        while (diff < -3.14159265f) diff += 6.2831853f;
        agent.goal_bearing = std::clamp(diff / 3.14159265f, -1.0f, 1.0f);
    }

    bool step_agent(Agent& agent, const CellularOrganism::ActionOutputs& acts, float dt = 0.1f) {
        if (agent.reached_goal) return true;

        agent.steps++;

        // 动力学控制: positive_action 驱动前进, negative_action 驱动转向
        float thrust = static_cast<float>(acts.positive_action * 2.5);
        float steer  = static_cast<float>(acts.negative_action * 3.0);

        if (acts.immune_lock) {
            // 碰撞免疫自锁：触发倒车与原地旋转脱困
            thrust = -1.0f;
            steer = 3.14f;
        }

        agent.theta += steer * dt;
        while (agent.theta > 3.14159265f) agent.theta -= 6.2831853f;
        while (agent.theta < -3.14159265f) agent.theta += 6.2831853f;

        float target_vx = std::cos(agent.theta) * thrust;
        float target_vy = std::sin(agent.theta) * thrust;

        agent.vx = agent.vx * 0.7f + target_vx * 0.3f;
        agent.vy = agent.vy * 0.7f + target_vy * 0.3f;

        float next_x = agent.x + agent.vx * dt;
        float next_y = agent.y + agent.vy * dt;

        // 碰撞与滑移检测
        if (!is_wall(next_x, next_y)) {
            agent.x = next_x;
            agent.y = next_y;
        } else {
            agent.collision_count++;
            if (!is_wall(next_x, agent.y)) agent.x = next_x;
            else if (!is_wall(agent.x, next_y)) agent.y = next_y;
            else { agent.vx = 0.0f; agent.vy = 0.0f; }
        }

        // 记录热力图
        int gx = std::clamp(static_cast<int>(std::floor(agent.x)), 0, width_ - 1);
        int gy = std::clamp(static_cast<int>(std::floor(agent.y)), 0, height_ - 1);
        heatmap_[gy * width_ + gx]++;

        // 记录历史轨迹
        if (agent.trail.empty() || 
            (std::hypot(agent.x - agent.trail.back().first, agent.y - agent.trail.back().second) > 0.3f)) {
            agent.trail.push_back({agent.x, agent.y});
            if (agent.trail.size() > 150) agent.trail.erase(agent.trail.begin());
        }

        float cur_dist = std::hypot(goal_x_ - agent.x, goal_y_ - agent.y);
        agent.min_dist_to_goal = std::min(agent.min_dist_to_goal, cur_dist);

        if (cur_dist < 0.6f) {
            agent.reached_goal = true;
            return true;
        }

        update_sensors(agent);
        return false;
    }

    int get_width() const { return width_; }
    int get_height() const { return height_; }
    float get_start_x() const { return start_x_; }
    float get_start_y() const { return start_y_; }
    float get_goal_x() const { return goal_x_; }
    float get_goal_y() const { return goal_y_; }
    const std::vector<uint8_t>& get_grid() const { return grid_; }
    const std::vector<uint32_t>& get_heatmap() const { return heatmap_; }

private:
    int width_{21};
    int height_{21};
    float start_x_{1.5f};
    float start_y_{1.5f};
    float goal_x_{19.5f};
    float goal_y_{19.5f};
    std::vector<uint8_t> grid_;
    std::vector<uint32_t> heatmap_;
    mutable std::mt19937 rng_;
};

/**
 * @brief 迷宫形态发生细胞演化导航引擎 (MazeEvolutionEngine)
 */
class MazeEvolutionEngine {
public:
    explicit MazeEvolutionEngine(size_t population_size = 24, int maze_size = 21)
        : pop_size_(population_size), maze_(maze_size, maze_size), morph_engine_(population_size) {
        reset_simulation();
    }

    void reset_simulation() {
        agents_.clear();
        auto& pop = morph_engine_.population();
        for (size_t i = 0; i < pop_size_ && i < pop.size(); ++i) {
            pop[i].reset_state();   // 清除跨世代 EMA/迟滞记忆残留, 否则适应度带噪声、精英保留失效
        }
        for (size_t i = 0; i < pop_size_; ++i) {
            MazeEnvironment::Agent ag;
            ag.id = i + 1;
            ag.x = maze_.get_start_x();
            ag.y = maze_.get_start_y();
            ag.theta = 0.0f;
            ag.min_dist_to_goal = std::hypot(maze_.get_goal_x() - ag.x, maze_.get_goal_y() - ag.y);
            maze_.update_sensors(ag);
            agents_.push_back(ag);
        }
        step_count_ = 0;
    }

    void step_simulation() {
        step_count_++;
        auto& pop = morph_engine_.population();

        bool all_done = true;
        for (size_t i = 0; i < pop_size_ && i < pop.size(); ++i) {
            auto& ag = agents_[i];
            auto& org = pop[i];

            if (ag.reached_goal) continue;
            all_done = false;

            double inputs[4] = {
                ag.ray_dists[0],
                ag.ray_dists[1],
                ag.ray_dists[2],
                ag.goal_bearing
            };

            auto acts = org.forward(inputs);
            maze_.step_agent(ag, acts, 0.12f);
        }

        if (all_done || step_count_ >= max_steps_per_gen_) {
            evaluate_and_evolve();
            reset_simulation();
        }
    }

    void evaluate_and_evolve() {
        generation_++;
        auto& pop = morph_engine_.population();
        float init_dist = std::hypot(maze_.get_goal_x() - maze_.get_start_x(), maze_.get_goal_y() - maze_.get_start_y());

        int success_count = 0;
        float best_fit = -999.0f;
        size_t champ_idx = 0;

        for (size_t i = 0; i < pop.size(); ++i) {
            const auto& ag = agents_[i];
            auto& org = pop[i];

            float progress = (init_dist - ag.min_dist_to_goal) / init_dist;
            float fit = progress * 100.0f;

            if (ag.reached_goal) {
                fit += 300.0f + (static_cast<float>(max_steps_per_gen_ - ag.steps) * 1.5f);
                success_count++;
            }
            fit -= static_cast<float>(ag.collision_count) * 0.5f;

            org.fitness_score = fit;
            if (fit > best_fit) {
                best_fit = fit;
                champ_idx = i;
            }
        }

        success_rate_ = static_cast<float>(success_count) / static_cast<float>(pop_size_);
        champion_trail_ = agents_[champ_idx].trail;
        morph_engine_.evolve_generation();
    }

    void generate_new_maze() {
        maze_.generate_classic_maze();
        reset_simulation();
    }

    std::string to_json() const {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"generation\": " << generation_ << ",\n";
        oss << "  \"step_count\": " << step_count_ << ",\n";
        oss << "  \"success_rate\": " << success_rate_ << ",\n";
        oss << "  \"width\": " << maze_.get_width() << ",\n";
        oss << "  \"height\": " << maze_.get_height() << ",\n";
        oss << "  \"start\": [" << maze_.get_start_x() << "," << maze_.get_start_y() << "],\n";
        oss << "  \"goal\": [" << maze_.get_goal_x() << "," << maze_.get_goal_y() << "],\n";

        // 迷宫栅格
        oss << "  \"grid\": [";
        const auto& g = maze_.get_grid();
        for (size_t i = 0; i < g.size(); ++i) {
            oss << (int)g[i] << (i + 1 < g.size() ? "," : "");
        }
        oss << "],\n";

        // 智能体当前状态
        oss << "  \"agents\": [\n";
        for (size_t i = 0; i < agents_.size(); ++i) {
            const auto& a = agents_[i];
            oss << "    {\"id\":" << a.id << ",\"x\":" << a.x << ",\"y\":" << a.y 
                << ",\"theta\":" << a.theta << ",\"goal\":" << (a.reached_goal ? 1 : 0)
                << ",\"rays\":[" << a.ray_dists[0] << "," << a.ray_dists[1] << "," << a.ray_dists[2] << "]}"
                << (i + 1 < agents_.size() ? ",\n" : "\n");
        }
        oss << "  ],\n";

        // 冠军历史轨迹
        oss << "  \"champion_trail\": [";
        for (size_t i = 0; i < champion_trail_.size(); ++i) {
            oss << "[" << champion_trail_[i].first << "," << champion_trail_[i].second << "]"
                << (i + 1 < champion_trail_.size() ? "," : "");
        }
        oss << "]\n";
        oss << "}\n";
        return oss.str();
    }

    const MazeEnvironment& get_maze() const { return maze_; }
    const std::vector<MazeEnvironment::Agent>& get_agents() const { return agents_; }
    int get_generation() const { return generation_; }
    float get_success_rate() const { return success_rate_; }

private:
    size_t pop_size_{24};
    int max_steps_per_gen_{160};
    int step_count_{0};
    int generation_{1};
    float success_rate_{0.0f};
    MazeEnvironment maze_;
    MorphogeneticEvolutionEngine morph_engine_;
    std::vector<MazeEnvironment::Agent> agents_;
    std::vector<std::pair<float, float>> champion_trail_;
};

} // namespace kun
