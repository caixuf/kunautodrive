# KunAutoDrive Pipeline 架构

> **契约真相源说明** — 本文为架构速览；Topic 连线契约以 `config/pipeline*.json` 与 `NodePlugin` 的 `s_inputs`/`s_outputs` 为单一事实源（由 `ci/gates/topic_contract_check.py` 在 CI 强制校验）。更多细节见 [ALGORITHM_STACK.md](ALGORITHM_STACK.md) 与 [CODE_WIKI.md](CODE_WIKI.md)。

KunAutoDrive 默认仿真管线（`config/pipeline.json`）由 **20 个节点插件**组成（传感器版 `config/pipeline_sensor.json` 与实车模板 `config/pipeline_car.json` 各 **19 个节点**），通过 `flow_launcher <pipeline.json>` 配置驱动加载。

## 20 节点数据流总览

```text
flowsim (6-DOF + IDM/MOBIL) -> sensor_model (16-beam LiDAR / Radar / GPS)
  -> perception + bev_detection (CenterPoint-Lite) + lane_detection
  -> object_tracker (Hungarian EKF) -> prediction (3-Modal Frenet) + fusion (EKF)
  -> behavior_planner + navigation -> planning (ScenarioManager: Frenet+DP / Hybrid A*)
  -> control (Stanley / LTV-MPC / ManeuverTracker) -> safety_control (FlowCoro TTC/MRM)
  -> odd_monitor + tor_manager + monitor -> flowmond (:8800) -> FlowBoard 3D / PEM
                                                |
                                                +-> data_recorder -> learner -> inference -> model_ota
```

## 20 节点清单（`config/pipeline.json`）

| 类别 | 节点 | 说明 |
|------|------|------|
| **仿真与传感** | `flowsim`, `sensor_model` | 6-DOF 簧载悬架动力学 + IDM/MOBIL 交通流；16 线 3D OBB LiDAR / 雷达 / GPS 噪声与退化 |
| **感知与跟踪** | `perception`, `bev_detection`, `lane_detection`, `object_tracker`, `prediction`, `fusion` | DBSCAN + 纯 C CenterPoint-Lite BEV CNN + 车道线多项式 + 64 槽位匈牙利 EKF + 3 模态 Frenet 预测 + EKF 定位融合 |
| **决策与控制** | `behavior_planner`, `navigation`, `planning`, `control`, `safety_control` | 行为 FSM + 路由导航 + ScenarioManager（Frenet+ST DP / Hybrid A*）+ Stanley/LTV-MPC + FlowCoro 安全闸门 |
| **L3 安全与监控** | `odd_monitor`, `tor_manager`, `monitor` | ODD 实时监控 + TOR 接管倒计时/MRM 触发 + 拓扑/3D 场景/PEM 采集 |
| **端到端学习闭环** | `data_recorder`, `inference`, `learner`, `model_ota` | 样本录制 + 影子推理（仅对比监控/软建议，不绕过 `safety_control`）+ 在线训练 + OTA |

> **Learning Loop:** `data_recorder` → `inference` → `learner` → `model_ota` 是车端学习闭环节点。
> `inference` 运行在影子模式（shadow mode），只发布 `inference/trajectory` 供对比监控，
> **不**接入真实控制链路。详见 [LEARNING_LOOP.md](LEARNING_LOOP.md)。

## 控制闭环

```
control (PID) → control/raw_cmd → safety_control → control/cmd → flowsim (bicycle model) → vehicle/state
                                              ↑                                              │
                                              └──────── planning/trajectory ←── planning ←── fusion/localization ←── fusion ←── sensor/lidar,gps
```

- **频率**: flowsim 60Hz，planning 20Hz，control 20Hz
- **安全包络**: safety_control 限制 throttle ≤ 0.85，steer ≤ 0.22 rad（低速 0.18 rad）
- **ACC**: time headway 1.4s，最小 gap 5m
- **Stuck recovery**: 静止 >3s + 横向卡在线附近 → 强制收敛到最近车道中心
- **Road guard**: |ego_y| > 2.1m → brake + steer toward center

## 可视化链路

