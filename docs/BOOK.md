# 《KunAutoDrive：从零搭建自动驾驶中间件与算法栈》

> **这是什么**：一本从微内核底座、通信与调度讲起，经过感知、定位、决策、规划、控制、安全与执行器，最后落到仿真、评估和学习闭环的自动驾驶全栈实践书。正文在 `docs/book/`，本页是阅读顺序和源码对照。
>
> **怎么读**：按第零部到第六部往下读：第零部从这里开始，第一部框架骨架，第二部通信与时间，第三部执行与调度，第四部录制与回放，第五部自动驾驶算法栈，第六部仿真、可视化与学习闭环。表里的编号是新的阅读顺序；「现文件」链到 `docs/book/` 里**现在的文件名**。编号和文件名不一致时，以链接为准，例如新的第 05 章链到 [`book/03_message_bus.md`](book/03_message_bus.md)。各章文件以后由该章自己的改写改名；在那之前，用表里的链接打开正文。
>
> 正文里点到函数时写成 `路径::符号`。附录里的路径保持原样。

---

## 第零部　从这里开始

先把仓库编过、跑起来，再读后面的章。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 00 | 前言 | 这本书要造一个什么样的系统，后面各章共用的设计约束从哪里来 | [`book/00_preface.md`](book/00_preface.md) |
| 00b | 跑起来：构建、演示与 pipeline.json | 用 `build.sh` 编出来，用 `scripts/demo.sh` 跑起来，再读懂 `config/pipeline.json` 里的进程表 | [`book/00b_run_pipeline.md`](book/00b_run_pipeline.md) |

## 第一部　框架骨架

用 C 做出任务对象、插件和参数，状态机放在这一部的末尾。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 01 | 用 C 造一个对象 | 用结构体首成员和函数指针表做出 `TaskBase` / `TaskInterface`，管住 initialize、execute、cleanup | [`book/01_oop_in_c.md`](book/01_oop_in_c.md) |
| 02 | 可插拔 .so 与启动器 | `include/node_plugin.h::NodePlugin` 与 `include/node_plugin.h::NODE_PLUGIN_SYMBOL`（导出名字是 node_get_plugin），`src/flow_launcher.c` 按 pipeline 做 dlopen，`src/flow_node_host.c` 把同一份 .so 放进独立进程运行 | [`book/02_plugin_system.md`](book/02_plugin_system.md) |
| 03 | 注册中心与参数系统 | int/float 注册时带上下界，`include/param_registry.h::param_set_int` 与 `include/param_registry.h::param_set_float` 越界拒绝，范围内则写入当前值。`include/param_registry.h::param_set_callback` 与 `include/param_registry.h::param_enable_hot_reload` 没有别的 C/C++ 调用点，改值回调不会因此跑起来。跑起来的调参是 `src/flowctl.c` 的 `flowctl param` 经 `include/param_bridge.h::param_bridge_client_request` 写入，节点下一拍用 `include/param_registry.h::param_get_int` 与 `include/param_registry.h::param_get_float` 读出。任务、话题、类型和插件登记到 `include/flow_registry.h::flow_registry_register_task` 这一组函数 | [`book/03_registry_and_params.md`](book/03_registry_and_params.md) |
| 04 | 状态机 | 反射式状态机：转移表、guard、entry/exit，非法事件有明确策略 | [`book/08_state_machine.md`](book/08_state_machine.md) |

## 第二部　通信与时间

进程内总线、序列化、共享内存、发现和时钟。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 05 | 消息总线 | 进程内 Pub/Sub：`include/message_bus.h::message_bus_publish` / `include/message_bus.h::message_bus_subscribe`，以及 `include/message_bus.h::message_bus_set_topic_qos` | [`book/03_message_bus.md`](book/03_message_bus.md) |
| 06 | 类型 ID、IDL 与序列化 | `include/serializer.h::fnv1a_hash` 做类型 ID，`include/serializer.h::serializer_register_type` 登记类型。IDL 写在 `msg/adas_msgs.msg`，`tools/msg_codegen.py` 据此生成 C 头：结构体、FNV-1a 类型 ID、序列化函数。这份 .msg 是输入，不是生成物 | [`book/07_serializer.md`](book/07_serializer.md) |
| 07 | 共享内存 IPC | `include/ipc_channel.h::ipc_channel_open` / `include/ipc_channel.h::ipc_channel_publish` 的共享内存通道，仪表盘 JSON 走 `include/dashboard_bridge.h::dashboard_bridge_publish` | [`book/04_ipc_channel.md`](book/04_ipc_channel.md) |
| 08 | 统一传输与服务发现 | `include/transport.h::transport_publish` 统一收发；发现用组播 `include/discovery.h::DISC_MULTICAST_GROUP`（`239.255.0.100`）和 `include/discovery.h::DISC_MULTICAST_PORT`（5500）；跨机走 `include/network_transport.h::net_transport_connect` | [`book/09_discovery.md`](book/09_discovery.md) |
| 09 | 时钟服务 | `include/clock_service.h::clock_now_us` 逻辑调度可注入；`include/clock_service.h::clock_now_monotonic_wall_us` 专供物理延迟测量；`include/clock_service.h::clock_now_realtime_us` 为 Unix 绝对时钟 | [`book/06_clock_service.md`](book/06_clock_service.md) |

## 第三部　执行与调度

协程任务怎么挂到调度器上，以及任务绑到哪颗核。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 10 | C++20 协程 | `include/coroutine_task.h::CoroutineTask` 与 `include/coroutine_task.h::node_pump`。总线上用 `co_await` 等消息的有 `modules/adas_nodes/fusion_node.cpp::recv_any_for`（`fusion_bridge`）、`modules/adas_nodes/control_node.cpp::recv_any_for`（`ctrl_bridge`）、`modules/adas_nodes/planning_node.cpp::recv_any_for`（`plan_bridge`）、`modules/adas_nodes/perception_fusion_node.cpp::recv_any_for`（`pf_bridge`） | [`book/10_coroutine.md`](book/10_coroutine.md) |
| 11 | 调度器与绑核 | `include/scheduler.h::scheduler_register_task` 与 `include/scheduler.h::rate_control_init`。`include/scheduler.h::scheduler_set_params` 的 `cpu_mask` 写入 `cpu_affinity_mask`（0 表示不绑定）；线程创建时 `src/core/task_interface.c` 调用 `include/platform_pal.h::flow_pal_thread_attr_set_affinity`。`include/task_manager.h::task_manager_start_all_deps` 按依赖启动 | [`book/11_scheduler.md`](book/11_scheduler.md) |

## 第四部　录制与回放

把消息录下来，再按时间放回去。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 12 | Bag、MCAP 与 flowrec | `include/bag.h::bag_writer_write` 与 `include/bag.h::bag_reader_play`，`include/mcap_writer.h::mcap_writer_write_msg` 与 `include/mcap_reader.h::mcap_reader_next`，以及 `include/flowrec.h::flowrec_engine_process` 按配置留存话题 | [`book/05_bag_recording.md`](book/05_bag_recording.md) |

## 第五部　自动驾驶算法栈

