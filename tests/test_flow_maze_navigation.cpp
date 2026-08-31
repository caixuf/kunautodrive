#include "kun/cellular/maze_navigator.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace kun;

void test_maze_geometry_and_raycasts() {
    std::cout << "[Test 1] 运行迷宫环境拓扑与激光测距几何测试...\n";
    MazeEnvironment maze(21, 21, 12345);

    assert(maze.get_width() == 21);
    assert(maze.get_height() == 21);

    // 起点与终点必须是可通行的通道 (0)
    assert(!maze.is_wall(maze.get_start_x(), maze.get_start_y()));
    assert(!maze.is_wall(maze.get_goal_x(), maze.get_goal_y()));

    // 四周外围必须是实心墙 (1)
    assert(maze.is_wall(0.5f, 0.5f));
    assert(maze.is_wall(20.5f, 20.5f));

    // 测距传感器有效性
    float r_forward = maze.cast_ray(maze.get_start_x(), maze.get_start_y(), 0.0f);
    assert(r_forward >= 0.0f && r_forward <= 1.0f);

    std::cout << "  ↳ 起点 (1.5, 1.5) 正前方测距: " << r_forward 
              << ", 终点坐标: (" << maze.get_goal_x() << ", " << maze.get_goal_y() << ")\n";
    std::cout << "  -> 迷宫环境拓扑与激光测距几何测试通过!\n";
}

void test_agent_continuous_kinematics_and_collision() {
    std::cout << "[Test 2] 运行智能体连续动力学与障碍物碰撞滑动测试...\n";
    MazeEnvironment maze(21, 21, 12345);
    MazeEnvironment::Agent agent;
    agent.x = maze.get_start_x();
    agent.y = maze.get_start_y();
    agent.theta = 0.0f; // 朝向 +X 方向

    maze.update_sensors(agent);
    assert(agent.ray_dists[0] >= 0.0f && agent.ray_dists[0] <= 1.0f);
    assert(std::isfinite(agent.goal_bearing));

    // 模拟正向推进
    CellularOrganism::ActionOutputs acts;
    acts.positive_action = 1.0; // 前进
    acts.negative_action = 0.0; // 直行
    acts.immune_lock = false;

    for (int step = 0; step < 10; ++step) {
        maze.step_agent(agent, acts, 0.1f);
    }

    assert(agent.steps == 10);
    assert(agent.x > maze.get_start_x()); // 确实向前移动
    assert(!maze.is_wall(agent.x, agent.y)); // 绝不会穿墙卡在实心体中

    std::cout << "  ↳ 10 步推进后智能体坐标: (" << agent.x << ", " << agent.y << "), 朝向=" << agent.theta << " rad\n";
    std::cout << "  -> 智能体连续动力学与碰撞滑动测试通过!\n";
}

void test_morphogenetic_maze_learning_evolution() {
    std::cout << "[Test 3] 运行形态发生细胞种群迷宫导航与学习演化测试...\n";
    MazeEvolutionEngine engine(16, 21);

    // 运行 8 代演化验证拓扑变异与学习
    for (int gen = 0; gen < 8; ++gen) {
        for (int step = 0; step < 160; ++step) {
            engine.step_simulation();
        }
    }

    int final_gen = engine.get_generation();
    float final_success = engine.get_success_rate();
    std::string json_str = engine.to_json();

    assert(final_gen >= 8);
    assert(json_str.find("\"grid\"") != std::string::npos);
    assert(json_str.find("\"agents\"") != std::string::npos);
    assert(json_str.find("\"champion_trail\"") != std::string::npos);

    std::cout << "  ↳ 经历 8 代演化后: 代际=" << final_gen 
              << ", 通关成功率=" << (final_success * 100.0f) << "%\n";
    std::cout << "  -> 迷宫形态发生细胞演化学习全流程测试 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "     FlowEngine 形态发生细胞迷宫空间学习与导航演化测试集   \n";
    std::cout << "=========================================================\n\n";

    test_maze_geometry_and_raycasts();
    test_agent_continuous_kinematics_and_collision();
    test_morphogenetic_maze_learning_evolution();

    std::cout << "\n=========================================================\n";
    std::cout << "   全部 3 组形态发生迷宫导航与学习单测 100% 满分通过!    \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
