# 规划速度剖面：ST 图 + DP

> 本文是规划速度剖面的权威说明；[算法栈](ALGORITHM_STACK.md)只保留全栈总览。

## 职责与实现

`planning` 是速度和轨迹的唯一权威：行为层提供意图与目标速度，规划层输出
`planning/trajectory` 的速度点；`control` 只跟踪轨迹，`safety_control` 只做安全闸门。

速度剖面由以下实现共同构成：

| 项目 | 位置 | 作用 |
|---|---|---|
| ST 图 + DP 求解器 | `modules/adas_nodes/st_graph.{h,c}` | 纯 C 时空网格搜索，无第三方依赖 |
| 接入与轨迹组装 | `modules/adas_nodes/planning_node.cpp` | 将路径曲率、灯和障碍物约束组装为输入，并写入轨迹 |
| Python 对照仿真 | `tools/speed_planner_sim.py` | 在移植或改参前验证红灯、曲率、跟停、掉头和停走闭环 |

## 当前约束

每次规划在同一速度剖面中处理下列约束，而不是增加独立的速度 override：

- 红绿灯停止线和相位窗口；
- 本车道动态障碍物的时空占据；
- 路径曲率速度上限；
- 目标速度、加减速度和制动距离可行性。

对向车和跨车道决策仍属于行为层；规划只处理其职责范围内、可沿本车道速度剖面表达的约束。
掉头轨迹同样使用曲率约束限速。QP 时间域平滑已经过验证但会破坏 s 域约束，当前不接入；
DP 的加速度约束是唯一的速度平滑机制。

## 验证

算法或参数变更必须先运行 Python 对照仿真，再运行 C++ 管线回归：

```bash
python3 tools/speed_planner_sim.py --run-all
python3 tools/pipeline_check.py
python3 ci/evaluators/demo_evaluator.py --duration 45
```

`--run-all` 覆盖红灯停车、弯道限速、跟停再起步、同向慢车、掉头曲率和信号灯停走。
完整的分层门禁见[算法验证工作流](ALGORITHM_VERIFY_PATTERN.md)。
