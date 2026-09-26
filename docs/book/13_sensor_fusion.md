# 第 14 章：定位融合：EKF

这一章在第五部　自动驾驶算法栈。
上一章是感知：从激光点云到目标（含其他感知源）。
[第 13 章](12_lidar_tracking.md#卡尔曼追踪器在追什么) 把一帧激光雷达点云收成前车的边界框（Bounding Box）。
超车还要知道自车在哪一条车道上。
本章给出自车位置。
位置若是漂的，或者 `fusion/localization` 根本不发，下一章的行为决策就会用错车道。

`fusion/localization` 上的数来自两个互不调用的扩展卡尔曼滤波（EKF）。
中间还有一处三分支的位置选择。
选错时，真车配置下这条话题（Topic）可以一直是空的。
或者位姿已经很不确定，滤波器却不采纳激光雷达位置，只按预测往前积分。
这一章按源码走完两条滤波和三个分支，并用 `ekf_chapter` 把布局、一条合成轨迹和故障用例跑出来。

## 定位接在跟踪和变道之间

默认管线里，自车位置有两条来路。
仿真的 `sensor_model` 把自车坐标写成 `LidarFrame`，发到 Topic `sensor/lidar`。
`fusion_node` 再把它和 `sensor/gps`、`sensor/pose` 收进同一个 EKF。
`config/pipeline.json` 里没有 `slam` 进程。
所以默认路径上 `sensor/pose` 是空的，位置走激光雷达这一支。

真车模板 `config/pipeline_car.json` 把 `slam` 的 `algo` 设成 `ekf_slam`。
`slam_node` 在这个算法下跑的是 `ekf_slam`，把 `Pose2D` 发到 `sensor/pose`。
源码中 `ekf_slam.c` 不调用 `ekf_fusion.c`。
两边各有自己的状态向量。
`fusion_node` 只消费 `Pose2D` 的 x、y，不把同时定位与建图（SLAM）的速度和航向写进融合状态。

行为规划订阅的是融合结果的 JavaScript 对象表示法（JSON），不是 `Pose2D`。
`on_fusion` 解析 `x`、`y`、`v`、`heading`（`modules/adas_nodes/behavior_planner_node.cpp::on_fusion:L332-L357`）。
同一函数里，若 `vehicle/state` 在 200 ms 内到过，就不用这份 EKF。
默认仿真里 FlowSim 的真值会盖住融合位置。
真车模板没有这路真值。
超车时自车在快车道还是慢车道，就只看 `fusion/localization`。

发布和订阅的英文是发布/订阅（Publish/Subscribe, Pub/Sub）。
本章后面只说发布、订阅。

## 两条滤波，三种位置

卡尔曼滤波（KF）每拍做两件事：先按运动模型把状态往前推，再用测量把偏差拉回来。
EKF 的差别只在运动方程是弯的。
推协方差之前，要把这根弯的在当前点拉直，拉直用的矩阵叫雅可比。
KunAutoDrive 的做法是：融合节点用五维 EKF，SLAM 节点用另一套五维 EKF，两者不互相调用。

融合状态的顺序是 `[x, y, v, heading, yaw_rate]`，单位分别是米、米、米/秒、弧度、弧度/秒（`src/algorithms/ekf_fusion.h::EKF_STATE_DIM:L45-L45`）。
`ekf_fusion_init` 把这个顺序抄进 `x[]`，并铺上 `P` 和 `Q`（`src/algorithms/ekf_fusion.c::ekf_fusion_init:L262-L305`）。
SLAM 状态的顺序不同，是 `[x, y, heading, v, omega]`，而且成员是 `float`（`modules/adas_nodes/ekf_slam.h::EkfState:L14-L20`）。

融合的预测不是自行车模型。
头文件 Prediction 注释写着 bicycle kinematic model，还写着 `v' = v + a·dt`。
这两句都过时了。
`ekf_fusion_predict` 写的是下面这五行（`src/algorithms/ekf_fusion.c::ekf_fusion_predict:L307-L349`）。

```c
x_pred[0] = ekf->x[0] + v * cos(psi) * dt;
x_pred[1] = ekf->x[1] + v * sin(psi) * dt;
x_pred[2] = v;
x_pred[3] = psi + yr * dt;
x_pred[4] = yr;
```

速度不变，横摆角速度也不变。
这是常速、常横摆角速度（CTRV）。
加速度不进状态，只通过过程噪声 `Q` 让速度的方差变大。
`compute_jacobian_F` 在更新前的状态上把方程拉直（`src/algorithms/ekf_fusion.c::compute_jacobian_F:L51-L66`）。
单位阵之外，非零项是 `F[0][2] = cos(ψ)·dt`、`F[0][3] = -v·sin(ψ)·dt`、`F[1][2] = sin(ψ)·dt`、`F[1][3] = v·cos(ψ)·dt`、`F[3][4] = dt`。
最后一行仍是单位阵的那一格：`F[4][4] = 1`。
SLAM 那边最后一行全是 0，因为 `omega` 被陀螺仪直接赋值，不依赖上一拍的 `omega`。

位置从哪来，不在 EKF 内部，而在 `FusionTask::run`（`modules/adas_nodes/fusion_node.cpp::FusionTask::run:L129-L282`）。

```c
if (pose && pose->converged) {
    double pose_cov = (double)pose->cov_xx + (double)pose->cov_yy;
    if (pose_cov < 100.0) {
        pos_used_x = pose->x; pos_used_y = pose->y;
        ekf_fusion_update_lidar(ekf_, (double)pose->x, (double)pose->y, nullptr);
    }
} else {
    pos_used_x = lidar->x; pos_used_y = lidar->y;
    ekf_fusion_update_lidar(ekf_, (double)lidar->x, (double)lidar->y, nullptr);
}
```

三个分支：

1. `Pose2D` 存在、`converged` 为真，且 `cov_xx + cov_yy < 100`。用位姿的 x、y。
2. 位姿存在且已收敛，但协方差之和大于等于 100。这个周期不做位置更新。也不回退到 `LidarFrame`。
3. 没有位姿，或 `converged` 为假。用 `LidarFrame` 的 x、y。

`else` 只挂在外层。
内层 `if (pose_cov < 100.0)` 没有 `else`。
把整段读成「否则用激光雷达」会漏掉分支 2。

两次位置更新传的 `R` 都是空指针。
`R` 为空时，`ekf_fusion_update_lidar` 改用 `DEFAULT_R_LIDAR_VAR`，也就是 0.25（`src/algorithms/ekf_fusion.c::ekf_fusion_update_lidar:L351-L374`）。
位姿协方差不进 `R`。
全球定位系统（GPS）的速度和航向是另一次更新。
`FusionTask::run` 把 `heading_deg` 乘 `π/180`，再调用 `ekf_fusion_update_gps`。
纬度、经度只写进输出 JSON 的 `world_lat`、`world_lon`，不进状态。

图 14-1：两条 EKF 和位置三分支

```text
sensor/imu ──► ekf_slam ──► Pose2D on sensor/pose
sensor/lidar (LidarFrame) ──┐
sensor/gps (速度、航向) ────┼──► ekf_fusion ──► fusion/localization
sensor/pose ── 三分支 ──────┘
```

## 状态向量占多少字节

`EkfFusion` 是双精度。
五维状态 5×8 = 40 字节，所以 `P` 从偏移 40 开始。
`P` 和 `Q` 各是 5×5 的 `double`，各 200 字节。
`EkfSlam` 是单精度，状态加协方差更小。
线上消息的 C 结构体和打包长度不一定相等。
`Pose2D` 里 `bool converged` 后面有填充，`source` 在偏移 28。
打包函数按字段紧排，长度是 29。
`GpsData` 同样：结构体 40 字节，打包 36 字节。
`LidarFrame` 两边都是 24。

下面这段是本章机器上的原样输出。
命令是 `env -u LD_LIBRARY_PATH ./build/bin/ekf_chapter layout`。

```text
sizeof(EkfFusion)=480
offsetof(EkfFusion,x)=0
offsetof(EkfFusion,P)=40
offsetof(EkfFusion,Q)=240
offsetof(EkfFusion,dt)=440
offsetof(EkfFusion,predict_count)=448
offsetof(EkfFusion,update_count)=452
offsetof(EkfFusion,last_innovation)=456
offsetof(EkfFusion,diverged)=464
offsetof(EkfFusion,chi2_fail_count)=468
offsetof(EkfFusion,gated_count)=472
sizeof(EkfSlam)=160
offsetof(EkfSlam,x)=0
offsetof(EkfSlam,P)=20
offsetof(EkfSlam,last_time_us)=120
offsetof(EkfSlam,initialized)=128
offsetof(EkfSlam,process_noise)=132
offsetof(EkfSlam,measurement_noise)=152
sizeof(EkfState)=20
sizeof(Pose2D)=32
Pose2D_serialize_len=29
offsetof(Pose2D,converged)=24
offsetof(Pose2D,source)=28
sizeof(LidarFrame)=24
LidarFrame_serialize_len=24
sizeof(GpsData)=40
GpsData_serialize_len=36
EKF_STATE_DIM_fusion_header=5
```

表 14-1：结构体大小（同上一次运行）

| 类型 | `sizeof` | 打包长度 |
| --- | --- | --- |
| `EkfFusion` | 480 | 无打包函数 |
| `EkfSlam` | 160 | 无打包函数 |
| `Pose2D` | 32 | 29 |
| `LidarFrame` | 24 | 24 |
| `GpsData` | 40 | 36 |

最后一行 `EKF_STATE_DIM_fusion_header=5` 是程序里的字面量，用来标明头文件里的维数。
它不是 `sizeof` 测出来的。

## 创建、预测和更新

`ekf_fusion_init` 把 `dt` 和初值抄进对象，再铺 `P` 和 `Q`。
`fusion_init` 传入的初值是 `[0, 0, 5, 0, 0]`，`dt` 固定 0.05 秒，并且把 `params_json` 标成未使用（`modules/adas_nodes/fusion_node.cpp::fusion_init:L303-L359`）。
`pipeline` 里的 `frequency_hz` 不会改这个 `dt`，也不会改循环频率。

`ekf_slam_init` 把位置和航向设上，`omega` 置 0，`last_time_us` 置 0。
第一次 `ekf_slam_predict` 看到 `last_time_us == 0` 时只记下时间就返回（`modules/adas_nodes/ekf_slam.c::ekf_slam_predict:L63-L136`）。
`slam_node` 默认算法是 `dead_reckon`。
`pipeline_car.json` 才写成 `ekf_slam`。
`slam_init` 把航位推算用的协方差设成 0.01、0.01、0.01（`modules/adas_nodes/slam_node.cpp::slam_init:L437-L580`）。
航位推算（Dead Reckoning）在这里就是不再用位置测量、只把上一拍状态往前积。

`FusionTask::run` 每个醒来的周期做一次预测，`dt` 仍是 0.05 秒，不是墙钟间隔。
唤醒来自 `sensor/lidar`、`sensor/gps`、`sensor/pose` 里任意一条，或者 100 ms 超时。
源码中激光雷达缓冲是空的就会 `continue`。
这一拍不预测、不更新、也不发布。

缓冲里有帧时，先 `ekf_fusion_predict`。
再走上面的三分支做位置更新。
有 GPS 再做速度和航向更新。
然后 `ekf_fusion_get_state` 读出状态，打成一行 JSON 发布。
速度若小于 0 或大于 50，`FusionTask::run` 先夹到这个区间，再写回 `ekf->x[2]`。

位置更新会牵动速度。
雅可比把 `v` 耦合进 `x`。
预测之后 `P` 里就有 `x` 和 `v` 的交叉项。
位置残差因此能把 `v` 拉下去。
没有 GPS 时，速度不会停在初值 5。
故障实验的用例 `a` 里，`v` 从 5.62 落到 0，而 `x` 停在位姿的 50 附近。

SLAM 主路径在 `slam_update_ekf_slam`。
有惯性测量单元（IMU）时，`ekf_slam_predict` 吃 `accel_x` 和 `gyro_z`。
`v` 加上 `accel_x·dt`，`omega` 直接等于 `gyro_z`。
发布前 `converged` 写成 `true`（`modules/adas_nodes/slam_node.cpp::slam_update_ekf_slam:L262-L321`）。
`dead_reckon` 分支同样写死 `true`（`modules/adas_nodes/slam_node.cpp::slam_update_dead_reckon:L323-L364`）。
源码中只要位姿还在到，分支 3 就不会发生。

## 对不上的时候

测量和预测差太远时，更新可以被丢掉。
`ekf_update_generic` 算马氏距离（Mahalanobis Distance）的平方（`src/algorithms/ekf_fusion.c::ekf_update_generic:L97-L256`）。
二维测量的门槛是 5.991。
`update_count < 100` 时这段不执行。
注释写这是为了让滤波器先收敛。
过了 100 次成功更新之后，超门槛就 `chi2_fail_count` 和 `gated_count` 各加 1，状态不改。
`chi2_fail_count > 10` 时把 `diverged` 置 1。
所以是第 11 次连续失败，不是第 10 次。
被丢掉的更新不增加 `update_count`。

`diverged` 为真时，`FusionTask::run` 每 10 帧用当前这次的位置种子调用 `ekf_fusion_reset`。
`ekf_fusion_reset` 把 x、y 设成种子（`src/algorithms/ekf_fusion.c::ekf_fusion_reset:L454-L480`）。
速度若在 0 到 100 之间就保留，否则写回 5。
航向保留。
`chi2_fail_count` 清零，`gated_count` 不清。

GPS 航向残差用 `while` 折到 `±π`，不是 `atan2(sin, cos)`（`src/algorithms/ekf_fusion.c::ekf_fusion_update_gps:L376-L404`）。
更新之后没有把状态航向再折一次。
`ekf_fusion_predict` 末尾才会折。

位姿和 GPS 要和最新一帧 `LidarFrame` 的时间戳对上。
`message_buffer_find_nearest` 要求时间差不超过窗口，并且消息没有老于缓冲自己的 `window_us`。
融合节点创建位姿缓冲时窗口是 5 秒，容量 16。
查找位姿时 `max_delta` 是 100 ms，GPS 是 50 ms。
位姿缓冲的窗口是 5 秒，容量 16，都在 `fusion_init` 里。
`message_buffer_latest` 不看过期。
`message_buffer_push` 的「evict」循环体是空的，旧消息留在槽里，只靠查找时跳过（`src/core/fusion.c::message_buffer_push:L36-L58`）。

## 关掉之后，以及会卡住

`fusion_cleanup` 销毁任务包装和三个缓冲（`modules/adas_nodes/fusion_node.cpp::fusion_cleanup:L379-L391`）。
共享内存名字不会在订阅端关掉时删掉。
`ipc_channel_close` 写明发布者退出也不 `unlink`（`src/core/ipc_channel.c::ipc_channel_close:L571-L593`）。
下一次由发布者重新 `open` 时才会删掉旧名字再建。
`ekf_chapter` 的在线用例因此在拉起融合节点之前，自己先以发布者身份打开三条输入，把上一轮留在环里的帧清掉。

`message_bus_publish` 在第一次见到某个 Topic 时拿 `topic_mutex`，再调用 `count_active_subscribers`（`src/core/message_bus.c::message_bus_publish:L786-L980`）。
`count_active_subscribers` 在 `subs_seq` 为奇数时自旋（`src/core/message_bus.c::count_active_subscribers:L343-L353`）。
`message_bus_subscribe` 先把 `subs_seq` 加成奇数，再在 `update_subscriber_count` 里拿 `topic_mutex`（`src/core/message_bus.c::message_bus_subscribe:L1122-L1164`）。
一边持有 `topic_mutex` 自旋等序列号变偶，另一边持有奇数序列号等 `topic_mutex`。
文件里 L339 的注释已经指出不能在持有 `topic_mutex` 时再拿 `sub_mutex`。
序列号自旋和 `update_subscriber_count` 的加锁顺序仍能对上。

这次写作里有一次 `fault-live` 卡在这个点上。
`gdb` 看到融合进程主线程停在 `count_active_subscribers`，调用栈是 `node_announce_self` → `transport_publish` → `message_bus_publish`。
协程线程停在 `update_subscriber_count`，调用栈经过 `BusQueueBridge` 的构造和 `message_bus_subscribe`。
父进程的 `waitpid` 不返回。
下面贴出的那次 `all` 没有撞上。
标准错误是空的，退出码是 0。
示例里若 3 秒内等不到子进程退出，会再送 `SIGKILL`。
贴出的那次运行没有走到这一步。

## 主路径没覆盖的用法

`ekf_fusion_update_gps_full` 能一次吃进 x、y、速度和航向。
`fusion_node` 不调用它。
GPS 的经纬度因此不会修正位置。
卫星定位在原理上叫全球导航卫星系统（GNSS）。
驱动和 Topic 名用的是 GPS，载荷类型是 `GpsData`。

`slam_node` 还可以走 `dead_reckon`。
源码中每拍会把 `pose_cov_xx` 和 `pose_cov_yy` 各加 `0.05·dt`，上限 100。
每轴每秒加 0.05，两轴之和每秒加 0.10。
源码中从 0.02 加到 100 会经过 (100 − 0.02) / 0.10 = 999.8 秒。
那时每轴大约 50，还没到单轴上限 100。
和一旦不小于 100，融合就会进入分支 2，而且 `converged` 仍是 `true`。

默认管线没有 `slam`，位姿缓冲是空的，于是一直走分支 3。
`sensor_model` 发布的是原生 `sizeof(LidarFrame)` 字节。
融合节点的尺寸检查能通过。

真车模板的 `fusion` 进程在 JSON 里只订阅 `sensor/gps` 和 `sensor/pose`。
`fusion_init` 仍然订阅 `sensor/lidar`。
`lidar_execute` 发布的是 `ObstacleList`，Topic 名来自参数 `output_topic`（`modules/adas_nodes/lidar_driver_node.c::lidar_execute:L258-L333`）。
`pipeline_car.json` 里这个值是 `perception/obstacles`，并且 `enable` 为 0。
源码中没有任何进程往 `sensor/lidar` 发布 `LidarFrame`。
缓冲为空时 `FusionTask::run` 会 `continue`，`fusion/localization` 不会发出。
这和「JSON 没写订阅所以节点没订」不是一回事。
节点订了，只是没有 `LidarFrame` 发布者。

`Pose2D` 的消息注释写本地坐标 x 向前、y 向左。
`ekf_fusion.h` 写世界系，0 弧度朝东。
`sensor_model_execute` 把自车坐标加噪声后写进 `LidarFrame`，再按 `sizeof` 发布（`modules/adas_nodes/sensor_model_node.c::sensor_model_execute:L369-L453`）。
这三处哪个参考点、哪套坐标是权威的，源码没有收成一句话。
本章不选边。

## 编译然后跑通

示例在 `examples/ekf_chapter/ekf_chapter.c`。
CMake 目标名是 `ekf_chapter`，放在 `FLOW_BUILD_BOOK_EXAMPLES` 打开、且不是 Windows 的那个分支里。
它编译这个 C 文件，再加上 `ekf_slam.c` 和 `slam_math.c`。
`ekf_fusion.c` 已经在 `flowengine_core` 里，目标里不再编译一遍。
`transport.c` 在远程分支引用了 `net_transport_*`，这些符号在 `scheduler_cpp`。
所以链接了 `scheduler_cpp`，并把链接语言设成 C++。
在线用例还要有 `flow_node_host` 和 `build/lib/libfusion_node.so`。
插件用节点目录自己的 CMake 编，工作目录必须是仓库根，因为管线里的库路径是相对的。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DFLOW_BUILD_BOOK_EXAMPLES=ON
cmake --build build --target ekf_chapter flow_node_host -j"$(nproc)"
cmake -S modules/adas_nodes -B build/modules/adas_nodes \
  -DFLOWENGINE_BUILD="$PWD/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/modules/adas_nodes --target fusion_node -j"$(nproc)"
env -u LD_LIBRARY_PATH ./build/bin/ekf_chapter demo
```

`demo` 用 CTRV 走一条合成圆弧：`v = 8`，`yaw_rate = 0.2`，`dt = 0.05` 秒，40 步。
真值先走一步，再对融合做预测、位置更新和 GPS 更新，对 SLAM 做预测和位置更新。
下面是这次运行的原样输出。

```text
demo fusion_dt=0.05 slam_dt_us=50000 steps=40 v=8.0 yaw_rate=0.2
step fusion_x fusion_y fusion_v fusion_hdg_deg fusion_yr fusion_covxx slam_x slam_y slam_hdg_deg truth_x truth_y
0 0.000000 0.000000 8.000000 0.000000 0.200000 100.000000 0.000000 0.000000 0.000000 0.000000 0.000000
1 0.400000 0.000000 8.000000 0.572958 0.200000 0.249377 0.114286 0.000000 0.022037 0.400000 0.000000
10 3.994303 0.179865 8.000000 5.729578 0.200000 0.029722 2.012145 0.063885 4.946961 3.994303 0.179865
40 15.592392 3.079650 8.000000 22.918312 0.200000 0.020115 14.525130 2.752540 22.620113 15.592392 3.079650
fusion_predict_count=40 fusion_update_count=80 slam_initialized=1
```

第 40 步融合的 x、y 与真值相同，都是 15.592392 和 3.079650。
这是因为每步都把刚算出的真值送回去当测量，不是路上的定位精度。
`update_count` 是 80，因为每步有位置和 GPS 两次更新。
SLAM 的 x 是 14.525130，落后于同一真值。
它的预测还把 `omega` 直接写成陀螺输入，和融合的积分不是同一条式子。

## 一次预测要多少纳秒

计时方法：`clock_gettime(CLOCK_MONOTONIC)` 包住循环。
先空跑 1000 次，再跑 200000 次。
纳秒每调用 = 起止的微秒差 × 1000 / 200000。
机器是 Linux 6.12.94+，`uname` 为 `Linux cursor 6.12.94+ #1 SMP PREEMPT_DYNAMIC Thu Sep 24 16:04:37 UTC 2026 x86_64`。
CPU 是 `Intel(R) Xeon(R) Processor`，`cpu MHz` 2400.000，4 个逻辑核。
编译器是 gcc 13.3.0 与 g++ 13.3.0。
C 标志是 `-Wall -Wextra -fPIC -D_GNU_SOURCE -O2 -DNDEBUG -std=gnu11`。
命令是 `env -u LD_LIBRARY_PATH ./build/bin/ekf_chapter bench`。

```text
bench n=200000 clock=CLOCK_MONOTONIC warmup=1000
predict_ns_per_call=191.4
update_lidar_ns_per_call=203.3
update_gps_ns_per_call=195.5
```

这是单次函数调用的时间，不是整车定位周期。
周期由输入消息把 `FusionTask::run` 唤醒的频率决定。
故障实验里的 `rate` 用例，激光雷达名义 20 Hz、GPS 名义 10 Hz，定位输出跨度 1.950 秒里有 60 帧，算出来是 30.259 Hz。

## 故障实验

离线三组和在线九组都在同一台机器、同一次编出来的 `ekf_chapter` 上跑完。
在线组的命令是 `env -u LD_LIBRARY_PATH ./build/bin/ekf_chapter fault-live all`。
父进程先发布，再执行 `./build/bin/flow_node_host config/pipeline.json fusion 20`。
每个用例开头的三行发现日志形式相同，只是 `pid` 不同。
下面按用例贴该次运行的原文。

### 门限和重置

期望：满 100 次一致更新之后，一个离群点只记失败、不置 `diverged`。
连续 11 个离群点置 `diverged`。
`reset` 把 x 设成种子 10，`gated_count` 仍是 11。

```text
warmup update_count=100 gated_count=0 chi2_fail_count=0 diverged=0 x=-0.000025 last_innovation=0.000032
after_1_outlier update_count=100 gated_count=1 chi2_fail_count=1 diverged=0 x=-0.000022 last_innovation=0.000032
after_11_outliers update_count=100 gated_count=11 chi2_fail_count=11 diverged=1 x=0.000005 last_innovation=0.000032
after_reset_seed_10_0 update_count=100 gated_count=11 chi2_fail_count=0 diverged=0 x=10.000000 last_innovation=0.000032
```

`update_count` 停在 100，因为被拒绝的更新不加它。
与源码一致。

### 航向从 179 度到 -179 度

期望：残差走短弧 +2 度，而不是 -358 度。
状态航向在这次更新里不会被折回 `±180` 度以内。

```text
before_heading_deg=179.000000
meas_heading_deg=-179
after_heading_deg=180.960795
naive_delta_deg=-358.000000
wrapped_delta_deg=2.000000
last_innovation=0.034907 update_count=1
```

180.960795 度是短弧的结果。
若残差真是 -358 度，同一增益不会落在 180 度这一侧。
`last_innovation` 0.034907 弧度约等于 2 度。

### 时间戳盖在总线上

期望：`Message.timestamp_us` 在 `message_bus_publish` 里写成单调钟，`type_id` 被写成 0。
进程内两次发布相隔约 50 ms。

```text
stamp_n=2
msg0 timestamp_us=3797004912 data_size=24 type_id=0
msg1 timestamp_us=3797055009 data_size=24 type_id=0
delta_us=50097 wall_span_us=70304
publisher_t0=3797004901
```

第一条比 `publisher_t0` 晚 11 μs。
`type_id` 是 0。
我们传入的是 `LidarFrame`，类型号没有留在消息上。
源码中跨进程到达融合进程时，`ipc_to_bus_relay` 会再次调用 `message_bus_publish`，从而再盖一次戳（`src/core/transport.c::ipc_to_bus_relay:L323-L327`）。
100 ms 的配对窗口比的是融合进程里的到达时间，不是传感器采样时间，也不是 `GpsData` 载荷里的时间。

### 分支 a：协方差小，跟位姿

位姿 x 为 50，协方差 0.2。
激光雷达 x 为 80。
期望：融合 x 靠近 50，`raw_pos_x` 仍是 80。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9666, caps=0x0b, 0 topics)
case=a wire=native lidar_x=80.000 pose_x=50.000
lidar_pub=31 pose_pub=31 gps_pub=0 loc_n=60 sample_n=60 subscribed=1 elapsed_s=1.554 dropped_old=0 raw_mismatch=0
loc_span_s=1.486 loc_hz=39.716 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=49.876012 y=0.000000 v=5.620000 raw_x=80.000000 has_raw=1 cov_xx=0.249377 innovation=49.750000 diverged=0 timestamp_us=3823559169
mid idx=30 t_rel=0.767 x=50.011617 y=-0.000111 v=0.017024 raw_x=80.000000 has_raw=1 cov_xx=0.033455 innovation=0.013412 diverged=0 timestamp_us=3824326053
last idx=59 t_rel=1.486 x=50.000634 y=-0.000163 v=0.000000 raw_x=80.000000 has_raw=1 cov_xx=0.028032 innovation=0.000733 diverged=0 timestamp_us=3825044696
raw_json_first={"x":49.876012464921729,"y":0,"v":5.6199996691588883,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":49.75,"diverged":false,"raw_pos_x":80,"raw_pos_y":0,"timestamp_us":3823559064}
[discovery:ekf_chapter] stopped
```

x 停在 50 附近，`raw_x` 一直是 80。
这是分支 a。
没有 GPS，`v` 仍从 5.62 落到 0。
定位频率 39.716 Hz，接近两路 20 Hz 之和，不是配置里的 `frequency_hz`。

### 分支 b：协方差大，只预测

位姿 x 仍是 50，但 `cov_xx` 和 `cov_yy` 各为 60，和为 120。
激光雷达 x 为 80。
期望：不做位置更新，`innovation` 保持 0，`v` 保持 5，x 按 `5 × 0.05` 米一拍往前走。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9686, caps=0x0b, 0 topics)
case=b wire=native lidar_x=80.000 pose_x=50.000
lidar_pub=31 pose_pub=31 gps_pub=0 loc_n=61 sample_n=61 subscribed=1 elapsed_s=1.606 dropped_old=0 raw_mismatch=0
loc_span_s=1.488 loc_hz=40.317 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=0.250000 y=0.000000 v=5.000000 raw_x=80.000000 has_raw=1 cov_xx=100.062503 innovation=0.000000 diverged=0 timestamp_us=3826416345
mid idx=30 t_rel=0.719 x=7.750000 y=0.000000 v=5.000000 raw_x=80.000000 has_raw=1 cov_xx=160.186597 innovation=0.000000 diverged=0 timestamp_us=3827135313
last idx=60 t_rel=1.488 x=15.250000 y=0.000000 v=5.000000 raw_x=80.000000 has_raw=1 cov_xx=333.508191 innovation=0.000000 diverged=0 timestamp_us=3827904413
raw_json_first={"x":0.25,"y":0,"v":5,"heading":0,"yaw_rate":0,"cov_xx":100.062503125,"cov_yy":100.062503125,"cov_vv":25.005,"cov_hh":1.000275,"cov_yyaw":0.11,"innovation":0,"diverged":false,"raw_pos_x":80,"raw_pos_y":0,"timestamp_us":3826416268}
[discovery:ekf_chapter] stopped
```

1.488 秒里 x 从 0.25 走到 15.25，`v` 保持 5.000，`innovation` 保持 0，`raw_x` 保持 80。
位姿的 50 和激光雷达的 80 都没有成为状态。
每拍 0.25 米，输出约 40 Hz，所以状态里的 x 比「5 米/秒乘墙钟」走得快。
超车若撞上这个分支，自车横向位置是这段积分，不是邻道的测量。

### 分支 c：位姿停了，改跟激光雷达

`c-near` 的位姿 x 是 99.7，激光雷达 x 是 100。
看到第一帧定位之后 1.2 秒停止发位姿。
期望：前半跟位姿，后半跟激光雷达，差值只有 0.3 米，不会置 `diverged`。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9706, caps=0x0b, 0 topics)
case=c-near wire=native lidar_x=100.000 pose_x=99.700
lidar_pub=49 pose_pub=25 gps_pub=0 loc_n=73 sample_n=73 subscribed=1 elapsed_s=2.506 dropped_old=0 raw_mismatch=0
loc_span_s=2.398 loc_hz=30.028 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=99.452146 y=0.000000 v=6.239376 raw_x=100.000000 has_raw=1 cov_xx=0.249377 innovation=99.449997 diverged=0 timestamp_us=3829222953
mid idx=36 t_rel=0.867 x=99.712354 y=-0.000202 v=0.014289 raw_x=100.000000 has_raw=1 cov_xx=0.030413 innovation=0.014070 diverged=0 timestamp_us=3830090330
last idx=72 t_rel=2.398 x=100.054196 y=-0.000700 v=0.175380 raw_x=100.000000 has_raw=1 cov_xx=0.028031 innovation=0.061045 diverged=0 timestamp_us=3831620724
raw_json_first={"x":99.4521464962793,"y":0,"v":6.239376185040455,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":99.4499969482422,"diverged":false,"raw_pos_x":100,"raw_pos_y":0,"timestamp_us":3829222849}
[discovery:ekf_chapter] stopped
```

中点 x 是 99.712，靠近位姿 99.7。
末点 x 是 100.054，靠近激光雷达 100。
`diverged` 一直是 0。
首帧创新 99.45 被接受了，因为当时 `update_count` 还小于 100。

`c-far` 把激光雷达放到 103，位姿放在 100，1.6 秒后停位姿。
期望若按「停位姿后 3 米阶跃会把门限打爆」来写，这次没有发生。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9726, caps=0x0b, 0 topics)
case=c-far wire=native lidar_x=103.000 pose_x=100.000
lidar_pub=64 pose_pub=33 gps_pub=0 loc_n=96 sample_n=96 subscribed=1 elapsed_s=3.305 dropped_old=0 raw_mismatch=0
loc_span_s=3.154 loc_hz=30.121 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=99.751402 y=0.000000 v=6.243115 raw_x=103.000000 has_raw=1 cov_xx=0.249377 innovation=99.750000 diverged=0 timestamp_us=3832930320
mid idx=48 t_rel=1.166 x=100.004790 y=-0.000257 v=0.000881 raw_x=103.000000 has_raw=1 cov_xx=0.028350 innovation=0.005408 diverged=0 timestamp_us=3834096154
last idx=95 t_rel=3.154 x=103.570328 y=-0.010061 v=1.172277 raw_x=103.000000 has_raw=1 cov_xx=0.028031 innovation=0.642445 diverged=0 timestamp_us=3836084264
raw_json_first={"x":99.751401876903358,"y":0,"v":6.2431149145447069,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":99.75,"diverged":false,"raw_pos_x":103,"raw_pos_y":0,"timestamp_us":3832930241}
[discovery:ekf_chapter] stopped
```

中点 x 是 100.005，末点 x 是 103.570。
`diverged` 仍是 0。
3 米的切换在这 3.154 秒里被吃进状态了。
把门限打到 `diverged` 的是上面的离线 11 次离群，不是这个在线用例。

### 激光雷达停发之后

`stale` 先让位姿停在 40、激光雷达停在 100。
1.2 秒后停止发布激光雷达，位姿继续发。
期望：最新一帧的时间戳冻住，新的位姿对不上 100 ms 窗口，于是分支 3 反复吃同一帧 x 为 100 的 `LidarFrame`。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9746, caps=0x0b, 0 topics)
case=stale wire=native lidar_x=100.000 pose_x=40.000
lidar_pub=25 pose_pub=53 gps_pub=0 loc_n=77 sample_n=77 subscribed=1 elapsed_s=2.706 dropped_old=0 raw_mismatch=0
loc_span_s=2.598 loc_hz=29.257 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=39.900935 y=0.000000 v=5.495377 raw_x=100.000000 has_raw=1 cov_xx=0.249377 innovation=39.750000 diverged=0 timestamp_us=3837539252
mid idx=38 t_rel=0.923 x=40.005538 y=-0.000137 v=0.004459 raw_x=100.000000 has_raw=1 cov_xx=0.029779 innovation=0.006289 diverged=0 timestamp_us=3838462495
last idx=76 t_rel=2.598 x=97.798915 y=4.423924 v=28.734311 raw_x=100.000000 has_raw=1 cov_xx=0.112318 innovation=5.301678 diverged=0 timestamp_us=3840136915
raw_json_first={"x":39.9009345825254,"y":0,"v":5.4953766200817249,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":39.75,"diverged":false,"raw_pos_x":100,"raw_pos_y":0,"timestamp_us":3837539150}
[discovery:ekf_chapter] stopped
```

中点还在 x 为 40。
末点 x 是 97.799，`v` 是 28.734，`raw_x` 仍是 100。
旧帧被当成新的位置测量，交叉协方差把速度拽到 28.734 米/秒。
这不是「车停在最后一次位姿上」。

### 输出频率、打包长度

`rate` 不发位姿。
激光雷达 x 为 20，名义 20 Hz。
原生 `GpsData` 速度 5 米/秒，名义 10 Hz。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9766, caps=0x0b, 0 topics)
case=rate wire=native lidar_x=20.000 pose_x=50.000
lidar_pub=41 pose_pub=0 gps_pub=21 loc_n=60 sample_n=60 subscribed=1 elapsed_s=2.105 dropped_old=0 raw_mismatch=0
loc_span_s=1.950 loc_hz=30.259 lidar_hz_nominal=20 pose_hz_nominal=0 gps_hz_nominal=10
first idx=0 t_rel=0.000 x=19.950749 y=-0.000000 v=5.009470 raw_x=20.000000 has_raw=1 cov_xx=0.249377 innovation=0.246131 diverged=0 timestamp_us=3841542601
mid idx=30 t_rel=0.974 x=21.715073 y=0.003212 v=2.381264 raw_x=20.000000 has_raw=1 cov_xx=0.022479 innovation=1.884530 diverged=0 timestamp_us=3842516806
last idx=59 t_rel=1.950 x=21.462557 y=0.014132 v=1.454534 raw_x=20.000000 has_raw=1 cov_xx=0.021667 innovation=1.601448 diverged=0 timestamp_us=3843492487
raw_json_first={"x":19.950749311942072,"y":-2.9520867945991034e-09,"v":5.00947041206057,"heading":5.7868453007258259e-11,"yaw_rate":-1.5913824576996111e-11,"cov_xx":0.24937657356889265,"cov_yy":0.24937656634669977,"cov_vv":0.96152280511003241,"cov_hh":0.0196077092765297,"cov_yyaw":0.10997033301403757,"innovation":0.24613052192739815,"diverged":false,"raw_pos_x":20,"raw_pos_y":0,"raw_speed":5,"world_lat":39.9,"world_lon":116.4,"timestamp_us":3841542509}
[discovery:ekf_chapter] stopped
```

`loc_hz` 是 30.259，不是 20，也不是 100。
JSON 里有 `raw_speed` 5 和 `world_lat`。
GPS 原生长度被接受了。

`serialize` 用 `Pose2D_serialize` 发位姿（29 字节），激光雷达仍是 24 字节原生帧，x 为 80，位姿 x 为 50。
期望：尺寸检查拒绝位姿，x 跟激光雷达。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9786, caps=0x0b, 0 topics)
case=serialize wire=serialize lidar_x=80.000 pose_x=50.000
lidar_pub=31 pose_pub=31 gps_pub=0 loc_n=61 sample_n=61 subscribed=1 elapsed_s=1.606 dropped_old=0 raw_mismatch=0
loc_span_s=1.485 loc_hz=40.390 lidar_hz_nominal=20 pose_hz_nominal=20 gps_hz_nominal=0
first idx=0 t_rel=0.000 x=79.801246 y=0.000000 v=5.993869 raw_x=80.000000 has_raw=1 cov_xx=0.249377 innovation=79.750000 diverged=0 timestamp_us=3844850836
mid idx=30 t_rel=0.717 x=80.015721 y=-0.000136 v=0.024543 raw_x=80.000000 has_raw=1 cov_xx=0.033455 innovation=0.018150 diverged=0 timestamp_us=3845567976
last idx=60 t_rel=1.486 x=80.000842 y=-0.000214 v=0.000000 raw_x=80.000000 has_raw=1 cov_xx=0.028029 innovation=0.000972 diverged=0 timestamp_us=3846336350
raw_json_first={"x":79.8012461121107,"y":0,"v":5.99386881639038,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":79.75,"diverged":false,"raw_pos_x":80,"raw_pos_y":0,"timestamp_us":3844850606}
[discovery:ekf_chapter] stopped
```

末点 x 是 80.001，不是 50。
首帧创新 79.75，是从 0 拉向激光雷达 80，不是拉向位姿 50。
`slam_node` 的发布就是这条打包路径。
`slam_execute` 用 `Pose2D_serialize` 再发布（`modules/adas_nodes/slam_node.cpp::slam_execute:L377-L418`）。
按这次实验，那 29 字节到不了融合状态。

`gps-native` 发原生 `GpsData`，速度 12 米/秒，激光雷达 x 为 10，无位姿。
`gps-ser` 同样的数，但走 `GpsData_serialize`（36 字节）。

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9806, caps=0x0b, 0 topics)
case=gps-native wire=native lidar_x=10.000 pose_x=50.000
lidar_pub=31 pose_pub=0 gps_pub=16 loc_n=46 sample_n=46 subscribed=1 elapsed_s=1.607 dropped_old=0 raw_mismatch=0
loc_span_s=1.477 loc_hz=30.467 lidar_hz_nominal=20 pose_hz_nominal=0 gps_hz_nominal=10
first idx=0 t_rel=0.000 x=9.976526 y=0.000000 v=11.735335 raw_x=10.000000 has_raw=1 cov_xx=0.249377 innovation=6.878493 diverged=0 timestamp_us=3847655252
mid idx=23 t_rel=0.714 x=13.911506 y=0.005625 v=7.048651 raw_x=10.000000 has_raw=1 cov_xx=0.023748 innovation=5.325341 diverged=0 timestamp_us=3848369193
last idx=45 t_rel=1.477 x=13.769710 y=0.014709 v=4.150995 raw_x=10.000000 has_raw=1 cov_xx=0.021663 innovation=4.127401 diverged=0 timestamp_us=3849132304
raw_json_first={"x":9.97652551962234,"y":8.2500564323091664e-08,"v":11.735334902483647,"heading":-1.6172221082176742e-09,"yaw_rate":4.4473607975986287e-10,"cov_xx":0.24937657356889265,"cov_yy":0.24937656634669977,"cov_vv":0.96152280511003241,"cov_hh":0.0196077092765297,"cov_yyaw":0.10997033301403757,"innovation":6.8784925271497652,"diverged":false,"raw_pos_x":10,"raw_pos_y":0,"raw_speed":12,"world_lat":39.9,"world_lon":116.4,"timestamp_us":3847655144}
[discovery:ekf_chapter] stopped
```

```text
[discovery:ekf_chapter] started (group=239.255.0.100:5500, caps=0x03)
[discovery:ekf_chapter] auto-created 0 IPC channels
[discovery:ekf_chapter] discovered 'fusion' (pid=9826, caps=0x0b, 0 topics)
case=gps-ser wire=native lidar_x=10.000 pose_x=50.000
lidar_pub=31 pose_pub=0 gps_pub=16 loc_n=46 sample_n=46 subscribed=1 elapsed_s=1.606 dropped_old=0 raw_mismatch=0
loc_span_s=1.481 loc_hz=30.391 lidar_hz_nominal=20 pose_hz_nominal=0 gps_hz_nominal=10
first idx=0 t_rel=0.000 x=9.975701 y=0.000000 v=5.121507 raw_x=10.000000 has_raw=1 cov_xx=0.249377 innovation=9.750000 diverged=0 timestamp_us=3850560694
mid idx=23 t_rel=0.718 x=10.011118 y=-0.000051 v=0.019301 raw_x=10.000000 has_raw=1 cov_xx=0.040338 innovation=0.013257 diverged=0 timestamp_us=3851278483
last idx=45 t_rel=1.481 x=10.001316 y=-0.000096 v=0.000000 raw_x=10.000000 has_raw=1 cov_xx=0.028521 innovation=0.001489 diverged=0 timestamp_us=3852041414
raw_json_first={"x":9.97570093533642,"y":0,"v":5.1215074728502348,"heading":0,"yaw_rate":0,"cov_xx":0.24937694705991384,"cov_yy":0.24937694705991384,"cov_vv":24.989420560921477,"cov_hh":0.99965194705990823,"cov_yyaw":0.11,"innovation":9.75,"diverged":false,"raw_pos_x":10,"raw_pos_y":0,"timestamp_us":3850560606}
[discovery:ekf_chapter] stopped
```

原生 GPS 的首帧 `v` 是 11.735，JSON 里有 `raw_speed` 12。
打包 GPS 的 JSON 没有 `raw_speed`，末点 `v` 是 0。
`gps_driver` 走的是打包发布。
`gps_execute` 用 `GpsData_serialize` 再发布（`modules/adas_nodes/gps_driver_node.c::gps_execute:L112-L180`）。
按这次实验，那 36 字节不会进入速度更新。

同一次构建还跑了 `test_ekf_fusion` 和 `test_ekf_slam`。
前者末行是 `=== 汇总: 125 PASS, 0 FAIL ===`。
后者末行是 `=== Test Completed (0 failures) ===`。
单测里的模拟误差不能当成车上的定位误差。

## 这次不改的实现问题

这些都留在源码里。
本章的示例只绕开它们，或者把它们打印出来。

融合文件头写自行车模型和 `v' = v + a·dt`。
预测实现是 CTRV，`v' = v`。
头文件的调用示例也过时：`ekf_fusion_init` 实际要初值，`ekf_fusion_update_lidar` 实际要 `R`，`ekf_fusion_update_gps` 实际是速度和航向两个量。
`fusion_node.cpp` 开头的注释写「无 GPS 时 SLAM 全维更新」，还写着用协方差调节 `R`。
`FusionTask::run` 只更新 x、y，`R` 用常数 0.25。
`ekf_fusion.c` 在 `DEFAULT_Q_YAWRATE_VAR` 上面的注释写偏航角加速度方差 0.5。
宏本身是 0.01，由 `ekf_fusion_init` 写进 `Q`。

分支 2 不回退激光雷达。
`slam_node` 又把 `converged` 写死为真。
源码中位姿还在到的时候，进不了分支 3。
源码中真车配置没有 `LidarFrame` 发布者，融合循环又在激光雷达缓冲为空时直接 `continue`。
于是源码中分支 a 和分支 b 在这条配置上也不会发生：根本不会进入更新。

`message_bus_publish` 把 `type_id` 写成 0。
`_msg_cast_impl` 在类型号为 0 时改比 `data_size` 和 `sizeof`（`src/core/serializer.c::_msg_cast_impl:L215-L247`）。
`Pose2D` 差 3 字节，`GpsData` 差 4 字节。
`slam_node` 和 `gps_driver` 的打包字节会被拒绝。
本次在线用例 `serialize` 和 `gps-ser` 就是这个结果。

协方差更新是 `(I - KH)P`，没有约瑟夫形式，也没有强制对称，对角没有 `1e-6` 下限。
`test_ekf_fusion.c` 文件头写了两层机制：速度为负时翻成正，以及航向方差不低于 0.01。
`ekf_fusion.c` 里没有这两段。
单测仍然 125 项通过，是因为用例没有把速度打成负数，航向方差又被 `Q` 托在门槛附近。
不能据此说这两层存在。

`LidarFrame`、`Pose2D` 用的是车体中心还是后轴，源码没有写。
坐标是世界系还是车体前方为 x，三处注释互相不一致。
两处都不要替源码补一句。

`message_bus.c` 里序列号自旋和 `topic_mutex` 的加锁顺序，在一次启动里死锁过。
见上面「关掉之后，以及会卡住」。
这次贴出的 `all` 没有复现，所以没有「杀掉之后的输出」可以贴。

## 用的时候会踩到的地方

### 把 else 读成「否则就用激光雷达」

现象：位姿还在发，协方差之和已经大于等于 100，融合 x 却既不跟位姿也不跟激光雷达，只按 5 米/秒往前积。
用例 `b` 里 1.488 秒走到了 x 为 15.25，`raw_x` 仍是 80。
原因：内层 `if (pose_cov < 100.0)` 没有 `else`。
外层 `else` 只在「没有位姿或未收敛」时才用 `LidarFrame`。
改法：读 `FusionTask::run` 时按三个分支写下来。
不要把两层 `if` 收成一句「否则用激光雷达」。

### 文件头写着自行车模型

现象：按头文件去找转向角和轴距，预测里没有这两项。
按 `v' = v + a·dt` 去对拍，速度在没有 GPS 时也不会按加速度改变。
原因：头文件 Prediction 注释没有跟着 `ekf_fusion_predict` 改。
实现是 CTRV，`v` 和 `yaw_rate` 在预测里保持不变。
改法：以 `ekf_fusion_predict` 的五行为准。
头文件那一段要改注释时再改，本章不动它。

### 没有 GPS 时车速却掉到 0

现象：只有位置测量，`v` 从初值 5 附近落到 0。
用例 `a` 的末点 `v` 是 0.000，x 停在 50。
原因：预测的雅可比把 `v` 写进 `x` 的偏导。
`P` 出现交叉项之后，位置更新会改速度。
改法：需要速度就发能通过尺寸检查的 GPS 速度。
不要把 `fusion_init` 里的 5 当成「没有 GPS 就一直是 5」。

### 序列化字节数对不上结构体

现象：`slam_node` 在发 `sensor/pose`，融合 x 却跟 `LidarFrame`，或者 GPS 速度完全不进状态。
用例 `serialize` 的末点 x 是 80.001，不是位姿的 50。
用例 `gps-ser` 的末点 `v` 是 0，JSON 里没有 `raw_speed`。
原因：总线把 `type_id` 清零后，融合用 `sizeof` 验收。
`Pose2D` 打包 29、结构体 32。
`GpsData` 打包 36、结构体 40。
改法：在尺寸检查接受打包长度之前，发布端要发原生结构体字节数。
这是改源码才能做的事，本章没有改。

### 激光雷达停了，旧帧还在把车速拽起来

现象：激光雷达不再发，位姿还在发，融合 x 却冲向旧的激光雷达坐标，速度升到 28.734 米/秒。
用例 `stale` 末点 x 为 97.799，`v` 为 28.734，`raw_x` 为 100。
原因：`message_buffer_latest` 不丢过期帧。
新位姿的时间戳离这帧超过 100 ms 之后，查找失败，分支 3 把同一帧再喂进去。
`update_count` 还没到 100 时，大残差也不会被门限丢掉。
改法：最新帧要有过期，或者时间戳用采样时刻而不是转发时刻。
本章只把这个行为跑出来。

### 真车配置下定位 Topic 一直是空的

现象：按 `pipeline_car.json` 启动时，源码中不会发出 `fusion/localization`。
原因：`FusionTask::run` 在 `LidarFrame` 缓冲为空时直接 `continue`。
`lidar_driver` 发的是 `perception/obstacles` 上的 `ObstacleList`，而且默认 `enable` 为 0。
没有进程发 `sensor/lidar`。
另外，即便有人补了一路 `LidarFrame`，`slam_node` 的 29 字节 `Pose2D` 仍过不了尺寸检查。
改法：要么有一路尺寸正确的 `LidarFrame`，要么空缓冲时也允许只做预测。
后一条会改变分支 2 的含义，要单独设计。
本章不改节点。

## 小结

融合定位是两个独立的 EKF，加上 `FusionTask::run` 里的三个位置分支。
默认仿真多半走分支 3，因为没有 `sensor/pose`。
分支 2 会跳过位置测量。
分支 3 在位姿还在到达时进不去，因为 `converged` 被写成真。
真车配置还缺 `LidarFrame`，循环会停在 `continue`。
预测是 CTRV，`dt` 固定 0.05 秒，一拍一次，频率跟着输入消息走。
这次测到两路 20 Hz 时输出 39.716 Hz。
激光雷达 20 Hz 加 GPS 10 Hz 时输出 30.259 Hz。

[第 15 章](14_behavior_decision.md#只要还在高级模式里决策器就一直在选) 行为决策用这里的 `x`、`y`、`v`、`heading` 决定跟车还是变道。
默认仿真里这份数经常被 `vehicle/state` 盖住。
真车路径上盖不住。
超车前先确认自己落在哪一个分支。

## 练习

1. `dead_reckon` 的 `pose_cov_xx` 每秒加 0.05，从 0.01 加到上限 100。
   两轴之和达到 100 要多少秒？
   达到之后，只要位姿还在发，融合会停在哪个分支？
   用 `slam_update_dead_reckon` 和 `FusionTask::run` 的不等式算，不必把进程放够这么久。

2. 用例 `rate` 的输出是 30.259 Hz。
   若激光雷达和 GPS 都改成名义 20 Hz，而且仍然每个唤醒预测一次，你预期输出接近多少？
   用 `ekf_chapter` 改频率再跑一次，对照你的预期。
   记住 `dt` 仍是 0.05 秒，状态里的位移会比墙钟乘速度更大。

3. 从 `x' = x + v·cos(ψ)·dt` 和 `y' = y + v·sin(ψ)·dt` 写出 `∂x'/∂ψ` 和 `∂y'/∂ψ`。
   对照 `compute_jacobian_F` 的 `F[0][3]` 和 `F[1][3]`。
   说明为什么位置更新能改变 `v`。

4. 用例 `c-near` 的首帧创新约 99.45，状态直接跳到 99 附近。
   若同样的阶跃发生在 `update_count` 已经大于等于 100 之后，门限会怎样？
   离线的 `fault-gate` 已经给了 1 次和 11 次的结果。
   解释为什么 `c-far` 末点 `diverged` 仍是 0。

5. 写一份修改计划，把 `Pose2D` 的 `cov_xx`、`cov_yy` 送进 `ekf_fusion_update_lidar` 的 `R`。
   不要改仓库。
   计划里要写：分支 2 还要不要留，以及 `R` 用协方差本身还是用它的倒数。

6. `Pose2D_serialize` 得到 29，`sizeof(Pose2D)` 是 32。
   画出 `converged` 和 `source` 之间的填充。
   说明 `type_id` 为 0 时 `fusion_node` 会不会采用 `slam_node` 的那一帧。

## 勘误：本章上一版的错误说法

| 上一版说法 | 实际行为 | 源码 |
| --- | --- | --- |
| GPS、IMU 和轮速计（Wheel Odometry）进同一个滤波器 | 两个 EKF，互不调用。没有轮速计输入 | `ekf_fusion_predict` 与 `ekf_slam_predict` |
| 经纬度先投影到 ENU 再当位置测量 | 经纬度只抄进 `world_lat`、`world_lon` | `FusionTask::run` |
| 预测和输出 100 Hz | `dt` 固定 0.05 秒，一唤醒预测一次。本次两路 20 Hz 时输出 39.716 Hz | `fusion_init` |
| 厘米级位姿 | `demo` 第 40 步贴住真值，是因为测量就是刚生成的真值 | `ekf_chapter` 的 `demo` |
| 状态顺序 `[px, py, θ, v, ω]` | 只符合 `EkfState`。融合状态是 `[x, y, v, heading, yaw_rate]` | `EKF_STATE_DIM` |
| 阿克曼或角速度积分，`v` 加 `a·dt`，`ω` 等于陀螺 | 融合预测是 CTRV，`v` 和 `yaw_rate` 保持不变。SLAM 才是 `v += accel_x·dt` 且 `omega = gyro_z` | `ekf_fusion_predict` |
| 雅可比最后一行全 0 | 只符合 `ekf_slam_predict` 里的 `F[24] = 0`。融合的 `F[4][4] = 1` | `compute_jacobian_F` |
| 里程计用 `H_odom` 做速度更新 | 速度更新来自 `ekf_fusion_update_gps`，观测是 GPS 速度 | `ekf_fusion_update_gps` |
| `ekf_update_gps(EkfSlam*, x, y, heading)` | 没有这个函数。融合 GPS 更新的参数是速度和航向 | `ekf_fusion_update_gps` |
| GPS 丢失时把 `localization_status` 标成 `DEGRADED` | 只有 `diverged`。没有这个状态字段 | `FusionTask::run` |
| 每次更新后把 `P` 对称化，对角低于 `1e-6` 就抬上去 | `ekf_update_generic` 写的是 `(I-KH)P`，没有这两步 | `ekf_update_generic` |
| 航向差用 `atan2(sin, cos)` 折回 | 融合用 `while` 加减 `2π`。SLAM 用 `slam_wrap_pi` | `ekf_fusion_update_gps` |
| 环岛里航向从 +179 度跳到 -179 度，规划画出左右猛打的轨迹 | 短弧残差在 `fault-wrap` 里测到了。环岛那次路测没有对应记录可以核对 | `ekf_fusion_update_gps` |
| 没有 χ²，前 100 次更新也不设门 | `ekf_update_generic` 有门限，`update_count < 100` 时跳过 | `ekf_update_generic` |
| 协方差不小于 100 时仍用激光雷达位置 | 内层 `if` 没有 `else`，这一拍不做位置更新 | `FusionTask::run` |
| 真车配置会发布 `sensor/lidar` 上的 `LidarFrame` | `lidar_execute` 发布 `ObstacleList` | `lidar_execute` |
