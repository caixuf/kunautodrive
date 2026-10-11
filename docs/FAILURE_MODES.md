# 运行期故障模式表

> 本表是「**现象 → 根因 → 位置**」的仓库权威副本，2026-09 从 `CLAUDE.md` 迁出
> （CLAUDE.md 触及 40k 上下文上限，本表占其中 13.7k）。`CLAUDE.md` 的「常见故障模式」
> 段只留指针 —— **勿在两处各留一份**。

## 怎么用

- **按现象搜**：评估器报什么、仪表盘显示什么、车做了什么怪动作，直接 `Ctrl-F` 现象列。
- **读根因列**：写的是最终定位到的**那一层 + 那一个变量**，不是初判猜测。
- ⚠️ 标「设计如此，非 bug」的行别去"修"（例：NPC 瞬移是障碍物回收逻辑放在 100m 外）。
- **别猜层**：现象对上后再按 [ALGORITHM_VERIFY_PATTERN.md](ALGORITHM_VERIFY_PATTERN.md) 的
  分层验证阶梯定位。分层探针 + 值传播验证 + 状态锁死 + 缓存层检查是这批表的排查方法论
  （`~/.claude/skills/debugging/SKILL.md`，未随仓库分发；读不到就按此手法执行）。

## 怎么加行

每排掉一个行为异常，往对应分类追加一行，沿用 `| 现象 | 根因 | 位置 |`：

- **现象** = 可观测症状（门禁报什么 / 仪表盘显示什么 / 车什么表现）
- **根因** = 层 + 变量 + 为什么错；有修复动作的写进去（"改为…"）—— 下一行很可能踩同一个坑
- **位置** = `文件:行号`；找不到精确行号才退而写函数名或文件名

一条铁律：**根因列宁长勿省**。行号、确切参数值、多层因果链是最值钱的部分，压缩等于报废。

相关：[TROUBLESHOOTING_3D_DASHBOARD.md](TROUBLESHOOTING_3D_DASHBOARD.md)（3D 仪表盘专项）、
[MONITORING_ARCHITECTURE.md](MONITORING_ARCHITECTURE.md)、[CODE_WIKI.md](CODE_WIKI.md)。

---

## 目录

| # | 分类 | 条数 |
|---|------|------|
| 1 | 控制 / 规划 / 安全层 | 33 |
| 2 | 感知 / 点云 | 6 |
| 3 | 仿真 / 几何 / NPC | 8 |
| 4 | 仪表盘 / 监控 / 前端 | 9 |
| 5 | 评估器 / 门禁 | 6 |
| 6 | 框架 / 工具链 | 4 |
| | **合计** | **66** |

## 1. 控制 / 规划 / 安全层

`control_node` / `planning_node` / `behavior_planner` / `safety_control_node` / `navigation` —— 职责边界见 CLAUDE.md「模块职责铁律」。

