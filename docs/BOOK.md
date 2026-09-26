# 《KunAutoDrive：从零构建高性能自动驾驶系统与仿真引擎》

> **这是什么**：一本从微内核底座、通信与调度，到规控算法栈、仿真闭环和真车执行器的自动驾驶全栈技术专著。正文在 `docs/book/`，本页是阅读顺序和源码对照。
>
> **怎么读**：按下面的 Part 0 到 Part VI 往下读。表里的编号是新的阅读顺序；「现文件」链到 `docs/book/` 里**现在的文件名**。编号和文件名不一致时，以链接为准，例如新的第 05 章链到 [`book/03_message_bus.md`](book/03_message_bus.md)。各章文件以后由该章自己的改写改名；在那之前，用表里的链接打开正文。
>
> 00b 和 03 还没有正文，表里标 **待写**，不放链接。

---

## Part 0　从这里开始

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 00 | 前言 | 这本书要造的系统、以及后面各章共用的设计约束从哪来 | [`book/00_preface.md`](book/00_preface.md) |
| 00b | 跑起来：构建、demo、读懂 pipeline.json | 用 `build.sh` 编出来，用 `scripts/demo.sh` 跑起来，再读懂 `config/pipeline.json` 里的进程表 | **待写** |

## Part I　框架骨架

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 01 | 用 C 造一个对象 | 用结构体首成员和函数指针表做出 `TaskBase` / `TaskInterface`，管住 initialize、execute、cleanup | [`book/01_oop_in_c.md`](book/01_oop_in_c.md) |
| 02 | 可插拔 .so 与启动器 | `NodePlugin` 与 `node_get_plugin`，`flow_launcher` 按 pipeline 做 dlopen，`flow_node_host` 把同一份 .so 起成独立进程 | [`book/02_plugin_system.md`](book/02_plugin_system.md) |
| 03 | 注册中心与参数系统 | int/float 注册时带上下界，`param_set_int` / `param_set_float` 越界拒绝；`param_set_callback` 做热更新。任务、话题、类型、插件进 `flow_registry_*` | **待写** |
| 04 | 状态机 | 反射式状态机：转移表、guard、entry/exit，非法事件有明确策略 | [`book/08_state_machine.md`](book/08_state_machine.md) |

## Part II　通信与时间

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 05 | 消息总线 | 进程内 Pub/Sub：`message_bus_publish` / `message_bus_subscribe`，以及按话题设置 QoS | [`book/03_message_bus.md`](book/03_message_bus.md) |
| 06 | 类型 ID、IDL 与序列化 | `fnv1a_hash` 类型 ID、`serializer_register_type`，IDL 在 `msg/adas_msgs.msg`，由 `tools/msg_codegen.py` 生成 | [`book/07_serializer.md`](book/07_serializer.md) |
| 07 | 共享内存 IPC | `ipc_channel_open` / `ipc_channel_publish` 的共享内存通道，仪表盘 JSON 走 `dashboard_bridge_publish` | [`book/04_ipc_channel.md`](book/04_ipc_channel.md) |
| 08 | 统一传输与服务发现 | `transport_publish` 统一收发；发现用组播 `DISC_MULTICAST_GROUP`（`239.255.0.100`）和 `DISC_MULTICAST_PORT`（5500）；跨机走 `net_transport_connect` | [`book/09_discovery.md`](book/09_discovery.md) |
| 09 | 时钟服务 | `clock_now_us` 在仿真模式下可被注入；`clock_now_realtime_us` 始终是墙钟 | [`book/06_clock_service.md`](book/06_clock_service.md) |

## Part III　执行

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 10 | C++20 协程 | `CoroutineTask` 与 `node_pump`。总线上真实的 `co_await` 用户是 `modules/adas_nodes/fusion_node.cpp` 里的 `fusion_bridge.recv_any_for` | [`book/10_coroutine.md`](book/10_coroutine.md) |
| 11 | 调度器与绑核 | `scheduler_register_task` 与 `rate_control_init`；`task_manager_start_all_deps` 按依赖启动 | [`book/11_scheduler.md`](book/11_scheduler.md) |