前四部的总线、调度和回放，在这里接上算法：看见前车，再决定、规划、控制、兜底和执行。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 13 | 感知：从激光点云到目标（含其他感知源） | `modules/adas_nodes/lidar_scan.h::lidar_scan_generate` 产生点，`src/algorithms/dbscan_cluster.h::dbscan_run` 聚类。跟踪是 `src/algorithms/kalman_tracker.c` 的线性常速卡尔曼滤波（KF）：状态 `[x, y, vx, vy]`，`F` 为常速转移，`H` 只观测位置，关联用匈牙利算法。这不是扩展卡尔曼滤波（EKF）。`modules/adas_nodes/perception_fusion_node.cpp` 合并激光与双目两路 `ObstacleList` | [`book/12_lidar_tracking.md`](book/12_lidar_tracking.md) |
| 14 | 定位融合：EKF | `src/algorithms/ekf_fusion.c::ekf_fusion_predict` 按常速、常横摆角速度（CTRV）传播，不用转角和轴距。`config/pipeline_car.json` 里 slam 的 `algo` 为 `ekf_slam` 时，位姿经 `sensor/pose` 进入 `modules/adas_nodes/fusion_node.cpp`。位置更新有三种：位姿已收敛且 `cov_xx + cov_yy < 100` 时，用位姿的 x/y 调用 `src/algorithms/ekf_fusion.h::ekf_fusion_update_lidar`；已收敛但协方差不小于 100 时，这一拍不做位置更新；没有位姿或未收敛时，用 `LidarFrame` 的 x/y。GPS 的速度和航向进 `src/algorithms/ekf_fusion.h::ekf_fusion_update_gps`。`modules/adas_nodes/slam_node.cpp` 写出的位姿把 `converged` 设为 true。默认 `config/pipeline.json` 没有 slam 进程 | [`book/13_sensor_fusion.md`](book/13_sensor_fusion.md) |
| 15 | 行为决策 | 上游读 `fusion/localization`、`perception/obstacles`、`perception/tracked_objects`、`vehicle/state` 和道路话题，发布 `planning/behavior`。下游 `modules/adas_nodes/planning_node.cpp` 订阅这条行为，并订阅 `navigation/path`，发布 `planning/trajectory`。`prediction/tracks` 由 `modules/adas_nodes/prediction_node.c` 发布，订阅者是 `modules/adas_nodes/scene_assembler_node.c`；行为节点和规划节点都不订阅 | [`book/14_behavior_decision.md`](book/14_behavior_decision.md) |
| 16 | Frenet 轨迹规划 | `modules/adas_nodes/planning_coordinates.h::project_to_path` 与 `src/algorithms/frenet_bridge.h::frenet_plan` 把问题投到参考线上。速度用 S-T 图（Station-Time graph），由 `modules/adas_nodes/st_graph.h::st_graph_plan` 做动态规划。`include/piecewise_jerk_qp.h::pjqp_path_solve` 与 `include/piecewise_jerk_qp.h::pjqp_speed_solve` 有声明和实现，规划节点没有调用；实际调用的是 `include/piecewise_jerk_qp.h::pjqp_smooth_2d` | [`book/15_trajectory_planning.md`](book/15_trajectory_planning.md) |
| 17 | 跟踪控制：横向级联、LTV-MPC 与机动跟踪器 | `modules/adas_nodes/control_node.cpp` 里是纵向 PID + 横向三级级联 PD（横向速度 → ψ_des → 转向角，含曲率前馈；不是教科书 Stanley 公式）。`include/ltv_mpc.h::ltv_mpc_solve` 解的是 3 状态 1 控制的仿射 LQR（后向 Riccati，非 QP，约束为事后截断），且默认关闭。`modules/adas_nodes/maneuver_tracker.h` 的 `ManeuverTracker` 管掉头和泊车这类断开的参考线，倒挡时反馈项反号 | [`book/16_tracking_control.md`](book/16_tracking_control.md) |
| 18 | 安全包络与降级 | 让控制指令先过一遍安全包络，再发布 `control/cmd`。`modules/adas_nodes/safety_arbiter.h::safety_arbiter_apply` 仲裁规则控制与学习模型（降级 > 转向包络 0.12 rad > 规则制动 > 模型油门上限 0.85，制动取 max）。`include/degrade_ladder.h::degrade_layer_action` 是 L0~L3 粘滞阶梯，`include/health.h::health_heartbeat` 的 5 s `HEALTH_STALE` 仅上报不动作 | [`book/17_safety_envelope.md`](book/17_safety_envelope.md) |
| 19 | 执行器：PWM 与 SocketCAN | `config/pipeline_car.json` 的 actuator 加载 PWM 节点（`libactuator_pwm_node.so`），`modules/adas_nodes/pwm_map.c::pwm_map_control_cmd` 把 `control/cmd` 映成 ESC 与舵机脉宽。看门狗 3 秒。`modules/adas_nodes/actuator_node.c` 是 SocketCAN 备选后端：**零 config 引用、零测试**。注意 `pipeline_car.json:269` 的 `max_steer: 0.35` 超过执行器的 0.22 量程 | [`book/22_socketcan_actuator.md`](book/22_socketcan_actuator.md) |

第 13、14 章的写作约束见附录对应小节。

## 第六部　仿真、可视化与学习闭环

仿真提供被控世界，可视化把运行摊开，评估锁住回归，学习闭环把样本送回去。

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 20 | FlowSim 场景与世界 | 仿真只做被控对象：`modules/adas_nodes/flowsim/physics.cpp` 用前向欧拉积分自行车模型的位置、航向和速度（`modules/adas_nodes/flowsim/physics.h::step_bicycle` 与 `modules/adas_nodes/flowsim/physics.h::step_bicycle_dynamic`），再发布真值。场景由 `include/scenario_loader.h::scenario_load` 读入，路网和 NPC 在 `modules/adas_nodes/flowsim/` | [`book/18_flowsim_scenario_design.md`](book/18_flowsim_scenario_design.md) |
| 21 | 可视化与内省 | `src/flowmond.c` 与 `include/monitor_server.h::monitor_server_start` 把拓扑送给浏览器；仪表盘 JSON 经 `include/dashboard_bridge.h::dashboard_bridge_publish`（通道机制见第 07 章）；前端入口是 `tools/flowboard/js/app.js` | [`book/20_flowmond_3d_vis.md`](book/20_flowmond_3d_vis.md) |
| 22 | 验证关卡与回归评估 | `ci/evaluators/demo_evaluator.py` 与 `ci/evaluators/scenario_regression.py` 做行为回归。`ci/gates/` 有 7 个脚本做静态契约检查，其中 6 个写进 `.github/workflows/ci.yml`，`ci/gates/lane_match_schema_check.py` 没有。`tools/pipeline_check.py` 做离线管道检查 | [`book/21_demo_evaluator.md`](book/21_demo_evaluator.md) |
| 23 | 端到端学习闭环 | `modules/adas_nodes/learner_node.c` 采集，`modules/adas_nodes/inference_node.cpp` 用 `modules/adas_nodes/tiny_mlp.h::tiny_mlp_load` 与 `modules/adas_nodes/tiny_mlp.h::tiny_mlp_forward` 做影子推理，`modules/adas_nodes/model_ota_node.c` 负责换模型；训练脚本在 `tools/train_e2e/` | [`book/19_e2e_learning_loop.md`](book/19_e2e_learning_loop.md) |

---

## 以下供改写与评审使用