| 现象 | 根因 | 位置 |
|------|------|------|
| 车速降到 0 后永久卡死 | ROAD_GUARD 低速恢复条件要求 `|y|>=road_center_limit`，但车可在任意 `2.1<|y|<2.5` 停下。改为只要 `speed<2.5` 就给小油门 | `control_node.cpp:534` |
| 红灯停稳后灯转绿也不走（评估器双峰：要么跑完 x≈306，要么卡死 x≈180） | planning 用 `spd_out[0]`（≈当前车速）覆盖 `command_speed`，停稳后 `v=0 → target=0 → 油门=0 → v=0` 自维持闭锁；TL override 只置 0 从不恢复，解不开。改为取 max | `planning_node.cpp:787` |
| MPC 输出每帧翻符号（bang-bang），调 `r_ddelta` 无效 | 求解器内部 `max_steer=0.35` 而外部限幅 ≈0.027，差 12.9 倍。解从不触及自身约束边界，平滑项全部失效。改为 solve 前 `mpc_set_max_steer()` 注入真实限幅 | `control_node.cpp:1372` |
| 变道冲出车道 | Stanley heading 阻尼硬编码 0.5，pipeline.json 的 `lat_kd_heading` 未生效 | `control_node.cpp:548` |
| steer 打到 0.25 硬限幅导致抖动 | 运动学自行车模型下 heading 漂移可达 0.8 rad，steer 限幅过紧导致控制器累积误差撞 clamp。修复：`lc_lat_accel_max` 从 2.4→4.5，`steer_min_clamp` 从 0.016→0.030 | `control_node.cpp:1245-1253` `pipeline.json:198` |
| 控制遇到慢车不减速、油门全开撞前车 | 积分饱和 anti-windup 逻辑错误：刹车分支 `g.integral < 0` 应为 `g.integral > 0`，`-= error*dt` 应为 `+=`。加速阶段积分累积到 +500，进入减速后 P term 不足以抵消 I term，油门全开撞车。修复：error 翻负时直接清零正积分 | `control_node.cpp:550-554` |
| behavior planner 一直不进 FOLLOW | `worthwhile = blocked && (best_gap < min_gap)` 是反逻辑——等 gap 小于 15.6m 才觉得"值得超车"。高速 8m/s 接近速度下只剩 ~2s，变道来不及。修复：改为 `best_gap > min_gap`，阈值提至 base=25m，mult=2.0 | `behavior_planner_node.cpp:516` |
| 变道减速甚至全刹、超车没意义 | behavior 变道中 P5 分支 `else if (blocked) target_speed = lead_speed`：变道转移首帧 blocked=1 且前车停着（等红灯）→ target=0，后续 blocked=0 帧无分支重置 → 锁死 → planning command_speed=0 → 全刹（实测 spd=10.7→0.0 brk=1.00）。修复：删除该分支，防追尾改由 planning TTC + safety_control 近场 TTC 兜底 | `behavior_planner_node.cpp` 变道分支 |
| 超车/归位后立刻在红灯前刹停（无效变道） | 归位/超车决策不看目标车道前方红绿灯，切回内侧道（灯只管辖 y_lane=-1.75）即刹停。修复：behavior 订阅 road/traffic_lights，`lane_ahead_stop_light()` 检查目标车道前方 60m 内非绿灯则不归位/不变入 | `behavior_planner_node.cpp` `lane_ahead_stop_light` |
| 多车道高速上遇对向车刹停到 0（会车让行过度保守） | planning 会车让行 + safety_control 对向 TTC 都用 `\|dy\|>2.0` 判"对向车"，把对向**任意车道**（含 2+ 车道外，横向 10.5m）都当迎头威胁 → 巡航压到 0.4×~1.0 全刹。修复：两者都加横向相邻上界（planning `1.5×路宽`、safety `6.0m`），只把相邻车道的对向车当真威胁 | `planning_node.cpp` 会车让行 / `safety_control_node.cpp` `min_oncoming_ttc` |
| 红灯不停直接闯（planning override 设 0 无效） | planning 红灯 override 在 `spd_out` 生成**之后**才设 `command_speed=0`，但轨迹 `points[].v` 读的是旧 `spd_out`（`spd_out = v0*(1-t)+command_speed*t` 已按巡航速度算完）→ control 拿到的还是巡航速度。一直被"跟停红灯前停着的车"掩盖。修复：override 触发时**同步重建 spd_out**（当前速度→0 减速斜坡），轨迹末点归零 control 才真正刹停 | `planning_node.cpp` 红灯 override（~line 1209） |
| 变道纠结：超车后立刻归位回慢车道、再超再归位（必现） | 归位只查目标车道 gap（空/远），不查目标车道**车速**——超车到快车道后切回慢车道，立刻又被慢车堵、再变道再归位，120s 必现 3 个来回。修复：归位加"目标车道可用"条件——空旷（无前车）或前车速度 ≥ 0.7×巡航才归位，否则留在快车道巡航 | `behavior_planner_node.cpp` 归位分支（~line 774） |
| 右转/支路场景 control 跟踪坏轨迹（偏出路沿） | flowsim ref_path 用 lane centerline（含当前车道偏移）→ planning 再加 target_lane_offset → 双重偏移。修复：ref_path 优先用 route centerline（d=0=道路中心），planning 用 project_to_reference_path 把 ego 投影到 map_ref 参考线 Frenet 弧长，control 直接取 trajectory 前视点的全局 y 替代 road_center + lane_d 混拼 | `flowsim_node.cpp` publish_ref_path / `planning_node.cpp` project_to_reference_path / `control_node.cpp` target_path_y |
| 掉头返程方向误判（travel_dir 反复翻转、route 步骤重放把 ego 拽离对向车道绕圈） | navigation 用 dx/heading 猜测行进方向：三把方向掉头（2s+3s+0.5s）中 heading 扫过 ±x 两个半平面各驻留 >0.8s，8 帧去抖压不住 → travel_dir 中途翻 +1/-1 → next_idx 归零重放 enter_noa/prepare_u_turn、A* hop 把返程中的 ego 拽回前进车道。修复：**方向唯一事实源 = flowsim 权威 `road/ref_path.reverse`**（`u_turn_active` 掉头 finalize 置位、整段返程保持、永不抖动），navigation `on_ref_path→on_return` 据此翻 travel_dir，behavior 返程锁 committed_lane 抑制变道，control target_lane_center 乘 cos(ref_road_heading) 返程符号自洽 | `flowsim_node.cpp` publish_ref_path（reverse 字段）/ `navigation_node.c` on_ref_path / `behavior_planner_node.cpp` on_return / `control_node.cpp` target_lane_center |
| 掉头返程幽灵刹车 + 无同向防撞（撞旁边车） | **safety 层系统性用世界 +x 坐标，掉头返程（向西）全部失效**：`min_oncoming_ttc` 用世界 obs_v<−2 判"迎面"→ 返程同向车（vx 也<0）被误判迎头 → head-on TTC 硬刹（幽灵刹车，实测返程 #4200 突然 brk=1.00 MRM）；`min_vehicle_ttc` 用世界 dx=obs_x−ego_x → 返程前车在 −x（dx<0）被 skip → 同向防撞完全失效 → 追尾 entity14（实撞）。planning 的会车让行已在 1221fad 用方向投影修复，safety 漏了。修复：safety 4 函数（min_vehicle_ttc / min_oncoming_ttc / nearest_same_lane_gap / nearest_vehicle_lateral_cross_risk）全部改沿车头方向投影（ahead=ex·cos h+ey·sin h、lat=|−ex·sin h+ey·cos h|、along_v=obs_v·cos h+obs_vy·sin h）；前进方向（heading=0）投影退化为原世界坐标零回归 | `safety_control_node.cpp` min_oncoming_ttc / min_vehicle_ttc / nearest_same_lane_gap / nearest_vehicle_lateral_cross_risk |
| 掉头撞路沿护栏 | 掉头距离兜底触发 `v≤7` 在车仍以 6.8 m/s 进弯时就触发（safety 幽灵刹车打断接近减速所致）→ 进弯太快 → 掉头弧外甩，车头角点（中心+半车长~2.2m）甩出路沿 7.0m。修复：兜底触发速度 7→5 m/s，车须减速到 5 才触发（实测进弯 4.8，OFFRAILS exit y=3.24 路内） | `behavior_planner_node.cpp` uturn 距离兜底触发 |
| 行为决策整块静默 30s（该掉头不掉、该变道不变），而车仍在动 → 所有量活性门禁全绿 | `behavior_planner on_raw_obstacles` 收到**空**障碍物列表时直接 `return`（把"空"当"感知没就绪"）→ `has_obs` 恒 0 → `run()` 前置守卫跳过**整个决策块**（含路端掉头触发 `UTURN_TRIGGER`）。sensor 编排下 ego 背对视锥起步（前方空路）必现 → 车开过路网尽头冲出路面（评估器 road departure 6.80m/74 帧）；`curve_road` 在 GT 编排里也一直静默（committed 基线 `behavior_state: "NA"`）。修复：空列表=**合法结果** → `obs_count/raw_obs_count 归零 + has_obs=1`；配套门禁 `behavior planner never decided`（全程 NA→FAIL / 过半→WARN），且在**未修 C++ 的 A/B** 里首次抓到 curve_road | `behavior_planner_node.cpp` `on_raw_obstacles` / `demo_evaluator.py` 行为活性判据 |
| 幽灵刹车：本车道前方 80m 内"有静止障碍物"，实际空无一物（起步被压到 3.7 m/s） | `planning on_perception_obstacles` 每帧清零 128 槽后只填 `list.count` 个，而 TTC 跟车 / 窄路会车 / Frenet 注入 / ST 图四处循环**扫满 128 槽** → 未用槽 `(0,0)` 被当成"世界原点有障碍物"（同一份数据在 `forward_space` 里另有一条 `(0,0) continue` 豁免，一个文件两套有效性语义）。实测 `TTC follow: gap=20.0 -> 3.7 m/s`（ego 起点 x=20 朝 −x，原点恰在前方 20m 同车道），两种编排都命中。修复：引入权威计数 `g.obs_count`（与 `truth_obs_count` 对称），四处循环改 `i < g.obs_count`，删豁免 | `planning_node.cpp` `on_perception_obstacles` + TTC/会车/Frenet/ST 四处循环 |
| 单向直路巡航 ego 漂出前进车道（`committed_lane` 2→1→0、y 从 −1.75 涨到 +5.3 越参考线；长跑 soak 全程压线） | **单向路两套车道横向布局相反**：`lane_center_d` 把车道对称铺在参考线两侧（idx0=+5.25），而 flowsim/OpenDRIVE 单向路全部车道在 −y 侧（`json_to_xodr.py` 只生成 `<right>`，lane −1..−4 于 −1.75…−12.25）→ idx0/1 映射到路面外；叠加 behavior"超车后归位"分支缺 `!road_oneway` 守卫，每帧触发往 idx−1(+y) 的 LEFT_CHANGE。修复：新增 `lane_group_side_offset(N,w,oneway)=−N·w/2`（单向；双向 0），`lane_center_d`/`nearest_lane` 增 `side_offset` 形参；归位分支加 `!road_oneway`；单向 idx0=−1.75…idx3=−12.25 与物理车道对齐 | `planning_coordinates.h` `lane_center_d`/`lane_group_side_offset`；`planning_node.cpp`/`behavior_planner_node.cpp`/`flowsim_node.cpp` 调用点；`behavior_planner_node.cpp` 归位分支（~line 1339） |
| **压车道线：CRUISE 状态骑在 2/3 号车道线上不动，`steer` 恒 0，control 看起来"很听话"** | **缺陷在 planning，不在 control。** 有 `lane_ref` 时"强制锁车道中心"整块被跳过，居中全交给 Frenet；但 Frenet 横向走廊 `max_road_width_l/r = 6.0m`（≈1.7 个车道宽）**无任何车道边界约束**，`d_road_w=1.5` 的格点又恰好让落点等于车道线。实测 `dense_route_traffic` 280s：ego 停在 `ego_d=-3.24`（车道线 -3.5，车身宽 2.0m 直接压线），planning 发布 `d_start≈d_lookahead≈d_end≈-1.5` 的**恒定偏移直线**、全程不收敛 → control 忠实跟随 → steer=0 → 每帧从当前位置重规划又得到 -1.5 → **自锁**（压线 125 连续帧）。**排查陷阱**：control 层一切正常（steer/限幅/Stanley 均无异常），只看 control 会误判成"车自己不开转向"；必须看 `planning/debug` 的 `d_start/d_lookahead/d_end` 是否收敛。**修复不能只夹紧**：夹紧是恒等映射，ego 会永久停在带沿同样压线（Python 反例验证），必须"安全带内放行 Frenet（保 W1 车道内避障）、带外强制回 d=0" | `planning_node.cpp` d_out 后处理块（`lane_ref_count < 2` 分支的 `else`）+ `frenet_bridge.cpp` `max_road_width_l/r` |
| **MRM 发出后车不停，1~2s 就宣告"停稳完成"并退回 OFF，随后重新加速**（L3：车还在 20 m/s 时 `tor.state=MRM→OFF`，`mrm.active` 只闪 2 帧） | **`tor_manager` 读错车速字段名**。`flowsim` 的 `vehicle/state` 车速字段是 **`spd`**；全仓其余消费者（control/planning/perception/behavior/sensor_model/object_tracker/monitor）都读 `spd`，唯独 `tor_manager_node.c` 读 `"speed"` → `g.speed` 恒 0 → MRM 停稳判据 `fabs(speed) < 0.1` **在进入 MRM 的那一刻就为真** → 立即 `MRM_COMPLETE` → `OFF` → planning 重新给速度。**排查陷阱**：只看 `tor.state` 序列（`ACTIVE→TOR_REQUESTED→MRM→OFF`）会以为链路全通，必须同时看 `mrm.active` 持续帧数 + 车速曲线。修复：以 `"spd"` 为准、保留 `"speed"` 作兼容回退。**门禁教训**：只断言"MRM 状态出现过"会全绿放过本缺陷，必须断言"MRM 生效后车速收敛到 ~0" | `modules/adas_nodes/tor_manager_node.c` `on_vehicle_state` |
| **降级逻辑存在但遥测里看不见**：注入 `planning/trajectory` 后评估器只能看到 `driver_mode` 停在最后一个正常值，`control_debug` 整块缺席，误判"降级路径没走到" | `control_node.cpp` 的 planning 掉线分支（`DATA_TIMEOUT` 车道保持 fallback）在**发布 `control/debug` 之前** `continue` → 该分支永不发布 debug，而 `control/debug` 是 monitor 拿 `control_debug`/`driver_mode` 的**唯一来源**。**排查陷阱**：日志里能看到 `#N DATA_TIMEOUT — lane-following fallback`（LOG_WARN 在 continue 之前），于是以为"路径在跑、只是评估器没读"，实则遥测里根本没有这条路径。修复：在 fallback 分支内补一条最小 debug（只带本分支实际持有的量，不复用常规块——那块的局部变量在 fallback 路径上不存在），频率与常规块一致（2Hz）。**通用教训**：在 `continue`/`return` 提前退出的降级分支里必须显式补遥测——**"有降级逻辑" ≠ "降级可被验证"** | `control_node.cpp` DATA_TIMEOUT fallback 分支（`continue` 前） |