## Part IV　录制与回放

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 12 | Bag / MCAP / flowrec | Bag 的 `bag_writer_write` / `bag_reader_play`，MCAP 的 `mcap_writer_write_msg` / `mcap_reader_next`，以及 `flowrec_engine_process` 按配置留存话题 | [`book/05_bag_recording.md`](book/05_bag_recording.md) |

## Part V　自动驾驶算法栈

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 13 | 激光点云到目标 | `lidar_scan_generate` 产生点，`dbscan_run` 聚类，`ktracker_associate_and_update` 用匈牙利算法做关联。视觉和其他感知源并进本章一节，见下面的说明 | [`book/12_lidar_tracking.md`](book/12_lidar_tracking.md) |
| 14 | 定位融合 EKF | `ekf_slam_predict` / `ekf_slam_update`，以及 `ekf_fusion_predict` 与 GPS / LiDAR 更新；NMEA 由 `nmea_parse_line` 解析 | [`book/13_sensor_fusion.md`](book/13_sensor_fusion.md) |
| 15 | 行为决策（含预测输入） | 行为状态机发布 `planning/behavior`；导航、轨迹规划节点和 `prediction/tracks` 是它的上下游 | [`book/14_behavior_decision.md`](book/14_behavior_decision.md) |
| 16 | Frenet 轨迹规划 | `project_to_path` 与 `frenet_plan` 把问题投到参考线上；`st_graph_plan` 做 S-T 速度，`pjqp_path_solve` / `pjqp_speed_solve` 做分段 jerk QP | [`book/15_trajectory_planning.md`](book/15_trajectory_planning.md) |
| 17 | 跟踪控制：Stanley / MPC | `modules/adas_nodes/control_node.cpp` 里是 PID + Stanley 跟轨迹；`ltv_mpc_solve` 解时变线性 MPC；`ManeuverTracker` 管掉头和泊车这类断开的参考线 | [`book/16_tracking_control.md`](book/16_tracking_control.md) |
| 18 | 安全包络与降级 | `modules/adas_nodes/safety_control_node.cpp` 对控制指令做安全包络再发布 `control/cmd`；`safety_arbiter_apply`、`degrade_layer_action`、`health_heartbeat` 负责仲裁、降级和心跳 | [`book/17_safety_envelope.md`](book/17_safety_envelope.md) |
| 19 | 执行器：SocketCAN / PWM | `modules/adas_nodes/actuator_node.c` 把控制指令打成 SocketCAN 帧；`pwm_map_control_cmd` 映射到 PWM。真车进程表在 `config/pipeline_car.json` | [`book/22_socketcan_actuator.md`](book/22_socketcan_actuator.md) |

第 13 章不单开视觉章。激光跟踪那一章里用一节「其他感知源」把它们收进来，并写明下面两件事：

- `modules/adas_nodes/lane_detection_node.c` 是沙箱。它订阅 `road/geometry`，用场景里的道路几何合成车道线。Canny 边缘检测和 Hough 变换只写在文件头的注释里（注释称之为尚未实现的 HAVE_CV 版本），这个 .c 文件里没有这条图像处理路径。
- `modules/adas_nodes/bev_post.h` 和 `modules/adas_nodes/bev_post.c` 只做协议映射。`bev_post_to_obstacle_list` 把已经解码好的 `BevPostDet` 填进 `ObstacleList`。网络输出怎么解码不在这两个文件里。