可视化由统一的 flowmond 守护进程（`build/bin/flowmond`）提供，同时启用 IPC 桥接（首选）
与文件桥接（回退）两条数据链路。

| 链路 | 数据路径 | 端口 |
|------|---------|------|
| IPC 桥接（首选） | monitor_node → `stats_bridge` / `dashboard_bridge` → `flowmond` | 8800 |
| 文件桥接（回退） | monitor_node → `/tmp/flow_topology.json` → `flowmond` | 8800 |
| ASCII 俯视（调试） | flowsim_node → `/tmp/flow_ascii_overhead.txt`（每秒覆盖） | — |
| Foxglove 3D | `foxglove_bridge.py` 读取 JSON 文件 | 8765 |

## 配置格式 (pipeline.json)

```json
{
  "scheduler": { "mode": "choreo", "tick_us": 1000 },
  "processes": [
    {
      "name": "flowsim",
      "library_path": "build/lib/libflowsim_node.so",
      "auto_start": true,
      "publish": [
        {"topic": "vehicle/state", "type": "VehicleState", "qos": {"depth": 1, "policy": "drop_oldest"}},
        {"topic": "road/geometry", "type": "RoadGeometry", "qos": {"depth": 1, "policy": "drop_oldest"}},
        {"topic": "road/traffic_lights", "qos": {"depth": 1, "policy": "drop_oldest"}},
        {"topic": "scene/frame", "qos": {"depth": 1, "policy": "drop_oldest"}},
        {"topic": "sim/tick", "qos": {"depth": 1, "policy": "drop_oldest"}},
        {"topic": "sim/collision", "qos": {"depth": 4, "policy": "drop_oldest"}}
      ],
      "subscribe": ["control/cmd"],
      "params": "{\"init_speed\":10.0,\"target_speed\":15.0,\"scenario_file\":\"scenarios/straight_road.json\"}"
    }
  ]
}
```

## 场景文件

| 场景 | 文件 | 要素 |
|------|------|------|
| 直路综合 | `scenarios/straight_road.json` | 4 车道直路 + 2 同向慢车 + 1 对向来车 + 1 行人 + 红绿灯 |

场景定义格式见 `include/scenario_loader.h`、`src/core/scenario_loader.c`。

## 关键参数

| 节点 | 参数 | 默认值 | 说明 |
|------|------|--------|------|
| flowsim | `init_speed` | 10.0 | 初始速度 m/s |
| flowsim | `target_speed` | 15.0 | 目标巡航速度 m/s |
| flowsim | `scenario_file` | `scenarios/straight_road.json` | 场景文件路径 |
| control | `pid_kp/ki/kd` | 800/50/100 | PID 纵向控制器 |
| control | `lat_kp` | 0.5 | 横向误差增益 (rad/m) |
| control | `lat_kd_heading` | 1.35 | 航向阻尼增益 |
| control | `yaw_damping` | 0.15 | 横摆角速度阻尼增益 |
| control | `steer_min_clamp` | 0.030 | 高速最小转向钳位 (rad)，动态模型需比运动学模型更高 |
| control | `lc_lat_accel_max` | 3.5 (高速) / 4.5 (低速) | 变道最大侧向加速度 (m/s²)，动态模型下轮胎松弛导致有效 ay 偏低 |
| control | `lane_change_blocked_timeout_s` | 0.6 | 变道阻塞超时 (s) |
| control | `lc_stable_wait_s` | 4.0 | 变道后稳定巡航时间 |
| control | `lc_cooldown_after_stable_s` | 1.5 | 稳定后冷却 |
| control | `lc_cooldown_after_return_s` | 2.0 | 返回原车道后冷却 |
| control | `min_overtake_gap_base` | 14.0 | 触发超车最小间距基准 (m) |
| safety_control | `max_throttle` | 1.0 | 最大油门 |
| safety_control | `max_steer` | 0.22 | 最大转向角 (rad) |
| safety_control | `time_headway` | 1.8 | 安全时距 (s) |

## 验证

```bash
# 回归评估器
python3 ci/evaluators/demo_evaluator.py --duration 45

# 烟测试
bash scripts/launcher_smoke.sh
```