| **全体源静默时 fusion 一帧都不发**：注入 `sensor/gps` + `sensor/lidar` 后 `fusion/localization` 频率归零，融合日志 `0 fused frames` —— 本节点自己的输入丢失在遥测里**完全不可见**，下游只看到"定位没了"却拿不到"为什么没的" | `fusion_node.cpp` 的 `if (!lidar_msg)` 分支里，`if (!pose_only && !gps_only) continue;` 把"所有源都停发"当成"没什么可做"直接跳过 → 永不发布。**讽刺的是紧邻的注释正是在警告这个"最贵的失败形态"**，但只在"无 lidar 但有 gps/pose"那条路上兑现了。修复：全丢也照发一帧（仍做 `ekf_fusion_predict` 纯惯性递推保持状态连续），用定位完整性字段如实报 `all_sources_lost` 并驱动 `DEGRADE_REASON_LOCALIZATION` | `fusion_node.cpp` `if (!lidar_msg)` 分支的"全丢"路径 |
| `fusion/localization` 有**两个**发布点，只给一个加诊断字段 → 另一条路径的场景里字段整体缺席（真车 `lidar_driver` 未使能时定位完整性在遥测里查无此项） | 正常路径与"无-lidar 降级路径"各自 `cJSON_CreateObject` + `transport_publish` 一次。给正常路径加了 `loc_*` 后忘了降级路径。**与上一轮 control 的 DATA_TIMEOUT fallback 漏发 debug 是同一类缺陷**：在多发布点上加遥测字段，必须逐点排查。修复：两条路径都补 `loc_trustworthy`/`loc_critical`/`loc_reasons[]`/`gated_count`/`gated_ratio` | `fusion_node.cpp`（两处 `transport_publish(..., "fusion/localization", ...)`） |
| 节点在 C 代码里 `transport_subscribe()` 了某 topic，但三份 `pipeline*.json` 的 subscribe 列表没同步 → 订阅在跑、拓扑里恒无该数据块 | 只改 C 侧不改配置。`topic_contract_check.py` 就是为此存在：它比对 `pipeline*.json` 的 subscribe/publish ↔ 插件 `s_inputs`/`s_outputs`，本轮当场抓出 `monitor` 漏登记 `fusion/localization`（4 issue）。修复：三份 pipeline 的 monitor subscribe + `s_inputs` 同步补上 | `config/pipeline{,_sensor,_windows}.json` / `modules/adas_nodes/monitor_node.c` `s_inputs` |
| ODD 判"定位健康"只看消息龄期 + `diverged`，而融合已判 `loc_trustworthy=false` 时 ODD 仍认为在域内 | `odd_monitor` 的定位项只消费 `diverged`（该标志要 χ² 连续失败 >10 或协方差 trace>1e9 才置位）→ 中间地带（双源互相矛盾、观测被大量拒绝）对 ODD 完全不可见。修复：消费 fusion 的交叉校验裁决，新增 `loc_untrustworthy` violation（ODD 是"在不在域内"的权威，定位不可信属域外） | `odd_monitor_node.c` `on_localization` / 分类器定位项 |