同一节里的相邻源码按各自文件的实际行为来写。`modules/adas_nodes/traffic_light_recognition_node.c` 也是沙箱，把 `road/traffic_lights` 转成 `perception/traffic_lights`，注释里的相机检测没有实现。`modules/adas_nodes/bev_detection_node.cpp` 是影子节点，发布 `bev/obstacles`；栅格用 `bev_pre_rasterize`，可选的 ONNX 前向用 `bev_onnx_backend_forward`，没有模型时走真值直通，填 `ObstacleList` 时调用 `bev_post_to_obstacle_list`。`modules/adas_nodes/stereo_vision_node.c` 把 `sensor/stereo` 的深度反投影后做 DBSCAN。`modules/adas_nodes/perception_node.cpp` 在 ground_truth 与 `sensor/lidar_points` 两种模式下产出 `perception/obstacles`。`modules/adas_nodes/perception_fusion_node.cpp` 里的 `fuse_obstacles` 与 `associate_and_track` 合并两路 `ObstacleList`。

## Part VI　仿真、可观测与闭环

| 编号 | 章节 | 这一章讲什么 | 现文件 |
|---|---|---|---|
| 20 | FlowSim 场景与世界 | 仿真只做被控对象：积分、发布真值。场景由 `scenario_load` 读入，路网和 NPC 在 `modules/adas_nodes/flowsim/` | [`book/18_flowsim_scenario_design.md`](book/18_flowsim_scenario_design.md) |
| 21 | 可视化与内省 | `flowmond` 与 `monitor_server` 把拓扑送给浏览器；仪表盘 JSON 经 `dashboard_bridge_publish`；前端入口是 `tools/flowboard/js/app.js` | [`book/20_flowmond_3d_vis.md`](book/20_flowmond_3d_vis.md) |
| 22 | 验证关卡与回归评估 | `ci/evaluators/demo_evaluator.py` 与 `ci/evaluators/scenario_regression.py` 做行为回归，`ci/gates/` 做静态契约，`tools/pipeline_check.py` 做离线管道检查 | [`book/21_demo_evaluator.md`](book/21_demo_evaluator.md) |
| 23 | 端到端学习闭环 | `modules/adas_nodes/learner_node.c` 采集，`modules/adas_nodes/inference_node.cpp` 用 `tiny_mlp_load` / `tiny_mlp_forward` 做影子推理，`modules/adas_nodes/model_ota_node.c` 负责换模型；训练脚本在 `tools/train_e2e/` | [`book/19_e2e_learning_loop.md`](book/19_e2e_learning_loop.md) |

---

## 章节依赖

阅读和改写都按这个顺序：后一章可以引用前面已经出现的章。

- 00b 是后面每一章可运行示例的共同起点。
- 01 → 02 → 03 → 04。
- 05 → 06 → 07 → 08。
- 09 排在 11 和 12 前面。
- 10 → 11。
- 12 依赖 05、06、09。
- 第五部依赖 05、09、10。部内顺序是 13 → 14 → 15 → 16 → 17 → 18 → 19。18 依赖 17，19 依赖 18。
- 第六部依赖 02、12，以及整个第五部。

原先拟单列的视觉章已并进 13，所以 13 之后直接是定位融合（14），中间没有空号。

---

## 附录：章节—源码对照表

路径都相对于仓库根目录。00b 与 03 尚无章节文件。关键符号只列在头文件或实现里能够对上的名字。

### 00　前言

- 现文件：[`book/00_preface.md`](book/00_preface.md)
- `README.md`
- `scripts/demo.sh`
- `tools/pipeline_check.py`

### 00b　跑起来：构建、demo、读懂 pipeline.json

- 现文件：待写
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
- `src/plugins/example_process.c`
- 同 00b 的六份 `config/pipeline.json`、`config/pipeline_car.json`、`config/pipeline_manual.json`、`config/pipeline_sensor.json`、`config/pipeline_cortex.json`、`config/pipeline_windows.json`

### 03　注册中心与参数系统