## 章节依赖

阅读和改写都按这个顺序：后一章可以引用前面已经出现的章。

- 00b 是后面每一章可运行示例的共同起点。
- 01 → 02 → 03 → 04。
- 05 → 06 → 07 → 08。
- 11、12 依赖 09。
- 10 → 11。
- 12 依赖 05、06、09。
- 第五部依赖 05、09、10。部内顺序是 13 → 14 → 15 → 16 → 17 → 18 → 19。18 依赖 17，19 依赖 18。
- 第六部依赖 02、12，以及整个第五部。

---

## 附录：章节—源码对照表

路径都相对于仓库根目录。00b 与 03 尚无章节文件。关键符号只列在头文件或实现里能够对上的名字。

### 00　前言

- 现文件：[`book/00_preface.md`](book/00_preface.md)
- `README.md`
- `scripts/demo.sh`
- `tools/pipeline_check.py`

### 00b　跑起来：构建、演示与 pipeline.json

- 现文件：[`book/00b_run_pipeline.md`](book/00b_run_pipeline.md)
- `build.sh`
- `CMakePresets.json`
- `CMakeLists.txt`
- `scripts/demo.sh`
- `src/flow_launcher.c`：读 pipeline，`dlopen` 加载 `NodePlugin`；多进程时 exec `flow_node_host`
- `src/flow_node_host.c`：单节点宿主，`main` 的用法是 `flow_node_host <config_path> <node_name> [duration_sec]`
- 进程表（六份，名字以文件为准）：`config/pipeline.json`、`config/pipeline_car.json`、`config/pipeline_manual.json`、`config/pipeline_sensor.json`、`config/pipeline_cortex.json`、`config/pipeline_windows.json`

### 01　用 C 造一个对象

- 现文件：[`book/01_oop_in_c.md`](book/01_oop_in_c.md)
- `include/task_interface.h`，`src/core/task_interface.c`：`TaskBase`，`TaskInterface`（`initialize` / `execute` / `cleanup`），`task_base_init`，`task_start`，`task_stop`
- `src/plugins/example_task.c`
- `src/plugins/simple_cpp_task.cpp`
- `tests/test_modules.c`

### 02　可插拔 .so 与启动器

- 现文件：[`book/02_plugin_system.md`](book/02_plugin_system.md)
- `include/node_plugin.h`，`src/core/node_plugin.c`：`NodePlugin`，`NODE_PLUGIN_API_VERSION`，`NODE_PLUGIN_SYMBOL`（`node_get_plugin`），`node_start_managed`，`node_announce_self`
- `src/flow_launcher.c`
- `src/flow_node_host.c`
- `modules/adas_nodes/manual_drive_node.c`：`node_get_plugin` 返回填好的 `NodePlugin`（`manual_drive`）
- `modules/adas_nodes/flowrec_node.c`：`NODE_PLUGIN_EXPORT` 的 `node_get_plugin` 返回填好的 `NodePlugin`（`flowrec`）。`src/plugins/example_process.c` 不是这份契约的示例，它导出的是 `get_process_interface`
- 同 00b 的六份 `config/pipeline.json`、`config/pipeline_car.json`、`config/pipeline_manual.json`、`config/pipeline_sensor.json`、`config/pipeline_cortex.json`、`config/pipeline_windows.json`

### 03　注册中心与参数系统

- 现文件：[`book/03_registry_and_params.md`](book/03_registry_and_params.md)
- `include/param_registry.h`，`src/core/param_registry.c`：`param_register_int` / `param_register_float` / `param_register_bool` / `param_register_string`。int 与 float 注册时带 min/max；`param_set_int` 与 `param_set_float` 越界则拒绝，范围内则写入 `current_value`，不看 `hot_reload`。`param_set_callback` 只把回调记到 `on_change`。`param_enable_hot_reload` 只把 `hot_reload` 设为 true。`validate_and_set` 要 `on_change` 和 `hot_reload` 都有才调用回调。这两个函数除本头文件和本 .c 外没有 C/C++ 调用点，新建参数时 `hot_reload` 为 false、`on_change` 为空，所以这条回调不会跑起来。`param_export_json` 导出，其中包含 `hot_reload` 字段。
- `include/flow_registry.h`，`src/core/flow_registry.c`：`flow_registry_register_task`，`flow_registry_register_topic`，`flow_registry_register_type`，`flow_registry_register_plugin`，`flow_registry_export_json`。宏 `FLOW_REGISTRY_DECLARE_PLUGIN` 展开后只调用 `flow_registry_register_plugin(名字, NULL, NULL, NULL)`，宏参数里的版本和描述没有写入注册表。
- `include/topic_registry.h`：编译期话题名常量，例如 `TOPIC_SENSOR_LIDAR`（`sensor/lidar`）、`TOPIC_PERCEPTION_OBSTACLES`、`TOPIC_CONTROL_CMD`
- `include/config_manager.h`，`src/core/config_manager.c`：`config_load`，`config_save`，`config_free`
- `include/param_bridge.h`，`src/core/param_bridge.c`：`param_bridge_server_start`，`param_bridge_server_stop`，`param_bridge_client_request`。默认套接字 `PARAM_BRIDGE_DEFAULT_SOCK`（`/tmp/flow_param.sock`），可用 `FLOW_PARAM_SOCK` 覆盖。线协议是 `LIST` / `GET` / `SET`
- `src/flowctl.c`：`flowctl param list`、`flowctl param get <name>`、`flowctl param set <name> <value>`。三条都经 `param_bridge_client_request` 发 `LIST` / `GET` / `SET`，不调用 `param_set_callback` 或 `param_enable_hot_reload`。`SET` 由 `param_bridge` 按类型转成 `param_set_int` / `param_set_float` / `param_set_bool` / `param_set_string`。节点下一拍用 `param_get_int` / `param_get_float` 读到新值。

### 04　状态机

- 现文件：[`book/08_state_machine.md`](book/08_state_machine.md)
- `include/state_machine.h`，`src/core/state_machine.c`：`ReflectiveStateMachine`，`statem_init`，`statem_send_event_ex`，`statem_set_guard`，`statem_add_transition`，`statem_dump_table`

### 05　消息总线

- 现文件：[`book/03_message_bus.md`](book/03_message_bus.md)
- `include/message_bus.h`，`src/core/message_bus.c`：`Message`，`MessageBus`，`message_bus_create`，`message_bus_publish`，`message_bus_subscribe`，`message_bus_set_topic_qos`
- `src/bus_demo.c`

这一章的现标题写着「15 个节点」。节点个数以 `config/pipeline.json` 的 `processes` 数组为准。改写时按该数组重数，标题里的 15 只是旧断言。

### 06　类型 ID、IDL 与序列化

- 现文件：[`book/07_serializer.md`](book/07_serializer.md)
- `include/serializer.h`，`src/core/serializer.c`：`fnv1a_hash`，`serializer_register_type`，`SerializeFunc`，`DeserializeFunc`，`_msg_cast_impl`，`msg_cast`
- `include/msg_schema.h`，`src/core/msg_schema.c`：`msg_schema_register`，`msg_schema_check`，`MSG_REGISTER_TYPE`，`MSG_CHECK_SIZE`
- `include/fp_env.h`：`fp_env_init`（置位 x86 MXCSR FTZ/DAZ 与 ARM FPCR.FZ 硬件防御）
- `msg/adas_msgs.msg`：单一黄金信源（SSOT）
- `tools/msg_codegen.py`：AST 解析、拓扑排序与版本哈希追踪
- `ci/gates/msg_layout_check.py`：线格式（Wire）与内存结构体（Struct）物理对齐门禁