| **PEM 有事件框架却一条 L3 事件都记不下来**（production 管线跑完 L3 场景，PEM 里只有 `degrade_transition`） | EDR 的事件源订阅（`odd/state`、`tor/state`）被关在 `if (!g.embedded_mode)` 块内，而 **PEM 写出恰恰只在 embedded（production）模式** —— 两个条件**永不同时成立**。修法：两条订阅移出该分支（体量极小，两种模式都订）。**通用教训**：`embedded_mode` 这类"模式开关"两侧的依赖要成对审查 —— 只在 A 模式采数据、只在 B 模式消费，是静默失效的经典组合 | `monitor_node.c` `if (!g.embedded_mode)` 订阅块 / EDR 写出块（embedded 分支内） |
| 新 `src/core/*.c` 漏加进 CMake `CORE_SOURCES`，`plugin_symbol_check.py` 仍报 OK，直到 `dlopen` 才 'symbol lookup error' | 该闸门只查"插件**定义**但未解析的项目符号"，漏了"插件**引用**而核心库未编译进去"这一向（反向依赖）。实测：`nm -D --undefined-only build/lib/libmonitor_node.so` 里 `l3_edr_tor_state_code` 为 `U`，而闸门报 "OK (36 plugins)"。**排查手法**：加新 core 源文件后，除了跑闸门，还要 `nm -D --undefined-only` 检查受影响 .so 有无项目符号未解析 | `CMakeLists.txt` `CORE_SOURCES` / `ci/gates/plugin_symbol_check.py` 覆盖盲区 |

| **安全闸门/感知链掉线无人发现**：`safety_control`、`sensor_model`、`perception`、`object_tracker`、`behavior_planner` 全部静默，而 `metrics.degrade` 显示"心跳都健康" | supervisor 的递进策略（L1/L2/L3+去抖+自动恢复）本身完整，但**只有 3 个节点上报心跳**（control/fusion/planning）—— 覆盖面而非机制的问题。`safety_control` 是最后一层保护，它掉线最不该无人发现。修复：新增 5 个上报点，覆盖面 3→8；并给 `reason_for_node` 补感知链映射（`sensor_model`/`perception`/`object_tracker` → `sensor_timeout`），否则感知掉线会报 `control_timeout` 把人指向明明在跑的 control | `degrade_ladder.c` `reason_for_node` + 各节点的 `degrade_supervisor_record_heartbeat` |
| **第 17 个节点起的心跳被静默丢弃**，而默认 `pipeline.json` 恰好有 19 个节点 → 运维侧看到"心跳都健康"的假绿 | `DEGRADE_MAX_NODES` 为 16，`degrade_supervisor_record_heartbeat` 在表满时直接 `return`（**不报错**）。这是本仓库反复出现的"最贵的失败形态"：静默截断被当成正常。修复：上限 16→32（与 `HEALTH_MAX_NODES` 对齐），表满改 `LOG_ERROR` 点名（带防刷屏） | `include/degrade_ladder.h` `DEGRADE_MAX_NODES` / `degrade_ladder.c` 心跳注册分支 |


| **前方静止障碍把 ego 永久堵死**（`stagnation 44.6s` / `lane_change_count=0` / avg_speed 0.40 m/s / behavior 恒 `FOLLOW`） | `behavior_planner` 超车价值判据 `worthwhile = blocked && (best_gap > min_gap)` 而 `min_gap = min_overtake_gap_base(25.0) + rel_speed×2.0`。ego 正常跟停在 desired_gap≈5m（实测 7.2m），`min_gap` 却是 25m 起跳 → `best_gap > min_gap` **恒不成立** → 永不评估变道 → 永久堵死。**关键洞察**：该判据的语义（"前方留够空间才**值得**超车"）对**运动**前车成立（逼近过程 best_gap 从大到小穿过阈值），但对**已停在正前方**的障碍物无解 —— **车越守规矩停得越近，越永远不绕行**。修复：前车速度 < `min_overtake_lead_moving_mps`（新参数，默认 0.5 m/s）即视为已停住 → 绕行价值恒真，可不可行交给下游变道安全评估（目标车道 rear/front gap）。修后实测 avg_speed 0.40→14.26 m/s、变道 0→4 次 | `behavior_planner_node.cpp` `worthwhile` 判据（`min_overtake_lead_moving_mps`） |
| 场景写了 `allow_npc_lane_change: true` 想让 NPC 切入 ego 车道，NPC 却始终在原车道直行 | 该开关在 `scene_events.cpp` 的注释里被写为"未来需要时再加"，但**从未实现** —— NPC 横控被无条件归零（配合 MOBIL `#if 0`）。于是任何"NPC 切入"的法规场景（UN R157 cut-in/cut-out）在结构上写不出来，写了也只测到"没被加塞"（假绿）。修复：`Choreography.allow_npc_lane_change` 字段 + 解析 + 消费点条件判断，默认 false 保既有场景零变化。**另**：cut-in 应用 actor 的 `vy`（连续横向速度），**不要**用 choreography 的 `dl` 传送 —— 传送是位置跳变，实测会直接撞上 ego | `include/scenario_loader.h` `Choreography` / `src/core/scenario_loader.c` / `flowsim/scene_events.cpp` `effective_dl` |