- 现文件：待写
- `include/param_registry.h`，`src/core/param_registry.c`：`param_register_int` / `param_register_float` / `param_register_bool` / `param_register_string`。int 与 float 注册时带 min/max；`param_set_int` 与 `param_set_float` 越界则拒绝。`param_set_callback` 在运行时改值时调用。`param_enable_hot_reload` 打开热更新。`param_export_json` 导出。
- `include/flow_registry.h`，`src/core/flow_registry.c`：`flow_registry_register_task`，`flow_registry_register_topic`，`flow_registry_register_type`，`flow_registry_register_plugin`，`flow_registry_export_json`。宏 `FLOW_REGISTRY_DECLARE_PLUGIN` 展开后只调用 `flow_registry_register_plugin(名字, NULL, NULL, NULL)`，宏参数里的版本和描述没有写入注册表。
- `include/topic_registry.h`：编译期话题名常量，例如 `TOPIC_SENSOR_LIDAR`（`sensor/lidar`）、`TOPIC_PERCEPTION_OBSTACLES`、`TOPIC_CONTROL_CMD`
- `include/config_manager.h`，`src/core/config_manager.c`：`config_load`，`config_save`，`config_free`
- `include/param_bridge.h`，`src/core/param_bridge.c`：`param_bridge_server_start`，`param_bridge_server_stop`，`param_bridge_client_request`。默认套接字 `PARAM_BRIDGE_DEFAULT_SOCK`（`/tmp/flow_param.sock`），可用 `FLOW_PARAM_SOCK` 覆盖。线协议是 `LIST` / `GET` / `SET`
- `src/flowctl.c`：`flowctl param list`、`flowctl param get <name>`、`flowctl param set <name> <value>`

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
- `include/serializer.h`，`src/core/serializer.c`：`fnv1a_hash`，`serializer_register_type`，`SerializeFunc`，`DeserializeFunc`
- `include/msg_schema.h`，`src/core/msg_schema.c`：`msg_schema_register`，`msg_schema_check`
- `msg/adas_msgs.msg`
- `tools/msg_codegen.py`

### 07　共享内存 IPC

- 现文件：[`book/04_ipc_channel.md`](book/04_ipc_channel.md)
- `include/ipc_channel.h`，`src/core/ipc_channel.c`：`IpcChannel`，`ipc_channel_open`，`ipc_channel_publish`，`ipc_channel_subscribe`，`ipc_channel_start`
- `include/dashboard_bridge.h`，`src/core/dashboard_bridge.c`：`dashboard_bridge_publisher_open`，`dashboard_bridge_publish`，`dashboard_bridge_subscriber_open`
- `src/ipc_demo.c`

### 08　统一传输与服务发现

- 现文件：[`book/09_discovery.md`](book/09_discovery.md)
- `include/transport.h`，`src/core/transport.c`：`Transport`，`transport_create`，`transport_publish`，`transport_subscribe`
- `include/discovery.h`，`src/core/discovery.c`：`DiscoveryManager`，`discovery_create`，`discovery_advertise`，`discovery_create_ipc_channels`；`DISC_MULTICAST_GROUP`，`DISC_MULTICAST_PORT`
- `include/network_transport.h`，`src/cpp/network_transport.cpp`：`net_transport_start`，`net_transport_connect`，`net_transport_bridge_topic`
- `src/benchmark_tcp.c`

### 09　时钟服务

- 现文件：[`book/06_clock_service.md`](book/06_clock_service.md)
- `include/clock_service.h`，`src/core/clock_service.c`：`clock_now_us`，`clock_now_realtime_us`，`clock_set_sim_mode`，`clock_set_sim_time`，`clock_advance_us`

### 10　C++20 协程

- 现文件：[`book/10_coroutine.md`](book/10_coroutine.md)
- `include/coroutine_task.h`：`CoroutineTask`，`node_pump`
- `src/plugins/flowcoro_task.cpp`
- `src/coro_bus_demo.cpp`
- `src/rt_heartbeat_demo.cpp`
- `src/benchmark_coro.cpp`
- `tests/coro_correctness_test.cpp`
- `modules/adas_nodes/fusion_node.cpp`：`co_await fusion_bridge.recv_any_for(...)`

### 11　调度器与绑核

- 现文件：[`book/11_scheduler.md`](book/11_scheduler.md)
- `include/scheduler.h`，`src/core/scheduler.c`：`Scheduler`，`scheduler_create`，`scheduler_register_task`，`scheduler_run_loop`，`RateControl`，`rate_control_init`
- `src/cpp/scheduler_cpp.cpp`
- `include/task_manager.h`，`src/core/task_manager.c`：`task_manager_create`，`task_manager_register`，`task_manager_start_all_deps`