### 07　共享内存 IPC

- 现文件：[`book/04_ipc_channel.md`](book/04_ipc_channel.md)
- `include/ipc_channel.h`，`src/core/ipc_channel.c`：`IpcChannel`，`ipc_channel_open`，`ipc_channel_publish`，`ipc_channel_subscribe`，`ipc_channel_start`
- `include/dashboard_bridge.h`，`src/core/dashboard_bridge.c`：`dashboard_bridge_publisher_open`，`dashboard_bridge_publish`，`dashboard_bridge_subscriber_open`
- `src/ipc_demo.c`

### 08　统一传输与服务发现

- 现文件：[`book/09_discovery.md`](book/09_discovery.md)
- `include/transport.h`，`src/core/transport.c`：`Transport`，`transport_create`，`transport_publish`，`transport_subscribe`，`transport_publish_loaned`，`topic_to_ipc_name`
- `include/discovery.h`，`src/core/discovery.c`：`DiscoveryManager`，`discovery_create`，`discovery_advertise`，`discovery_wait_for_deps`，`discovery_create_ipc_channels`；`DISC_MULTICAST_GROUP`，`DISC_MULTICAST_PORT`，`DISC_MSG_HELLO`，`DISC_MSG_HEARTBEAT`，`DISC_MSG_GOODBYE`，`DISC_MSG_QUERY`
- `include/network_transport.h`，`src/cpp/network_transport.cpp`：`net_transport_start`，`net_transport_connect`，`net_transport_bridge_topic`，`serialize_frame`，`decode_frame`
- `src/benchmark_tcp.c`

### 09　时钟服务

- 现文件：[`book/06_clock_service.md`](book/06_clock_service.md)
- `include/clock_service.h`，`src/core/clock_service.c`：`clock_now_us`，`clock_now_monotonic_wall_us`，`clock_now_realtime_us`，`clock_set_sim_mode`，`clock_set_sim_time`，`clock_advance_us`，`clock_set_step_us`
- `include/platform_pal.h`，`src/core/platform_pal.c`：`flow_pal_clock_gettime_monotonic`，`flow_pal_clock_gettime_realtime`
- `src/core/message_bus.c`，`src/core/ipc_channel.c`：绑定 `clock_now_monotonic_wall_us` 杜绝仿真延迟为 0

### 10　C++20 协程

- 现文件：[`book/10_coroutine.md`](book/10_coroutine.md)
- `include/coroutine_task.h`：`CoroutineTask`，`node_pump`
- `src/plugins/flowcoro_task.cpp`
- `src/coro_bus_demo.cpp`
- `src/rt_heartbeat_demo.cpp`
- `src/benchmark_coro.cpp`
- `tests/coro_correctness_test.cpp`
- 总线上 `co_await … recv_any_for` 的调用点：`modules/adas_nodes/fusion_node.cpp`（`fusion_bridge`）、`modules/adas_nodes/control_node.cpp`（`ctrl_bridge`）、`modules/adas_nodes/planning_node.cpp`（`plan_bridge`）、`modules/adas_nodes/perception_fusion_node.cpp`（`pf_bridge`）。别的节点还有 `co_await sleep_us`，那不是等总线消息

### 11　调度器与绑核

- 现文件：[`book/11_scheduler.md`](book/11_scheduler.md)
- `include/scheduler.h`，`src/core/scheduler.c`：`Scheduler`，`scheduler_create`，`scheduler_register_task`，`scheduler_run_loop`，`RateControl`，`rate_control_init`。`scheduler_set_params` 的 `cpu_mask` 写入 `cpu_affinity_mask`（0 表示不绑定）
- `include/task_interface.h`，`src/core/task_interface.c`：配置字段 `cpu_affinity_mask`。掩码非 0 时，创建线程调用 `include/platform_pal.h` 的 `flow_pal_thread_attr_set_affinity`，里面是 `pthread_attr_setaffinity_np`
- `src/cpp/scheduler_cpp.cpp`
- `include/task_manager.h`，`src/core/task_manager.c`：`task_manager_create`，`task_manager_register`，`task_manager_start_all_deps`

### 12　Bag、MCAP 与 flowrec

- 现文件：[`book/05_bag_recording.md`](book/05_bag_recording.md)
- `include/bag.h`，`src/core/bag.c`：`bag_writer_open`，`bag_writer_write`，`bag_reader_open`，`bag_reader_play`
- `include/mcap_writer.h`，`src/core/mcap_writer.c`：`mcap_writer_open`，`mcap_writer_write_msg`，`mcap_writer_close`
- `include/mcap_reader.h`，`src/core/mcap_reader.c`：`mcap_reader_open`，`mcap_reader_next`，`mcap_reader_close`
- `src/mcap_replay.c`
- `include/flowrec.h`，`src/core/flowrec.c`：`flowrec_engine_create_from_json`，`flowrec_engine_process`，`flowrec_engine_tick`
- `modules/adas_nodes/flowrec_node.c`
- `tools/bag_check.c`
- `src/bag_demo.c`

### 13　感知：从激光点云到目标（含其他感知源）

- 现文件：[`book/12_lidar_tracking.md`](book/12_lidar_tracking.md)
- `modules/adas_nodes/lidar_scan.h`，`modules/adas_nodes/lidar_scan.c`：`LidarScanPlan`，`lidar_scan_generate`，`lidar_scan_plan_sanitize`
- `modules/adas_nodes/lidar_contract.h`：`lidar_point_cloud_validate`，`lidar_point_cloud_capacity`
- `modules/adas_nodes/lidar_driver_node.c`
- `src/algorithms/dbscan_cluster.h`，`src/algorithms/dbscan_cluster.c`：`dbscan_init`，`dbscan_run`，`dbscan_cluster_count`
- `src/algorithms/kalman_tracker.h`，`src/algorithms/kalman_tracker.c`：线性常速卡尔曼滤波（KF），不是扩展卡尔曼滤波（EKF）。`KTRACKER_STATE_DIM` 为 4，状态 `[x, y, vx, vy]`。`kf_predict` 的转移矩阵 `F` 是 `x'=x+vx·dt`、`y'=y+vy·dt`、速度保持；`kf_update` 的 `H` 只取位置 `[zx, zy]`。`ktracker_predict`，`ktracker_associate_and_update`，关联实现是 `hungarian_solve`
- `modules/adas_nodes/object_tracker_node.c`：订阅 `perception/obstacles`，发布 `perception/tracked_objects`，调用上面的 `ktracker_*`
- `modules/adas_nodes/perception_fusion_node.cpp`：目标级融合，不是定位 EKF。默认订阅 `perception/obstacles_lidar` 与 `perception/obstacles_stereo`（两路 `ObstacleList`）。`fuse_obstacles` 按距离去重合并，`associate_and_track` 做最近邻跨帧关联，写出持久 id 和差分速度，发布 `perception/obstacles`。`config/pipeline_car.json` 文件头写明该节点默认关闭；同时使用激光和双目时，才把两路输出改到上述话题并打开融合