| 同一场景 **ground_truth 编排能跑、sensor 编排（真点云链路）跑不动**（`r157_cut_out`：GT avg 13.63 m/s / x_delta 532m / 变道 4 次 vs sensor avg 0.54 m/s / x_delta 21m / 变道 1 次，behavior 停在 `CRUISE`、`obs_count=1`） | sensor 编排下 ego 背对视锥起步、DBSCAN 聚类出的目标数远少于 ground_truth 直通；`obs_count=1` 时 behavior 的加塞/绕行逻辑缺少可评估的**邻道**目标，可能根本进不了变道评估分支。**排查手法**：对比两种编排的 `metrics.behavior.obs_count` 与 `behavior_state` 序列，而不是只看最终 x_delta。**注意**：既有 sensor 门禁场景（urban_challenge / dense_npc / lane_change_traffic）全 PASS，故这是**新场景超出 sensor 编排现有能力**，不是回归 | `config/pipeline_sensor.json`（sensor 编排）/ `behavior_planner_node.cpp` 左右车道评估分支（依赖邻道目标） |

## 2. 感知 / 点云

`sensor_model_node` / `perception_node` / `lidar_scan` —— 点云链路（真点云编排 `config/pipeline_sensor.json`）。

| 现象 | 根因 | 位置 |
|------|------|------|
| 感知降频 | DBSCAN 点云过多时聚类耗时超过 deadline | `perception_node.cpp` |
| sensor 模式改了生产者量程却"没效果"（远处目标一个都不出） | **量程门有两份实现**：`sensor_model` 与 `perception` 各有一个 `lidar_max_range_m`，消费者默认 **60m**，把生产者发来的 >60m 的点**静默**全丢（无日志）。实测 60→120 同步后锥内识别率 37.0%→86.9%（本 Phase 收益最大的一处）。属 CLAUDE.md 铁律"同一功能第二份实现=违规" | `perception_node.cpp:495` 默认值 / `config/pipeline_sensor.json` 必须两处同值 |
| 相邻车道两台车被聚成一个 width>15m 的"车"，中心落在两车之间 | ①测距噪声写成 `σ=0.08·range`（33m 处 2.6m，糊过 1.5m 车间空隙）；②DBSCAN `eps=2.0 > 相邻车道空隙 1.5m`（车道中心距 3.5m、车宽 2m）。修复：`σ = noise_std_m + noise_rel·range`（0.002/m）、`dbscan_eps` 收到 1.2 | `lidar_scan.c` 噪声模型 / `config/pipeline_sensor.json` 的 `dbscan_eps` |
| 点云永远 z=0、装在 1.8m 高却看不见 1.5m 高的车 | 旧观测模型只发射**单层方位射线**（elevation 恒 0），AABB 求交也是 2D、不检查 z。改为"方位 × 仰角层"3D 射线 + 3D AABB；仰角层间距须 ≤ 目标在 R 处的张角（车 1.5m 在 120m 处 ≈0.7° → 至少 32 层/22°） | `lidar_scan.c` `channels` / `sensor_model_node.c` |
| sensor 模式"预警提前量"恒为 0（且伪造 critical event） | `perception_clusters_to_obstacles` 用 `frame_id*100+ci` 当障碍物 id → 每帧都变，跨帧跟踪失效。ground_truth 模式用 `vehicle/state` 的 `oid%d` 已修，sensor 模式无 oid 可用，需给簇做跨帧关联或用 tracker 的已确认航迹 id | `perception_points.c` / `ci/evaluators/demo_evaluator.py` 预警提前量 |
| 真点云写点越界（潜在崩溃） | 旧射线循环 `cloud.points[cloud.count++]` **无容量守卫**；`n_rays=240 < 2048` 时侥幸没炸，提高射线数/密集场景（42 actor）即越界写。修复：写满即停 + `dropped` 上报 | `lidar_scan.c` `lidar_scan_generate` |


## 3. 仿真 / 几何 / NPC

`flowsim` / `physics` / `npc_ai` / `route` / XODR 生成 / 地图几何。