### 12　Bag / MCAP / flowrec

- 现文件：[`book/05_bag_recording.md`](book/05_bag_recording.md)
- `include/bag.h`，`src/core/bag.c`：`bag_writer_open`，`bag_writer_write`，`bag_reader_open`，`bag_reader_play`
- `include/mcap_writer.h`，`src/core/mcap_writer.c`：`mcap_writer_open`，`mcap_writer_write_msg`，`mcap_writer_close`
- `src/core/mcap_reader.c`：`mcap_reader_open`，`mcap_reader_next`，`mcap_reader_close`
- `src/mcap_replay.c`
- `include/flowrec.h`，`src/core/flowrec.c`：`flowrec_engine_create_from_json`，`flowrec_engine_process`，`flowrec_engine_tick`
- `modules/adas_nodes/flowrec_node.c`
- `tools/bag_check.c`
- `src/bag_demo.c`

### 13　激光点云到目标

- 现文件：[`book/12_lidar_tracking.md`](book/12_lidar_tracking.md)
- `modules/adas_nodes/lidar_scan.h`，`modules/adas_nodes/lidar_scan.c`：`LidarScanPlan`，`lidar_scan_generate`，`lidar_scan_plan_sanitize`
- `modules/adas_nodes/lidar_contract.h`：`lidar_point_cloud_validate`，`lidar_point_cloud_capacity`
- `modules/adas_nodes/lidar_driver_node.c`
- `src/algorithms/dbscan_cluster.h`，`src/algorithms/dbscan_cluster.c`：`dbscan_init`，`dbscan_run`，`dbscan_cluster_count`
- `src/algorithms/kalman_tracker.h`，`src/algorithms/kalman_tracker.c`：`KalmanTracker`，`ktracker_init`，`ktracker_predict`，`ktracker_associate_and_update`（实现里的 `hungarian_solve`）
- `modules/adas_nodes/object_tracker_node.c`：订阅 `perception/obstacles`，发布 `perception/tracked_objects`

其他感知源（并入本章，不另开编号）：

- `modules/adas_nodes/lane_detection_node.c`：沙箱。输入 `road/geometry` 与 `vehicle/state`，输出 `perception/lanes`。车道线由道路几何合成。Canny / Hough 只存在于注释。
- `modules/adas_nodes/bev_post.h`，`modules/adas_nodes/bev_post.c`：`BevPostDet`，`bev_post_to_obstacle_list`
- `modules/adas_nodes/bev_pre.h`，`modules/adas_nodes/bev_pre.c`：`bev_pre_rasterize`，`bev_pre_config_default`
- `modules/adas_nodes/bev_onnx_backend.h`，`modules/adas_nodes/bev_onnx_backend.cpp`：`bev_onnx_backend_load`，`bev_onnx_backend_forward`
- `modules/adas_nodes/bev_detection_node.cpp`：影子模式，发布 `bev/obstacles`，调用上面的 pre / onnx / `bev_post_to_obstacle_list`
- `modules/adas_nodes/traffic_light_recognition_node.c`：沙箱，转发 `road/traffic_lights`
- `modules/adas_nodes/stereo_vision_node.c`：`sensor/stereo` 深度反投影后聚类
- `modules/adas_nodes/perception_node.cpp`：`perception/obstacles`
- `modules/adas_nodes/perception_fusion_node.cpp`：`fuse_obstacles`，`associate_and_track`

### 14　定位融合 EKF