其他感知源（并入本章，不另开编号）：

- `modules/adas_nodes/lane_detection_node.c`：沙箱。输入 `road/geometry` 与 `vehicle/state`，输出 `perception/lanes`。车道线由道路几何合成。Canny / Hough 只存在于注释。
- `modules/adas_nodes/bev_post.h`，`modules/adas_nodes/bev_post.c`：`BevPostDet`，`bev_post_to_obstacle_list`
- `modules/adas_nodes/bev_pre.h`，`modules/adas_nodes/bev_pre.c`：`bev_pre_rasterize`，`bev_pre_config_default`
- `modules/adas_nodes/bev_onnx_backend.h`，`modules/adas_nodes/bev_onnx_backend.cpp`：`bev_onnx_backend_load`，`bev_onnx_backend_forward`
- `modules/adas_nodes/bev_detection_node.cpp`：影子模式，发布 `bev/obstacles`，调用上面的 pre / onnx / `bev_post_to_obstacle_list`
- `modules/adas_nodes/traffic_light_recognition_node.c`：沙箱，转发 `road/traffic_lights`
- `modules/adas_nodes/stereo_vision_node.c`：`sensor/stereo` 深度反投影后聚类
- `modules/adas_nodes/perception_node.cpp`：`perception/obstacles`

写作约束：不单开视觉章。用一节「其他感知源」写明下面两件事。

- `modules/adas_nodes/lane_detection_node.c` 是沙箱。它订阅 `road/geometry`，用场景里的道路几何合成车道线。Canny 边缘检测和 Hough 变换只写在文件头的注释里（注释称之为尚未实现的 HAVE_CV 版本），这个 .c 文件里没有这条图像处理路径。
- `modules/adas_nodes/bev_post.h` 和 `modules/adas_nodes/bev_post.c` 只做协议映射。`bev_post_to_obstacle_list` 把已经解码好的 `BevPostDet` 填进 `ObstacleList`。网络输出怎么解码不在这两个文件里。

同一节里的相邻源码按各自文件的实际行为来写。`modules/adas_nodes/traffic_light_recognition_node.c` 也是沙箱，把 `road/traffic_lights` 转成 `perception/traffic_lights`，注释里的相机检测没有实现。`modules/adas_nodes/bev_detection_node.cpp` 是影子节点，发布 `bev/obstacles`；栅格用 `bev_pre_rasterize`，可选的 ONNX 前向用 `bev_onnx_backend_forward`，没有模型时走真值直通，填 `ObstacleList` 时调用 `bev_post_to_obstacle_list`。`modules/adas_nodes/stereo_vision_node.c` 把 `sensor/stereo` 的深度反投影后做 DBSCAN。`modules/adas_nodes/perception_node.cpp` 在 ground_truth 与 `sensor/lidar_points` 两种模式下产出 `perception/obstacles`。

### 14　定位融合：EKF

- 现文件：[`book/13_sensor_fusion.md`](book/13_sensor_fusion.md)
- `modules/adas_nodes/ekf_slam.h`，`modules/adas_nodes/ekf_slam.c`：5 维状态 `[x, y, heading, v, omega]`。`ekf_slam_predict` 用 IMU 的 `accel_x`、`gyro_z` 做运动学预测，并传播雅可比 `F`。`ekf_slam_update` 观测位置和航向，`ekf_slam_update_pos` 只观测位置。`ekf_slam_get_pose` 读出 x/y/heading 和 `cov_xx` / `cov_yy` / `cov_hh`
- `modules/adas_nodes/slam_node.cpp`：订阅 `sensor/lidar`、`sensor/imu`，发布 `sensor/pose`（`Pose2D`）。两处写出位姿都把 `converged` 设为 true，所以 `pipeline_car` 上只要位姿还在到，融合节点进不了上面的（c）。默认 `algo` 是 `dead_reckon`，不调用 `ekf_slam.c`。`algo` 为 `ekf_slam` 时走 `slam_update_ekf_slam`：预测用最近一帧 IMU，激光新鲜时做位置更新，位移大于 `heading_obs_min_disp` 时用 `atan2(Δy, Δx)` 作为航向观测。`config/pipeline_car.json` 的 slam 进程把 `algo` 设为 `ekf_slam`。默认 `config/pipeline.json` 没有这个进程，`allow_hung_subs` 含 `sensor/pose`
- `src/algorithms/ekf_fusion.h`，`src/algorithms/ekf_fusion.c`：`EkfFusion` 状态 `[x, y, v, heading, yaw_rate]`。`ekf_fusion_predict` 是 CTRV：速度保持，横摆角速度保持，`x` 加上 `v·cos(heading)·dt`，没有转角，也没有轴距。头文件 Prediction 注释写着 “bicycle kinematic model”，和这段实现不一致，改写时按实现写。`ekf_fusion_update_lidar` 只更新位置 x/y。`ekf_fusion_update_gps` 的实参是速度和航向（`z_v`，`z_heading`），不是经纬度。头文件用法注释里的四参数形式（位置、速度、航向）和声明对不上
- `modules/adas_nodes/fusion_node.cpp`：订阅 `sensor/lidar`、`sensor/gps`、`sensor/pose`，发布 `fusion/localization`。先 `ekf_fusion_predict`。位置更新有三种：（a）`Pose2D` 已 `converged` 且 `cov_xx + cov_yy < 100`，把位姿的 x/y 送进 `ekf_fusion_update_lidar`；（b）已 `converged` 但协方差不小于 100，这一拍不做位置更新，也不用 `LidarFrame`；（c）没有位姿或未收敛，把 `LidarFrame` 的 x/y 送进同一个函数。有 GPS 时再 `ekf_fusion_update_gps(speed_mps, heading_deg 转弧度)`。`ekf_slam.c` 没有调用 `ekf_fusion.c`；车端这条链路是 `sensor/pose` 上的 `Pose2D`。文件头写「无 GPS 时 SLAM 位姿全维更新」，函数体只更新了位置
- `modules/adas_nodes/slam_math.h`，`modules/adas_nodes/slam_math.c`：`slam_wrap_pi`
- `modules/adas_nodes/gps_driver_node.c`
- `modules/adas_nodes/imu_driver_node.c`
- `src/algorithms/nmea_parser.h`，`src/algorithms/nmea_parser.c`：`nmea_parser_init`，`nmea_parse_line`

写作约束：两条 EKF 不在同一个文件里互相调用。`slam_node.cpp` 仅当 `algo` 为 `ekf_slam` 时跑 `ekf_slam_predict`（IMU 的 `accel_x`、`gyro_z`）和 `ekf_slam_update` / `ekf_slam_update_pos`，再把 `Pose2D` 发到 `sensor/pose`。融合节点的位置更新按上面三种写，不要写成「否则用 LidarFrame」。预测按 CTRV 写，不要按头文件里那句 bicycle 注释写。`slam_node` 始终把 `converged` 设为 true。

### 15　行为决策