| 现象 | 根因 | 位置 |
|------|------|------|
| NPC 瞬移 | 障碍物回收逻辑放入 100m 外（设计如此，非 bug） | `flowsim/npc_ai.cpp:204` |
| NPC/车飞出路面、不在车道上、坐标飞到几千米外 | flowsim NPC 用 `step_bicycle(steer=0)` 世界系直线积分、不跟道路几何，路一拐弯就直线冲出路网。已改为中央 `Route`（把各 road 连成有序主路）+ Frenet 沿车道推进 + 到头回收 | `npc_ai.cpp` step_npc_vehicle / `flowsim/route.cpp` |
| 车身左右晃动（1-2Hz 极限环） | 历史根因：`road_pos.world()` 每帧把 ego.heading 重置为道路切线，control 的 `v_lat_damp` 失效（heading_err≈0），退化为纯 P。**96447a9 起已改为两模式都自由积分 heading**：运动学模式由 step_bicycle 积分，靠 `sin(dh)` 负反馈闭环 + cte/heading 项 + 低通 + 死区稳住（曾尝试自由积分导致斜行后回滚，后加 sin(dh) 反馈再启用）；动力学模式由轮胎侧偏力积分。故 `is_dynamic` 分支对两模式一视同仁，只做 heading 归一化 | `flowsim_node.cpp` 主循环 ego 段 |
| 内部巡航 fallback 输出大 steer | `internal_cruise_control` 用 `road_h - heading` 全量前馈，运动学模型下 heading 漂移可达 0.8 rad，公式输出 0.8 被 clamp 到 0.25。修复：改用 `heading_err*0.3 + yaw_damp + lat_err*0.03`，cap 降到 0.15 | `flowsim_node.cpp:1007-1027` |
| **同向 NPC 车头朝后（逆向行驶）**，与相邻真实车道侧刮成堆；invariant `motion_direction` / Δs 符号失败 | 自动补给（D3-2）落点用 **esmini lane 0** —— OpenDRIVE 参考线（`type="none"`，不可行驶），esmini 对它的 `pd.h` 恒为 **π**，而 `RoadPosition::world()` 直接返回该 `data.h`，同向车（`route_dir=+1`）车头翻 180° → 逆向行驶 + Δs<0。且 lane 0 与 lane −1 中心仅差 1.75m < 车宽 2.0m，两车道车并列即侧刮。修复：`drivable_lane_ids()` 取本段可行驶车道 → 只留**负 id**（本仓右行地图的行进方向，正 id 在对向）→ 按 `abs(id)` 升序（由内到外，交替铺开落在相邻真实车道）→ 枚举为空回退 `{-1}`；`lane_spread` 只决定"第几槽"（`traffic_density_lane_slot`，恒 ≥0），lane_id 映射归 flowsim —— **与 `npc_ai.cpp` P3（硬编码 lane_id=0）/ P1 同一族 bug**。⚠️ 同一模式仍在 `step_poisson_traffic`：它也 `frenet_to_world(rid, 0, …)`，随后 `world_to_frenet` 反查回真实 lane_id（危害被部分掩盖），但 `e.heading = wp.h`（=π）已用于 `e.vx/e.vy` 初值 —— 本次未覆盖 | `flowsim_node.cpp:656`（dir_lanes 表）/ `traffic_density_spawn.h:75` / `npc_ai.cpp:406` |
| NPC 集体压在 ego 出生点、bbox 重叠、瞬移；invariant 一次爆 **121 条**（spatial+motion+temporal 三类混合） | 自动补给候选点**无清距检查**：第 0 个候选 `s≈0` 正是 ego 出生点，落进去后碰撞分离把两车挤开 → 双方车头与所在车道方向相反 + bbox 重叠 + `Δpos ≫ v·dt`，一条根因引出三类 invariant。另：s 直接拿 `i*spacing` 当 esmini s，漏掉 `seg.s0`（路口 fillet 的段首修剪偏移）→ 在被修剪过的段上把车放到段外。修复：候选点与 ego 用**世界坐标**距离、与其他车用 `route_s` 差，均 < 25m 即跳过（与 `step_poisson_traffic` 第 5 步同口径；ego 不走 `npc_init_route`，`route_s` 无记账，故不能同域比较）；同时把 s 统一到两个域 —— `s_esmini = seg.s0 + s_local`（`frenet_to_world` 要），`route_s_cand = seg.s_start + s_local`（与手列 actors 同域，清距检查用） | `flowsim_node.cpp:705`（清距块）/ `flowsim_node.cpp:692`（s 双域） |
| S 弯不跟弯（curve_road 场景车沿 y=-1.75 直开，heading 恒 0） | **四层连环**（2026-08-04 排查）：① `json_to_xodr.py` roads_from_road_network 只认 curvature_profile/length_m，完全忽略 `road_network.edges[].nodes` → 生成直道 XODR → ref_path 全 y=0；② 即便 XODR 弯了，control 横向目标 `target_path_y` 是轨迹 0.5s 前视点绝对 y，弯道上该点比 ego 当前位置高 → lat_error 虚高 → 车往弯内侧漂 ~3m；③ `scene_pub.cpp` `ROAD_NODES_PER_EDGE=8` 固定采样 → 前端 CR 过 8 点严重过冲、评估器弦长偏离真值 ~14m → 车在车道里被判 road departure；④ demo_evaluator 逆行/横向摆动检查假设直路（y<0 朝东 / 绝对 y 范围<4.5m），S 弯 ego y 合法扫过 ±103 却 heading 恒朝东 → 误报 WRONG-WAY/lateral excursion。修复：nodes 折线 → 三次 Hermite（端点切线 = 相邻 chord 平均，**不用 CR**——coarse 节点 CR 过冲生成 R≈19m 发卡弯，a_lat≈21m/s² 拐不过来；Hermite min R≈546m 可跟）密采样 5m → 逐段 line；planning map_ref 分支 kappa 从恒 0 改为切线中心差分恢复前馈；control 横向目标改用 query_ref_at **本地**参考（离 ego 最近轨迹点），cruise_lane_y = 本地 road_c + lane_d·cos(h)，只有本地查询失败才回退前视点；scene_pub 节点数按长度自适应（~25m 一点，8..128）；curve_road.json nodes 由 13 粗点重采样为 194 平滑点（前端 CR 与物理 Hermite 偏差 5.7m→<0.8m）；评估器弯道用局部 road_heading / road_signed_offset 替代绝对 y 启发 | `tools/json_to_xodr.py` build_polyline_road / `planning_node.cpp` frenet_to_cartesian / `control_node.cpp` query_ref_at 覆盖块+cruise_lane_y / `flowsim/scene_pub.cpp` road_nodes_per_edge / `scenarios/curve_road.json` / `ci/evaluators/demo_evaluator.py` |

| 场景里声明了 `fault_injection` 但注入**完全没生效**（`dropped=0`、目标 topic 照常发布） | hook 放在了 `flowsim` 的 **reset 函数**里，而那是**场景加载之前**执行的 —— 此时 `g.scenario` 仍是 nullptr，条件判断直接短路。**通用教训**：依赖"场景已加载"的逻辑必须放在 `g.scenario = scenario_load(...)` **之后**；reset/init 早于加载是容易踩的时序陷阱。修复：移到场景加载语句之后（实测 `dropped` 由 0 → 603） | `modules/adas_nodes/flowsim_node.cpp` 场景加载之后的注入应用点 |

## 4. 仪表盘 / 监控 / 前端

`monitor_node` / `monitor_server` / `flowmond` / FlowBoard（`tools/flowboard/**`）。