- 现文件：[`book/13_sensor_fusion.md`](book/13_sensor_fusion.md)
- `modules/adas_nodes/ekf_slam.h`，`modules/adas_nodes/ekf_slam.c`：`ekf_slam_init`，`ekf_slam_predict`，`ekf_slam_update`，`ekf_slam_get_pose`
- `src/algorithms/ekf_fusion.h`，`src/algorithms/ekf_fusion.c`：`ekf_fusion_init`，`ekf_fusion_predict`，`ekf_fusion_update_gps`，`ekf_fusion_update_lidar`，`ekf_fusion_get_state`
- `modules/adas_nodes/slam_node.cpp`
- `modules/adas_nodes/slam_math.h`，`modules/adas_nodes/slam_math.c`：`slam_wrap_pi`
- `modules/adas_nodes/gps_driver_node.c`
- `modules/adas_nodes/imu_driver_node.c`
- `src/algorithms/nmea_parser.h`，`src/algorithms/nmea_parser.c`：`nmea_parser_init`，`nmea_parse_line`

### 15　行为决策（含预测输入）

- 现文件：[`book/14_behavior_decision.md`](book/14_behavior_decision.md)
- `modules/adas_nodes/behavior_planner_node.cpp`：发布 `planning/behavior`
- `modules/adas_nodes/planning_node.cpp`
- `modules/adas_nodes/navigation_node.c`
- `modules/adas_nodes/prediction_node.c`：输出 `prediction/tracks`

### 16　Frenet 轨迹规划

- 现文件：[`book/15_trajectory_planning.md`](book/15_trajectory_planning.md)
- `modules/adas_nodes/planning_coordinates.h`：`planning_coord::Projection`，`project_to_path`，`lane_center_d`
- `src/algorithms/frenet_bridge.h`，`src/algorithms/frenet_bridge.cpp`：`frenet_set_reference_path`，`frenet_plan`
- `modules/adas_nodes/st_graph.h`，`modules/adas_nodes/st_graph.c`：`st_graph_plan`
- `src/algorithms/piecewise_jerk_qp.c`：`pjqp_path_solve`，`pjqp_speed_solve`，`pjqp_smooth_1d`（没有单独的头文件）
- `modules/adas_nodes/traj_safety.c`：`traj_point_test_hits`

### 17　跟踪控制：Stanley / MPC

- 现文件：[`book/16_tracking_control.md`](book/16_tracking_control.md)
- `modules/adas_nodes/control_node.cpp`：PID + Stanley 横向控制
- `modules/adas_nodes/maneuver_tracker.h`：`ManeuverTracker`，`ManeuverTrackerParams`，`TrackPoint`，`ManeuverResult`
- `include/ltv_mpc.h`，`src/algorithms/ltv_mpc.c`：`ltv_mpc_set_reference`，`ltv_mpc_set_state`，`ltv_mpc_solve`
- `modules/adas_nodes/waypoint_follower_node.c`：Pure Pursuit 航点跟随，输出 `planning/trajectory`（RC 小车路径，与 `planning_node` 二选一）
- `docs/LTV_MPC_DESIGN.md`

### 18　安全包络与降级

- 现文件：[`book/17_safety_envelope.md`](book/17_safety_envelope.md)
- `modules/adas_nodes/safety_control_node.cpp`：订阅原始控制与车辆状态，发布最终 `control/cmd`
- `modules/adas_nodes/safety_arbiter.h`，`modules/adas_nodes/safety_arbiter.c`：`safety_arbiter_apply`
- `include/degrade_ladder.h`，`src/core/degrade_ladder.c`：`degrade_set_level`，`degrade_layer_action`，`degrade_supervisor_tick`
- `include/health.h`，`src/core/health.c`：`health_init`，`health_register`，`health_heartbeat`
- `include/safety_evidence.h`，`src/core/safety_evidence.c`：`safety_evidence_to_json`
- `include/safety_fault_injection.h`，`src/core/safety_fault_injection.c`：`safety_fault_injection_init`，`safety_fault_injection_start`

### 19　执行器：SocketCAN / PWM

- 现文件：[`book/22_socketcan_actuator.md`](book/22_socketcan_actuator.md)
- `modules/adas_nodes/actuator_node.c`：SocketCAN 输出；非 Linux 环境降级为 dry-run
- `modules/adas_nodes/actuator_pwm_node.c`
- `modules/adas_nodes/pwm_map.h`，`modules/adas_nodes/pwm_map.c`：`pwm_map_control_cmd`
- `config/pipeline_car.json`