- 现文件：[`book/14_behavior_decision.md`](book/14_behavior_decision.md)
- `modules/adas_nodes/behavior_planner_node.cpp`：订阅 `fusion/localization`、`perception/tracked_objects`、`perception/obstacles`、`vehicle/state`、`road/geometry`、`road/traffic_lights`、`road/ref_path`、场景帧，发布 `planning/behavior`。不订阅 `prediction/tracks`
- `modules/adas_nodes/planning_node.cpp`：订阅 `planning/behavior` 与 `navigation/path`，发布 `planning/trajectory`。不订阅 `prediction/tracks`
- `modules/adas_nodes/navigation_node.c`：发布 `navigation/path`
- `modules/adas_nodes/prediction_node.c`：输出 `prediction/tracks`
- `modules/adas_nodes/scene_assembler_node.c`：订阅 `prediction/tracks`

### 16　Frenet 轨迹规划

- 现文件：[`book/15_trajectory_planning.md`](book/15_trajectory_planning.md)
- `modules/adas_nodes/planning_coordinates.h`：`planning_coord::Projection`，`project_to_path`，`lane_center_d`
- `src/algorithms/frenet_bridge.h`，`src/algorithms/frenet_bridge.cpp`：`frenet_set_reference_path`，`frenet_plan`
- `modules/adas_nodes/st_graph.h`，`modules/adas_nodes/st_graph.c`：`st_graph_plan`。`modules/adas_nodes/planning_node.cpp` 用它做速度规划（动态规划）。S-T 图是 Station-Time graph
- `include/piecewise_jerk_qp.h`，`src/algorithms/piecewise_jerk_qp.c`：`pjqp_smooth_1d`，`pjqp_smooth_2d`，`pjqp_path_solve`，`pjqp_speed_solve`。`planning_node.cpp` 包含这份头文件，调用的是 `pjqp_smooth_2d`（内部再调 `pjqp_smooth_1d`）。`pjqp_path_solve` 与 `pjqp_speed_solve` 只有声明和定义，仓库里没有调用点
- `modules/adas_nodes/traj_safety.h`，`modules/adas_nodes/traj_safety.c`：`traj_point_test_hits`

### 17　跟踪控制：横向级联、LTV-MPC 与机动跟踪器

- 现文件：[`book/16_tracking_control.md`](book/16_tracking_control.md)
- `modules/adas_nodes/control_node.cpp`：纵向 PID（`:854-920`）+ 横向级联 PD（`:999-1061`）。横向不是教科书 Stanley 公式，而是「横向速度 PD → ψ_des → 转向角」三级级联，另有曲率前馈与运动学前馈；`steer_limit_for_speed`（`:191`）按 $a_{lat} = L a_{max}/v^2$ 给出自适应转向包络
- `modules/adas_nodes/maneuver_tracker.h`：`ManeuverTracker`，`ManeuverTrackerParams`，`TrackPoint`，`ManeuverResult`。header-only 弧长跟踪器，`tick`（`:228`）按弧长推进轨迹，倒挡时反馈项反号（`:283`）；无 `ManeuverType` 枚举，机动类型由轨迹数据（$v<0$ 或 $|\kappa|>0.12$）判定
- `include/ltv_mpc.h`，`src/algorithms/ltv_mpc.c`：`ltv_mpc_set_reference`，`ltv_mpc_set_state`，`ltv_mpc_solve`。3 状态 1 控制的离散仿射 LQR（后向 Riccati 递推，$O(N)$），**不是 QP**；`max_steer`/`max_dsteer` 是事后截断而非优化约束。`use_ltv_mpc` 默认 0，无 `config/*.json` 启用
- `msg/adas_msgs.msg`：`ControlRaw`（`:189-202`）。转向角为 rad，油门/刹车为 [0,1] 归一化
- `modules/adas_nodes/waypoint_follower_node.c`：Pure Pursuit 航点跟随，输出 `planning/trajectory`（RC 小车路径，与 `planning_node` 二选一）
- `docs/LTV_MPC_DESIGN.md`

### 18　安全包络与降级

- 现文件：[`book/17_safety_envelope.md`](book/17_safety_envelope.md)
- `modules/adas_nodes/safety_control_node.cpp`：订阅原始控制、定位与障碍物，发布最终 `control/cmd`。声明的输入是 `control/raw_cmd`、`fusion/localization`、`perception/obstacles`。运行时的队列桥还收 `inference/raw_cmd`。没有 `vehicle/state`。5 ms（200 Hz）轮询节拍，看门狗独立于消息流
- `modules/adas_nodes/safety_arbiter.h`，`modules/adas_nodes/safety_arbiter.c`：`safety_arbiter_apply`，60 行。优先级：降级/模型不新鲜（P1，不计入 `intervened`）> 转向包络 `0.12` rad > 规则制动 `0.10` > 模型油门上限 `0.85`。制动取 `fmax` 而非规则独赢；规则不制动时模型 `brake` 原样透传
- `include/degrade_ladder.h`，`src/core/degrade_ladder.c`：`degrade_set_level`，`degrade_set_level_at`（粘滞，单调不减），`degrade_layer_action`（**不接收参数**），`degrade_supervisor_tick`（500 ms 触发 + 150 ms 去抖 / 2000 ms 严重 / 3000 ms 恢复），`degrade_clear`。`l1_speed_limit` 只在 L2 写 3.0、L3 写 0.0，**L1 不限速**
- `include/health.h`，`src/core/health.c`：`health_init`，`health_register`，`health_heartbeat`。5 s `HEALTH_STALE` **仅上报不动作**，与 degrade_ladder 的心跳系统无关
- `include/safety_evidence.h`，`src/core/safety_evidence.c`：`safety_evidence_to_json`，发布 `safety/evidence`
- `include/safety_fault_injection.h`，`src/core/safety_fault_injection.c`：`safety_fault_injection_init`，`safety_fault_injection_start`，`safety_raw_command_timeout_expired`（`moving && last>0 && Δt>2s`）
- `msg/adas_msgs.msg`：`ControlRaw`（输入，12 字段/59 B）与 **`ControlCmd`（输出，8 字段/20 B）** 是两个不同结构体
- 测试：`tests/test_adas_nodes_logic.c`（5 个仲裁用例，编译生产同一份 `safety_arbiter.c`）、`tests/test_safety_fault_evidence.c`（降级 + 超时）。**TTC / `apply_safety` / NaN 兜底全部无测试**（都在 `.cpp` 匿名命名空间里）

### 19　执行器：PWM 与 SocketCAN

