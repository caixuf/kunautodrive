#include "kun/cellular/maze_navigator.hpp"
#include "kun/cellular/evolvable_task.hpp"
#include <iostream>
#include <cassert>
#include <cmath>
#include <set>

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

void test_evolvable_task_contract_and_discrete_step() {
    std::cout << "[Test 3] 验证 EvolvableTask 标准 Gym 契约 (MazeTask 规范执行)...\n";
    MazeTask task(11, 11, 777, 100);

    assert(std::string(task.name()) == "MazeNavigation");
    assert(task.obs_dim() == 4);
    assert(task.act_dim() == 4);

    task.reset(888);
    auto obs0 = task.current_observation();
    assert(obs0.size() == 4);
    assert(obs0[0] >= 0.0f && obs0[0] <= 1.0f);

    // 单步执行离散动作 0 (直行)
    auto res1 = task.step(0);
    assert(res1.obs.size() == 4);
    assert(res1.steps == 1);
    assert(!res1.done);

    // 单步执行离散动作 1 (左转)
    auto res2 = task.step(1);
    assert(res2.steps == 2);

    double fit = task.current_fitness();
    (void)fit;
    std::cout << "  ↳ Task ObsDim=" << task.obs_dim() << ", ActDim=" << task.act_dim()
              << ", 2 步推演后适应度=" << fit << ", MinDist=" << res2.min_dist_to_goal << "\n";
    std::cout << "  -> EvolvableTask 标准接口契约 100% 满分通过!\n";
}

void test_dataset_split_and_train_holdout_isolation() {
    std::cout << "[Test 4] 验证训练集与留出集种子隔离 (Train vs Holdout ID vs OOD Split)...\n";
    auto split = TaskDatasetSplit::create_default_maze_split();

    assert(split.train_seeds.size() == 10);
    assert(split.holdout_id_seeds.size() == 10);
    assert(split.holdout_ood_seeds.size() == 10);
    assert(split.train_map_size == 11);
    assert(split.ood_map_size == 19);

    // 严密断言：三个集合种子绝对两两互斥，零数据泄露
    std::set<uint32_t> all_seeds;
    for (uint32_t s : split.train_seeds) {
        assert(all_seeds.find(s) == all_seeds.end());
        all_seeds.insert(s);
    }
    for (uint32_t s : split.holdout_id_seeds) {
        assert(all_seeds.find(s) == all_seeds.end());
        all_seeds.insert(s);
    }
    for (uint32_t s : split.holdout_ood_seeds) {
        assert(all_seeds.find(s) == all_seeds.end());
        all_seeds.insert(s);
    }

    assert(all_seeds.size() == 30);
    std::cout << "  ↳ 训练集 10 种子, ID 留出集 10 种子, OOD 留出集 10 种子 (30 种子完全隔离无泄露)\n";
    std::cout << "  -> 评测集划分与种子隔离机制 100% 满分通过!\n";
}

void test_oos_champion_reporting_and_gate_checking() {
    std::cout << "[Test 5] 验证 OOS 留出综合评测报告、WL 拓扑哈希与门禁判定 (M1 Gate)...\n";
    auto org = CellularOrganism::create_seed_organism(505);
    auto split = TaskDatasetSplit::create_default_maze_split();

    // 缩减种子数加速单测
    split.train_seeds = {101, 102, 103};
    split.holdout_id_seeds = {201, 202, 203};
    split.holdout_ood_seeds = {301, 302};
    split.max_steps_per_episode = 40;

    MazeTask train_task(split.train_map_size, split.train_map_size, 42, split.max_steps_per_episode);
    MazeTask id_task(split.train_map_size, split.train_map_size, 99, split.max_steps_per_episode);
    MazeTask ood_task(split.ood_map_size, split.ood_map_size, 199, split.max_steps_per_episode * 2);

    auto report = TaskEvaluator::evaluate_task_split(train_task, id_task, ood_task, org, split, 0.70);

    assert(!report.topology_hash.empty());
    assert(report.topology_hash.find("WL-") != std::string::npos);
    assert(report.train_metrics.num_episodes == 3);
    assert(report.holdout_id_metrics.num_episodes == 3);
    assert(report.holdout_ood_metrics.num_episodes == 2);

    std::string json_report = report.to_json();
    assert(json_report.find("\"topology_hash\"") != std::string::npos);
    assert(json_report.find("\"holdout_id\"") != std::string::npos);
    assert(json_report.find("\"holdout_ood\"") != std::string::npos);

    std::cout << "  ↳ 拓扑哈希: " << report.topology_hash << "\n";
    std::cout << "  ↳ 判定结论: " << report.verdict << "\n";
    std::cout << "  -> OOS 留出综合评测与门禁判定 100% 满分通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "     FlowEngine 形态发生细胞迷宫空间学习与导航演化测试集   \n";
    std::cout << "=========================================================\n\n";

    test_maze_geometry_and_raycasts();
    test_agent_continuous_kinematics_and_collision();
    test_evolvable_task_contract_and_discrete_step();
    test_dataset_split_and_train_holdout_isolation();
    test_oos_champion_reporting_and_gate_checking();

    std::cout << "\n=========================================================\n";
    std::cout << "   全部 5 组 M1 任务契约与留出评测单测 100% 满分通过!    \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