### 20　FlowSim 场景与世界

- 现文件：[`book/18_flowsim_scenario_design.md`](book/18_flowsim_scenario_design.md)
- `modules/adas_nodes/flowsim_node.cpp`
- `modules/adas_nodes/flowsim/`：`modules/adas_nodes/flowsim/physics.cpp`、`modules/adas_nodes/flowsim/npc_ai.cpp`、`modules/adas_nodes/flowsim/route.cpp`、`modules/adas_nodes/flowsim/scene_events.cpp`、`modules/adas_nodes/flowsim/scene_pub.cpp`、`modules/adas_nodes/flowsim/entity.h`、`modules/adas_nodes/flowsim/collision.cpp`、`modules/adas_nodes/flowsim/road_network.cpp`、`modules/adas_nodes/flowsim/sim_digest.cpp`
- `src/core/scenario_loader.c`：`scenario_load`，`scenario_free`
- `scenarios/`

### 21　可视化与内省

- 现文件：[`book/20_flowmond_3d_vis.md`](book/20_flowmond_3d_vis.md)
- `modules/adas_nodes/flowmond_node.cpp`
- `src/flowmond.c`
- `src/core/monitor_server.c`
- `include/dashboard_bridge.h`，`src/core/dashboard_bridge.c`：`dashboard_bridge_publish`
- `tools/flowboard/js/app.js`

### 22　验证关卡与回归评估

- 现文件：[`book/21_demo_evaluator.md`](book/21_demo_evaluator.md)
- `ci/evaluators/demo_evaluator.py`
- `ci/evaluators/scenario_regression.py`
- `ci/gates/`：`ci/gates/topic_contract_check.py`、`ci/gates/sensor_wiring_check.py`、`ci/gates/zombie_ban_check.py`、`ci/gates/plugin_symbol_check.py`、`ci/gates/msg_layout_check.py`、`ci/gates/lane_match_schema_check.py`、`ci/gates/lanelet_consistency_check.py`
- `tools/pipeline_check.py`
- `src/core/auto_tuner.c`：`auto_tuner_init`，`auto_tuner_register`
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
| 同上 | `modules/adas_nodes/example_filter_node.c` | 没有这个文件。示例插件是 `src/plugins/example_process.c` |
| [`book/10_coroutine.md`](book/10_coroutine.md)（新 10） | `modules/adas_nodes/coro_fusion_node.cpp` | 没有这个文件。协程融合节点是 `modules/adas_nodes/fusion_node.cpp` |
| [`book/16_tracking_control.md`](book/16_tracking_control.md)（新 17） | `include/maneuver_tracker.h` | 没有这个路径。头文件在 `modules/adas_nodes/maneuver_tracker.h` |
| 同上 | `src/core/ltv_mpc.c` | 没有这个路径。实现在 `src/algorithms/ltv_mpc.c`，声明在 `include/ltv_mpc.h` |
| [`book/21_demo_evaluator.md`](book/21_demo_evaluator.md)（新 22） | `tools/demo_evaluator.py` | 没有这个路径。评估器在 `ci/evaluators/demo_evaluator.py` |
| 同上 | `tools/param_sweep.py` | 没有这个文件 |
| [`book/15_trajectory_planning.md`](book/15_trajectory_planning.md)（新 16） | （正文没有点名任何源码文件） | 对照表见本页第 16 章：`modules/adas_nodes/planning_coordinates.h`、`src/algorithms/frenet_bridge.cpp`、`modules/adas_nodes/st_graph.c`、`src/algorithms/piecewise_jerk_qp.c`、`modules/adas_nodes/traj_safety.c` |
| [`book/03_message_bus.md`](book/03_message_bus.md)（新 05） | 标题「15 个节点」 | 这是标题里的断言，不是从 `config/pipeline.json` 的 `processes` 数出来的。改写时按该数组计数，不要照抄 15 |