- 现文件：[`book/22_socketcan_actuator.md`](book/22_socketcan_actuator.md)
- `modules/adas_nodes/pwm_map.h`，`modules/adas_nodes/pwm_map.c`：`pwm_map_control_cmd` 把 `throttle` / `brake` / `steering_rad` 映成 ESC 与舵机脉宽（μs），钳在 `PWM_MIN_US`（1000）与 `PWM_MAX_US`（2000）之间。**全文件 33 行**，三级优先：`e_stop` → `brake > 0.01`（`esc = 1500 − brake·scale`，反打）→ `throttle`（`esc = 1500 + throttle·scale`）。**`e_stop` 只覆盖 ESC，舵机保持原角度**。转向走 `steer_norm = steering_rad / PWM_MAX_STEER_RAD(0.22)`，独立于 e_stop
- **映射层测试**：`tests/test_adas_nodes_logic.c` 的 10 个 `test_pwm_*`（`:295-389`），`CMakeLists.txt:1110` 把**生产同一份** `pwm_map.c` 编进测试目标（`ctest adas_nodes_logic_tests`）。**CAN 后端零测试**；e_stop 用例只断言 `esc`，**不断言 `steer`**
- `modules/adas_nodes/actuator_pwm_node.c`（571 行，`modules/adas_nodes/CMakeLists.txt:733`）：订阅 `control/cmd`，不发布。`pca9685_set_freq` 用 `prescale = 25e6/(4096·f) − 1`（50 Hz → 121），`pca9685_set_pulse_us` 用 `tick = pulse_us/period_us × 4096`（1500 μs → 307）。上电与清理都强制回中。`watchdog_timeout_s` 默认 3（`time(NULL)` 1 s 粒度）：超时则 **ESC + 舵机双双回中**。`gpio_set_pulse_us` 硬编码 `pwmchip0` 且把 GPIO 号当 pwmchip 子索引，实际不可用
- `modules/adas_nodes/actuator_node.c`（523 行，SocketCAN，**零 config 引用**）：`can_open` / `can_send` / `encode_throttle_frame` / `encode_steering_frame` / `actuator_execute`。三报文 0x100（油门刹车 gear e_stop，DLC 8）、0x101（转向 + seq，DLC 4）、0x102（10 Hz 状态心跳，DLC 8）。`throttle_scale`/`steering_scale` 默认 1000。**单向降级到 dry-run 永不重试**；`watchdog_timeout_s` 写死 3 不可配；启动前 `last_cmd_time == 0` 使看门狗完全惰性。**零测试**
- **跨层不匹配**：`config/pipeline_car.json:269` 的 `max_steer: 0.35` 超过执行器 `PWM_MAX_STEER_RAD = 0.22`，0.22~0.35 rad 这段授权被 `pwm_map.c:27` 静默截断。同一物理量在四处独立出现（msg 注释、`pwm_map.h:21`、`actuator_node.c:235` 的局部 `0.22f`、`safety_control_node.cpp:77`）
- `src/algorithms/serial_port.c`：`serial_write`（`:163`）**全仓库零调用者**。`serial_open` 只被 gps/imu/激光雷达三个**只读**驱动使用——执行器不碰串口
- `config/pipeline_car.json`

### 20　FlowSim 场景与世界

- 现文件：[`book/18_flowsim_scenario_design.md`](book/18_flowsim_scenario_design.md)
- `modules/adas_nodes/flowsim_node.cpp`
- `modules/adas_nodes/flowsim/physics.h`，`modules/adas_nodes/flowsim/physics.cpp`：前向欧拉，积分自行车模型的位置、航向和速度。`step_bicycle`，`step_bicycle_dynamic`，`step_pedestrian`。`flowsim_node.cpp` 传入的步长是 `FLOWSIM_DT_SEC`。`physics.cpp` 文件头写 dt=0.05s（20Hz），和 `flowsim_time.h` 的 60 Hz 不一致，改写时以调用点为准
- `modules/adas_nodes/flowsim/flowsim_time.h`：`FLOWSIM_FREQUENCY_HZ`，`FLOWSIM_DT_SEC`
- `modules/adas_nodes/flowsim/npc_ai.h`，`modules/adas_nodes/flowsim/npc_ai.cpp`：`step_npc_vehicle`，`step_npc_pedestrian`
- `modules/adas_nodes/flowsim/route.h`，`modules/adas_nodes/flowsim/route.cpp`：`Route`，`build`，`sample_pose`
- `modules/adas_nodes/flowsim/road_network.h`，`modules/adas_nodes/flowsim/road_network.cpp`：`FlowRoadNetwork`，`load`，`frenet_to_world`
- `modules/adas_nodes/flowsim/collision.h`，`modules/adas_nodes/flowsim/collision.cpp`：`detect_collisions`，`apply_guardrail`
- `modules/adas_nodes/flowsim/scene_events.h`，`modules/adas_nodes/flowsim/scene_events.cpp`：`tick_traffic_lights`，`tick_etc_gates`
- `modules/adas_nodes/flowsim/scene_pub.h`，`modules/adas_nodes/flowsim/scene_pub.cpp`：`publish_scene_frame`
- `modules/adas_nodes/flowsim/sim_digest.h`，`modules/adas_nodes/flowsim/sim_digest.cpp`：`build_static_digest`
- `modules/adas_nodes/flowsim/entity.h`：`Entity`
- `modules/adas_nodes/flowsim/building.h`：`load_buildings`，`obb_hits_building`
- `modules/adas_nodes/flowsim/lane_frenet.h`：`lane_center_t`
- `modules/adas_nodes/flowsim/lane_match_helpers.h`：`flowsim_lm_curvature_3pt`
- `modules/adas_nodes/flowsim/road_position.h`：`RoadPosition`
- `modules/adas_nodes/flowsim/vehicle_lights.h`：`VehicleLights`
- `include/scenario_loader.h`，`src/core/scenario_loader.c`：`scenario_load`，`scenario_free`
- `scenarios/`

### 21　可视化与内省

- 现文件：[`book/20_flowmond_3d_vis.md`](book/20_flowmond_3d_vis.md)
- `modules/adas_nodes/flowmond_node.cpp`
- `src/flowmond.c`
- `include/monitor_server.h`，`src/core/monitor_server.c`：`monitor_server_create`，`monitor_server_start`，`monitor_server_stop`
- `include/dashboard_bridge.h`，`src/core/dashboard_bridge.c`：`dashboard_bridge_publish`
- `tools/flowboard/js/app.js`

### 22　验证关卡与回归评估

- 现文件：[`book/21_demo_evaluator.md`](book/21_demo_evaluator.md)
- `ci/evaluators/demo_evaluator.py`
- `ci/evaluators/scenario_regression.py`
- `ci/gates/` 共 7 个脚本：`ci/gates/topic_contract_check.py`、`ci/gates/sensor_wiring_check.py`、`ci/gates/zombie_ban_check.py`、`ci/gates/plugin_symbol_check.py`、`ci/gates/msg_layout_check.py`、`ci/gates/lanelet_consistency_check.py`、`ci/gates/lane_match_schema_check.py`。前 6 个写在 `.github/workflows/ci.yml` 里。`ci/gates/lane_match_schema_check.py` 没有接进 `ci.yml`
- `tools/pipeline_check.py`
- `include/auto_tuner.h`，`src/core/auto_tuner.c`：`auto_tuner_init`，`auto_tuner_register`，`auto_tuner_tick`
- `tools/auto_tune_mpc.py`

### 23　端到端学习闭环

- 现文件：[`book/19_e2e_learning_loop.md`](book/19_e2e_learning_loop.md)
- `tools/train_e2e/`：入口包括 `tools/train_e2e/train.py`、`tools/train_e2e/torch_train.py`、`tools/train_e2e/temporal_train.py`
- `modules/adas_nodes/learner_node.c`
- `modules/adas_nodes/inference_node.cpp`
- `modules/adas_nodes/tiny_mlp.h`：`tiny_mlp_load`，`tiny_mlp_forward`
- `modules/adas_nodes/model_ota_node.c`
- `tools/learning_loop.py`

---

## 附录：现有章节中已失效的源码引用

下面这些名字出现在**现有章节正文**里，但仓库里没有对应文件，或者路径写错了。评审对照现文件核对即可。本索引的源码对照表使用右列的真实路径。