| 现象 | 根因 | 位置 |
|------|------|------|
| 仪表盘/3D 一直 "Waiting for data"，curl 却有数据 | 仪表盘 JSON 是 cJSON_Print 多行格式，SSE 单 `data:` 帧发送被 EventSource 按行丢弃，浏览器只收到 45 字节前缀。已在发送前压平为单行，详见 [排查文档](docs/TROUBLESHOOTING_3D_DASHBOARD.md) | `monitor_server.c` handle_sse |
| 3D 场景整屏黑（curl 有数据、console 报 `Unexpected token 'export'`） | MVC 重构（c5e4ba9）拆 Controller 层时 `_renderFrame` 相机块漏闭合一个 `}`，scene3d.js 顶层 `export` 被当块内语句、整模块编译失败不执行 → `init3DScene` 未导出。已补回 | `scene3d.js:2159` 附近 |
| 仪表盘所有请求挂死（端口在监听） | 终端对前台 demo.sh 按了 Ctrl+Z，整个进程组 `T (stopped)`。Ctrl+C 结束或后台运行 | `scripts/demo.sh` |
| HTTP 返回 JSON 在 64KB 被截断 | `monitor_server.c` 的 `MONITOR_HTTP_BUF_SIZE 65536` 不够装含 samples 的完整拓扑 JSON。扩到 131072 (128KB) | `monitor_server.c:46` |
| 转向灯左右颠倒（变道打右灯亮左边/打左灯亮右边） | 车辆模型所有 `*_L` 件放在 z=+0.82（THREE 右手系车头朝 +X 时 +z=几何右），`*_R` 在 z=-0.82；前端 `_setVehicleLights` 按名字点 FL/RL → 左灯请求点亮几何右灯。修复：gen_models.py 全部 L/R 件 z 互换 + 重生成 gltf + `--validate` 对称性门禁（生成物中心 z 符号必须与名字一致，防复发） | `tools/flowboard/gen_models.py`（L/R 部件 z 号） |
| 改了模型/JS 但浏览器还显示旧效果（转向灯仍反） | `monitor_server.c` 把 `/tools/flowboard/models/*` 标 `Cache-Control: immutable`（1 年），模型文件改后浏览器永远用旧缓存不重拉。修复：models/ 改 `no-cache` + 前端模型 URL 加 `?v=` 缓存破坏版本号（改了模型就 bump） | `src/core/monitor_server.c` `cache_control_for_path` / `tools/flowboard/js/models.js` 模型 URL |
| 掉头时车辆屁股横移/甩动（3D 观感，物理模型本身正确） | 后端 step_bicycle 中心参考是对的（含 half_wb·yaw_rate 切向项），但前端死推算 `_advanceState` 外推只用 `speed·(cos,sin)`，丢切向项（掉头时切向速度 ≈1.2m/s = 车速 34%），SSE 5Hz 下每包中心横向偏 ~0.23m → 渲染"中心直线漂移 + 车身旋转"解耦 = 屁股横移。修复：`updateDeadReckon`/`updateEntityDeadReckon` 喂世界系 vx/vy（step_bicycle 已含切向项），`_advanceState` 有 vx/vy 时按世界速度外推，无则回退旧公式；配套收严 UTurnPlanner Phase 4 退出容差 0.25→0.10 rad 减少返程残差、planning wheelbase 2.8→2.7 与 physics 一致、off_rails 退出日志加 fold/lat 残差字段 | `tools/flowboard/js/vis/core/DeadReckon.js` _advanceState / `app.js` sync2DTarget / `SceneDirector.js` updateEntityDeadReckon / `planning_node.cpp` Phase 4 / `flowsim_node.cpp` [OFFRAILS] exit 日志 |
| 浏览器 3D 卡成 PPT（后端 pub/frame_time/CPU 全稳定，只前端卡） | **无 GPU 环境（WSL/云 VM）浏览器用软件 WebGL**（SwiftShader/llvmpipe），默认 `medium` 性能档开着 Bloom+SMAA 后处理 + 2048 阴影 → 软件逐像素计算极慢；PHM 自动降级要 3×1s 窗口 <30fps 才降一档（6-9s 才到 low），期间已在看 PPT。另：轨迹 ribbon 每帧 5 层、其中 3 层 AdditiveBlending（外层辉光/内亮核心/流动条）按屏幕面积软件混合，最贵。修复：`isSoftwareRenderer()` 读 `WEBGL_debug_renderer_info` 匹配 SwiftShader/llvmpipe/softpipe → 检测到**直接 low 档启动**（禁后处理/阴影/DPR=1），不等自动降级；档位同步到 `store.perfTier`；TrajectoryView 低档跳过辉光+流动条两层装饰性加法混合，只留主体光带+内亮核心（加法层 3→1） | `tools/flowboard/js/vis/core/Renderer.js` isSoftwareRenderer / `main.js` 软件检测+_syncPerfTier / `view/TrajectoryView.js` lowTier 裁剪 |
| **浏览器内存飙到数 GB（渐进式卡顿真凶，2026-08-04 实测 5GB）** | LabelView._ensureLabel 用 speed 判断"文本变了才换纹理"，而车速**每帧都在变**（20.01→20.02…）→ 几乎每帧换纹理 → 每帧 `makeLabelTexture` 新建 CanvasTexture：① 旧纹理被替换后**从不 dispose**（GPU+JS 双泄漏）；② `CanvasTextureFactory._cache` 按 speed 作 key **每帧新增条目无界膨胀**（256×64 纹理 ×60fps ≈ 3.8MB/s → 数分钟到数 GB）。修复：LabelView 换纹理前 dispose 旧 map；makeLabelTexture 加 noCache 路径（label 车速高变 key 缓存命中率≈0，缓存只积累死条目）+ `_CACHE_MAX=300` 有界保护 | `tools/flowboard/js/vis/view/LabelView.js` _ensureLabel / `utils/CanvasTextureFactory.js` makeLabelTexture |

## 5. 评估器 / 门禁

`ci/evaluators/**` / `tools/pipeline_check.py` / `scenario_regression` —— 门禁自身的度量伪影也算。

| 现象 | 根因 | 位置 |
|------|------|------|
| 算法评估挂死（demo_evaluator/scenario_regression 无输出不结束） | 三层因果：① demo.sh 行为监控管道 `{ tail -F \| grep } &` 的 EXIT trap 只 kill 子 shell，tail/grep 孙子泄漏（tail -F 永不退出）；② f412132 日志改到 `$LOG_DIR/launcher.log` 后泄漏 tail 命令行不再匹配启动清扫 `pkill -9 -f flow_launcher`（旧路径含该串会被顺带清理 → 旧版自愈）；③ 泄漏 grep 继承 fd2=评估器捕获管道写端 → `proc.stdout.read()` 等 EOF 永不返回。修复：tail 加 `--pid=$$`（GNU，BSD 降级）+ 启动清扫补 pkill、评估器 read() 改 select 限时读取、SIGTERM/INT killpg 防孤儿、scenario_regression 加超时 | `scripts/demo.sh` tail 块 / `ci/evaluators/demo_evaluator.py` / `ci/evaluators/scenario_regression.py` |
| demo_evaluator 报 road departure 但车明明在车道里 | 评估器 `_road_network_cross_track` 用 topology `scene.road_network.edges[].nodes` 的弦长算路沿，而该 nodes 是 `scene_pub.cpp` 固定 8 点粗采样 → 长弯道上弦长偏离 Hermite 真路 ~14m → 车被误判出路沿（实测 curve_road 3000m S 弯 -7.44m）。修复：节点数按长度自适应 ~25m 一点（8..128） | `flowsim/scene_pub.cpp` road_nodes_per_edge / `ci/evaluators/demo_evaluator.py` |
| 同一份代码重跑一遍，场景判定在 PASS/FAIL 之间摇摆（门槛附近抖） | 两条判据的"参考量"与它要守的策略不是同一套尺度：① 跟车 `min_forward_gap` 用**整段中位速度**算期望间距（注释却写"判据随车速伸缩"）→ 低速逼近/排队停下的帧被高速帧的尺度误判 FAIL（实测最差帧 `gap=4.96m @ ego 1.0 m/s`，该速度下期望 6.5m 安全；三次实测 28 帧误报、逐帧判据 0 帧违规）；② `max_duration_s` 用**墙钟跨度**（含 demo.sh 启动/收尾与机器负载，同场景 59.9→63.3s 漂移），而仿真钟只有 59.3s。修复：①逐帧用该帧车速（与 behavior 的 `acc_standoff + acc_time_headway·|v|` 同式同参）；②改按 `metrics.scene.t_us` 的仿真跨度判，墙钟超而仿真没超记 WARN | `demo_evaluator.py` `min_forward_gap` 判据 / `_sim_timestamps` + `max_duration_s` 判据 |
| 场景声明了 `traffic_density.cars_per_km` 但世界里一辆 NPC 都没有，**全部门禁仍绿** | 自动补给结论（D3-2）只存在于一条 `if (spawned > 0) LOG_INFO`：spawn 数为 0（route 没建好 / pool 满 / spacing 算错）时**完全静默**，`scene/frame` 里连 `traffic_density` 字段都没有 —— 排查时分不清"场景没声明"与"声明了没跑"，门禁也无从断言。修复：结论编码进 `scene/frame` 静态段（5 个标量，**不随 `embed_static` 省略** —— 那种"静态段太大就省"的裁剪针对 road_network/buildings 这类可能撑爆 64KB 总线的大块，大地图 OSM 场景若连带省略，评估器会把"静态段太大"误判成"spawn 没跑"）→ monitor 透传 → `metrics.scene.traffic_density`；`demo_evaluator` 两级断言：① 声明了就必须有块可读（无块 = 透传断了或 spawn 路径压根没跑，走 `require` 记为**无法判定**而非通过）② `spawned >= 1` **且** 世界车辆实体峰值 > `actors[]` 能解释的数量（防止 spawn 数字自说自话，不经 scene/frame 真值交叉校验）；flowsim 侧日志改为无条件（0 辆时打 WARN，并把"够了所以停 max_npcs"与"装不下所以停 pool 满"分开记） | `flowsim/scene_pub.h` `ScenePubTrafficDensity` / `flowsim/scene_pub.cpp:538` / `monitor_node.c:297,709,1526` / `ci/evaluators/demo_evaluator.py:2732`（`traffic_density_spawn`） |

| 故障注入后目标 topic 从 `metrics.topics` **整个消失**（不是显示 0 Hz），门禁误判"注入没生效" | 注入点在 `message_bus_publish` **之前**（`transport_publish` 入口），消息从未进过 bus → 该 topic 的 `pub_count` 恒 0 → stats 表里根本没有这个条目。A/B 实测：干净跑 43 个 topic（含 `perception/obstacles`），永久注入后 41 个（不含）。修复：门禁的"topic 缺席"判定改为**有丢弃计数时缺席即为注入生效的强证据**（`inject_dropped > 0`），而非一律判"无法确认" | `ci/gates/fault_inject_gate.py` `check_class` / `src/core/transport.c` `transport_publish` 注入点 |

| 场景声明"切断某传感器"，而评估器的 topic 频率门禁要求该 topic 达最低频率 → **该场景永远不可能 PASS**（门禁与它要验证的行为互相矛盾） | 门禁把"topic 必须在发"当无条件要求。但对**故意注入失效**的场景，"该 topic 停发"正是它的预期行为。修复：① 场景 `fault_injection` 里直接切断的 topic 跳过频率门禁，但仍断言它**确实停了**（还在发 → WARN 提示注入可能没生效）；② **级联豁免** —— 切断某模态后其**直接消费者**掉频是正确后果（切 lidar → fusion 只能靠 GPS 慢速更新 → `fusion/localization` 20Hz→10Hz）。级联映射**只覆盖直接消费者、不做传递闭包**，避免一次注入把整条链的判据全豁免 | `ci/evaluators/demo_evaluator.py` `injected_topics` / `INJECT_CASCADE` + `load_scenario_criteria_from_pipeline` 透传 `_inject_topics` |

## 6. 框架 / 工具链

消息总线 / 调度 / `node_pump` / 启动脚本。

| 现象 | 根因 | 位置 |
|------|------|------|
| 场景 JSON 里写了 `odd` / `driver` 注入块，但 ODD 判据从未触发（`l3_odd_exit_*` 场景必 FAIL，`odd.violations` 里只有 `route_stale` 没有 `visibility`） | **`demo.sh` 的 `scenario_file` sed 漏 patch 新节点**：`odd_monitor` / `tor_manager` 的 `params` 被写成 compact 形式 `"scenario_file":"..."`（冒号后**无空格**），而 sed 模式 `\"scenario_file\": \"` 强制要求一个空格 → 这两节点永远拿不到 `--scenario` 指定的场景，注入块静默失效。同一类漏洞也在 `demo_evaluator.pipeline_scenario_override` 的白名单里（只含 flowsim/planning/navigation/control）。修复：sed 改 `\\\": *\\\"`（零或多个空白）+ 白名单补两节点。**通用教训**：给 pipeline 节点加 `scenario_file` 参数时，params 的序列化风格（有无空格）必须与 `demo.sh`/评估器的 patcher 兼容，否则新节点会静默读旧场景 | `scripts/demo.sh` scenario patch sed / `ci/evaluators/demo_evaluator.py` `pipeline_scenario_override` |
| 8 个节点线程各占满一个核 | 裸 `while(!stop) ex.run();` 忙等；`idle_sleep_us` 只被零调用者的 `run_blocking()` 读取。改用 `node_pump()` | `coroutine_task.h` node_pump |
| 管道检查 topics 列表缺 perception/obstacles | `monitor_node.c` 的 `TopicStats tstats[16]` 只能装 16 个 topic，第 17 个静默丢弃。扩到 64 | `monitor_node.c:1304` |
| 新增 IDL 类型后 `msg_layout_check` 报 `wire=N struct=N+4`，构建通过但序列化/直读两套约定不一致 | `msg_codegen` 的 wire size 是字段尺寸朴素累加（无 padding），C 结构体按自然对齐插 padding。把 `uint64` 放结构体中间就会失配（实测 RadarFrame 原顺序 wire=1044/struct=1048）。**两种修法**：① 加进 `KNOWN_MISMATCHES` 记录既有失配；② **更好的修法**是从设计上消除它 —— 调整字段顺序（大对齐成员前置）+ 显式 `reserved` 填平尾部，使 wire == struct。新增类型优先 ②，别让已知失配表继续变长 | `msg/adas_msgs.msg` 新类型字段布局 / `ci/gates/msg_layout_check.py` |