| 现文件（新编号） | 正文里的引用 | 仓库里的实际情况 |
|---|---|---|
| [`book/02_plugin_system.md`](book/02_plugin_system.md)（新 02） | `src/core/process_manager.c` | 没有这个文件。加载与启动看 `src/flow_launcher.c`、`src/flow_node_host.c`、`src/core/node_plugin.c` |
| 同上 | `src/launcher.c` | 没有这个文件。启动器是 `src/flow_launcher.c` |
| 同上 | `modules/adas_nodes/example_filter_node.c` | 没有这个文件。`src/plugins/example_process.c` 包含 `include/process_interface.h`，导出 `get_process_interface`，没有 `node_get_plugin`。现成的 `NodePlugin` 入口是 `modules/adas_nodes/manual_drive_node.c` 与 `modules/adas_nodes/flowrec_node.c` 的 `node_get_plugin` |
| [`book/10_coroutine.md`](book/10_coroutine.md)（新 10） | `modules/adas_nodes/coro_fusion_node.cpp` | 没有这个文件。协程融合节点是 `modules/adas_nodes/fusion_node.cpp` |
| [`book/16_tracking_control.md`](book/16_tracking_control.md)（新 17，已修正） | `include/maneuver_tracker.h` | 旧稿路径有误，头文件在 `modules/adas_nodes/maneuver_tracker.h`；新稿已改正 |
| 同上（已修正） | `src/core/ltv_mpc.c` | 旧稿路径有误，实现是 `src/algorithms/ltv_mpc.c`，声明在 `include/ltv_mpc.h`；新稿已改正 |
| 同上（已修正） | 旧稿称 MPC 状态为 4 维、时域 `N_p=10~20`、用「OSQP 或内点法」求解 | 旧稿与源码不符：实为 3 状态 `[e_y, e_psi, delta]`、控制 `[ddelta]`，时域 60 步 × 0.025 s = 1.5 s，且**无任何 QP 求解器**，是后向 Riccati 递推的无约束仿射 LQR，约束为事后截断；新稿已逐条澄清 |
| [`book/17_safety_envelope.md`](book/17_safety_envelope.md)（新 18，已重写） | `is_obstacle_in_collision_corridor()`、`compute_radial_distance_to_arc()` | 旧稿凭空捏造的函数，全仓库不存在。真实的横向穿越守卫是 `safety_control_node.cpp:366` 的 `nearest_vehicle_lateral_cross_risk`，是车体系矩形门限，无弧线几何 |
| 同上（已重写） | 旧稿 TTC 分级阶梯 3.0/2.0/1.0 s、预充液压、0.3g / −1.0g | 全部不存在。真实阈值是 2.5 s 触发 / 1.5 s 降级 / 1.0 s 硬 AEB，`brake` 是 [0,1] 归一化量而非 g；无预充液逻辑 |
| 同上（已重写） | 旧稿 `safety/cmd` 话题、`actuator/cmd` 话题、`safety_override_active`、`apply_emergency_brake()`、规划 200 ms 心跳看门狗 | 均为虚构。真实输出是 `control/cmd`（20 B 的 `ControlCmd`）；看门狗是 `safety_raw_command_timeout_expired`（`moving && Δt > 2s`），监控 `control/raw_cmd` 而非规划指令 |
| 同上（已重写） | 旧稿完全缺失 `safety_arbiter_apply` 与 `degrade_ladder` | 新稿补全了规则/模型仲裁的完整优先级链、P1 不计入 `intervened` 的语义设计，以及 L0~L3 粘滞阶梯与 500/150/2000/3000 ms 四个时间常数 |
| [`book/22_socketcan_actuator.md`](book/22_socketcan_actuator.md)（新 19，已重写） | 旧稿的 `send_can_frame()`、`set_servo_pulse()`、`pca9685_set_pwm()` | 三个函数全部不存在。真实实现是 `actuator_node.c` 的 `can_open`/`can_send`/`encode_throttle_frame`/`encode_steering_frame`，以及 `actuator_pwm_node.c` 的 `pca9685_set_pulse_us()`（参数是 `int pulse_us` 微秒，不是 `float normalized_val`） |
| 同上（已重写） | 旧稿称油门与转向打包在同一个 8 字节帧的 `[0-3]`；`int16_t` 编码油门 | 与系统里任何 ID 都不匹配。真实是 0x100（DLC 8，throttle/brake/gear/e_stop）与 0x101（DLC 4，steering/seq）两个独立报文；油门是 `uint16_t` 且先钳位到 [0,1] |
| 同上（已重写） | 旧稿的 `config/pipeline_car.json` 片段：`car_real_hardware_pipeline` + `libactuator_node.so` + `can_throttle_id: 256` | 错三处：没有名为 `car_real_hardware_pipeline` 的配置；没有 config 引用 `libactuator_node.so`（CAN 后端零引用）；配置 schema 用 `library_path` 不是 `library`。真实配置是 `pipeline_car.json:272-278` 的 `libactuator_pwm_node.so` |
| 同上（已重写） | 旧稿暗示执行器经串口下发指令 | **执行器不碰串口**。`serial_port.c:163` 的 `serial_write()` 全仓库零调用者；`serial_open` 只被 gps/imu/激光雷达三个**只读**驱动使用 |
| 同上（已重写） | 旧稿完全缺失软件看门狗 | 新稿补全了两个后端各一份的 3 s 看门狗（`actuator_pwm_node.c:330-352` 与 `actuator_node.c:302-322`），以及三处不一致：CAN 后端启动前 `last_cmd_time == 0` 使看门狗惰性、CAN 端 `watchdog_timeout_s` 不可配、PWM 端健康检查另用硬编码 5 s |
| [`book/21_demo_evaluator.md`](book/21_demo_evaluator.md)（新 22） | `tools/demo_evaluator.py` | 没有这个路径。评估器在 `ci/evaluators/demo_evaluator.py` |
| 同上 | `tools/param_sweep.py` | 没有这个文件，仓库里也没有替代脚本 |
| 同上 | `scenarios/zhongkai_road_full.json` | 没有这个文件 |
| [`book/18_flowsim_scenario_design.md`](book/18_flowsim_scenario_design.md)（新 20） | `scenarios/city_to_highway_full.json` | 没有这个文件 |
| [`book/19_e2e_learning_loop.md`](book/19_e2e_learning_loop.md)（新 23） | `scenarios/city_to_highway_full.json`，`scenarios/zhongkai_road_full.json` | 没有这两个文件 |
| [`book/15_trajectory_planning.md`](book/15_trajectory_planning.md)（新 16） | （正文没有点名任何源码文件） | 对照表见本页第 16 章：`modules/adas_nodes/planning_coordinates.h`、`src/algorithms/frenet_bridge.cpp`、`modules/adas_nodes/st_graph.c`、`include/piecewise_jerk_qp.h`、`src/algorithms/piecewise_jerk_qp.c`、`modules/adas_nodes/traj_safety.h` |
| [`book/03_message_bus.md`](book/03_message_bus.md)（新 05） | 标题「15 个节点」 | 这是标题里的断言，不是从 `config/pipeline.json` 的 `processes` 数出来的。改写时按该数组计数，不要照抄 15 |
