# FlowBoard 3D Scene 数据契约

> 本文件是 `metrics.scene`（由 `monitor_node.c` 拼装，供 `tools/flowboard/js/vis/` 模块树消费）的**唯一事实来源**。任何修改 scene schema 的行为都应同步更新本文件，并提升版本号。
>
> **历史说明**：旧版引用的 `tools/flowboard/js/scene3d.js` 已被 vis/ 模块树重构取代，
> 当前 3D 渲染入口是 `vis/main.js`，由 `vis/director/SceneDirector.js` 协调各 View。

## 1. 版本

- **版本**: 2.1.0
- **生效日期**: 2026-08-12
- **变更**:
  - v2.1.0 新增 `scene.source`（实体渲染数据源开关）与 `scene.perception_entities`（感知输出实体，世界坐标）。可视化与仿真解耦：`source:"perception"` 时前端实体渲染改用感知输出，`source:"ground_truth"`（默认）时沿用仿真真值 `entities`。
  - v2.0.0 **破坏性变更**：移除 `scene.traffic_lights`（ego-relative fallback）字段，红绿灯统一由 `scene.entities` 中的 `tl`（world 坐标）提供。monitor 不再订阅 `road/traffic_lights` 透传到 scene（该 topic 仍由 flowsim 发布，供 planning/inference/recognition 消费）
- **维护者**: flowsim / perception / flowboard 三端共同维护

## 2. 数据链路

```text
flowsim_node
  └── topic: scene/frame ────────┐
                                 │
planning_node                    │
  └── topic: planning/trajectory ┼──► monitor_node ──► /tmp/flow_topology.json ──► flowboard/vis/ 模块树
                                 │      (融合 + 归一化)
flowsim_node                     │
  └── topic: road/geometry ──────┘
```

- **flowsim 侧**：发布 `scene/frame`，包含 `road_network` + `entities`（红绿灯以 `tl` 实体提供，world 坐标）
- **感知/规划侧**：发布 `vehicle/state`、`planning/trajectory` 等
- **monitor_node**：将多个 topic 融合为 `metrics.scene`，写入状态文件
- **flowboard/vis/**：仅消费 `metrics.scene`，不反向影响上游
- **注**：`road/traffic_lights` topic 仍由 flowsim 发布，供 planning/inference/recognition 消费，但不再流入 `metrics.scene`（v2.0.0 移除）
- `road/traffic_lights.lights[]` 使用 `x/y` 表示世界停止线位置，`lane_offset` 表示前向参考系下的管辖半幅；`y_lane` 仅为旧消费者保留，不得再用于判断行驶方向。

## 3. 坐标系约定

所有**世界坐标**字段都使用 ENU：`x`=East、`y`=North、`z`=Up（高程），
地面场景的 `z` 通常为 0。车辆和模型的 ENU `heading=0` 指向 +X，逆时针为正。

前端只能通过 `vis/math/Coord.js` 转换：

```text
worldToThree(x, y, z) = [x, z, -y]
headingToRotationY(heading) = heading
```

因此 THREE 的 Y 轴是高度、Z 轴与 ENU North 反向；不得在 View 中手写坐标翻转或
朝向公式。

## 4. `metrics.scene` JSON Schema

```jsonc
{
  "schema_version": "1.0.0",  // string, 数据契约版本号（v1.2.1 新增）

  "source": "ground_truth",  // string, 实体渲染数据源: "ground_truth"(默认,仿真真值) | "perception"(感知输出)

  "ego": {
    "x": 102.5,          // double, 世界坐标 X
    "y": -1.75,          // double, 世界坐标 Y（ENU North）
    "heading": 0.05,     // double, 航向角（rad）
    "speed": 8.0,        // double, m/s
    "steer": 0.02        // double, 方向盘转角（rad）
  },

  "lane": {
    "width": 3.5,        // double, 单车道宽度（m）
    "count": 2,          // int,    可行驶车道数
    "center": 0.0        // double, 当前车道中心横向偏移（m），保留字段
  },

  // 旧场景道路几何（无 road_network 时使用）
  "road": {
    "curve_start_x": 0.0,
    "curve_length_m": 200.0,
    "curve_offset_m": -10.0
  },

  // 新场景道路网络（优先使用）
  "road_network": {
    "edges": [
      {
        "id": 0,                              // int, 道路段 ID
        "name": "road_0",                     // string, 道路名称
        "nodes": [[0,0,0], [50,0,0]],         // [[x,y,z], ...]，ENU 参考线
        "lanes": 2,                           // int, 双向合计的可行驶车道数
        "lane_width": 3.5,                    // double, 单车道宽（m）
        "oneway": false,                      // bool, 匝道等单行道路为 true
        "length": 100.0,                      // double, 参考线长度（m）
        "type": "road"                        // string, 道路类型
      }
    ]
  },

  // 完整实体数组（flowsim_node 发布，世界坐标）
  "entities": [
    { "type": "ego", "id": 0, "x": 102.5, "y": -1.75, "heading": 0.05, "speed": 8.0, "steer": 0.02, "throttle": 0.3, "brake": 0.0, "length": 4.6, "width": 2.0, "vx": 8.0, "vy": 0.0, "target_vx": 10.0 },
    { "type": "car",  "id": 1, "x": 120.0, "y": -1.75, "heading": 0.0, "speed": 3.0, "length": 4.6, "width": 2.0, "ai_state": "follow", "vx": 3.0, "vy": 0.0 },
    { "type": "suv",  "id": 2, "x": 130.0, "y":  1.75, "heading": 0.0, "speed": 5.0, "length": 4.8, "width": 2.0, "ai_state": "cruise", "vx": 5.0, "vy": 0.0 },
    { "type": "truck","id": 3, "x": 140.0, "y": -1.75, "heading": 0.0, "speed": 4.0, "length": 8.0, "width": 2.5, "ai_state": "follow", "vx": 4.0, "vy": 0.0 },
    { "type": "pedestrian", "id": 4, "x": 110.0, "y": 3.5, "speed": 1.0, "vx": 0.0, "vy": 1.0, "parked": false },
    { "type": "tl",   "id": 5, "x": 200.0, "y": -5.0, "heading": 0.0, "state": "red", "remain_s": 12.3 },
    { "type": "etc_gate", "id": 6, "x": 450.0, "y": 0.0, "heading": 0.0, "state": "closed", "progress": 0.0 },
    { "type": "stop_line", "id": 7, "x": 190.0, "y": -1.75, "heading": 0.0 }
  ],

  // 感知输出实体（perception/tracked_objects 经 monitor 车体系→世界逆变换 + 推导
  // heading/speed）。source=="perception" 时前端用它替代 entities 渲染障碍物。
  "perception_entities": [
    { "id": 1, "type": "car", "x": 113.5, "y": -1.3, "z": 0.0, "heading": 1.51, "speed": 0.1, "vx": 0.0, "vy": 0.1, "length": 4.6, "width": 2.0 }
  ],

  // 障碍物 fallback（vehicle/state，ego-relative，最多 16 个）
  "obstacles": [
    { "id": 0, "type": "car", "x": 10.0, "y": -1.75, "vx": 0.0, "vy": 0.0, "len": 4.6, "wid": 2.0 }
  ],

  // 规划轨迹（Frenet 坐标，v1.2.0 起每点第 4 元素可选 edge_id）
  "trajectory_path": [
    [0.0,  0.0, 8.0],   // [s, d, spd]
    [5.0,  0.1, 8.5],
    [10.0, 0.2, 9.0]
  ],
  "trajectory_edge_id": 0,  // int, ego 所在 road edge id，前端用它定位轨迹起始曲线（v1.2.1 新增）

  // LiDAR 点云（ego-relative）
  "lidar": [
    [10.0, 1.75, 0.2],  // [x, y, z]
    [15.0, -1.75, 0.1]
  ]
}
```

## 5. 字段详细语义

### 5.1 `ego`

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `x` | double | m | 世界坐标 X |
| `y` | double | m | 世界坐标 Y（ENU North） |
| `z` | double? | m | ENU 高程；未提供时按 0 处理 |
| `heading` | double | rad | 航向角，与 +X 夹角，逆时针为正 |
| `speed` | double | m/s | 纵向速度 |
| `steer` | double | rad | 方向盘转角 |

### 5.2 `road_network`

| 字段 | 类型 | 说明 |
|------|------|------|
| `edges[].id` | int | 道路段 ID |
| `edges[].name` | string | 道路名称 |
| `edges[].nodes` | `[[x,y,z],...]` | 至少两个 ENU 参考线点；第三项为高程，必填 |
| `edges[].lanes` | int | 双向合计的可行驶车道总数 |
| `edges[].lane_width` | double | 单车道宽度（m） |
| `edges[].oneway` | bool | 是否单行；匝道通常为 `true` |
| `edges[].length` | double | 参考线长度（m） |
| `edges[].type` | string | `road`、`ramp_curve`、`viaduct_highway`、`urban` 或 `cross_road` |

`scene_pub.cpp` 在 esmini 路网模式下沿每条参考线约每 25 m 采样，限制在
8–128 个点；legacy 直道输出两个端点、legacy 弯道输出八点。道路网络静态不变，
发布端可缓存其 JSON，但每个 `scene/frame` 都带上它，避免前端首帧错过。

**独立地图来源（可视化与仿真解耦，方案 B）**：主仪表盘带 `?map=<id>&route=<id>`
时，前端经 `/api/map/preview` 加载 `maps/<id>/map.json`，用 `mapToRoadNetwork`
（`tools/flowboard/js/showcase/sceneAdapter.js`，纯函数，有 `vis_maptoroad.test.mjs`
门禁）生成 `road_network`，覆盖 `metrics.scene` 里的仿真路网，并置 `scene.source="perception"`
（实体来自感知输出）。此时路网与实体都脱离仿真，为实车预览铺路。

**消费约定**：

- `nodes` 是道路参考线，前端用 `sampleEdgeNodes()` 转为 THREE 坐标并平滑插值
- 道路总宽度 = `lanes * lane_width`
- 道路关于参考线对称：左边缘 = +halfWidth，右边缘 = -halfWidth
- 中心双黄线位于参考线两侧 ±0.15m
- 车道分隔虚线位于 ±(i * lane_width)，i = 1, 2, ...
- 道路边缘白实线位于 ±(halfWidth - 0.06m)

### 5.3 `entities`

#### 5.3.1 车辆类（car / suv / truck / ego）

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `type` | string | - | `"car"`, `"suv"`, `"truck"`, `"ego"` |
| `id` | int | - | 实体唯一 ID |
| `x` | double | m | 世界坐标 X |
| `y` | double | m | 世界坐标 Y（ENU North） |
| `z` | double? | m | ENU 高程；未提供时为 0 |
| `heading` | double | rad | 航向角 |
| `speed` | double | m/s | 速度 |
| `length` | double | m | 车长 |
| `width` | double | m | 车宽 |
| `vx` | double | m/s | X 方向速度分量 |
| `vy` | double | m/s | Y 方向速度分量 |
| `ai_state` | string | - | AI 状态：`cruise`, `follow`, `stop_for_tl`, `lane_change`, `cutin`, `stopped`, `yield` |

**渲染约定**：

- 车辆模型前向为 +X
- 前端旋转必须使用 `headingToRotationY(heading)`
- 行人没有 `heading` 时，使用 `headingBetweenPoints()` 从速度方向推导

#### 5.3.2 行人（pedestrian）

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `"pedestrian"` |
| `id` | int | 实体 ID |
| `x` | double | 世界坐标 X |
| `y` | double | 世界坐标 Y（ENU North） |
| `speed` | double | 速度 |
| `vx` | double | X 方向速度 |
| `vy` | double | Y 方向速度 |
| `parked` | bool | 是否静止 |

#### 5.3.3 红绿灯（tl）

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `"tl"` |
| `id` | int | 实体 ID |
| `x` | double | 世界坐标 X |
| `y` | double | 世界坐标 Y（ENU North） |
| `heading` | double | 航向角（rad），灯臂垂直于该方向 |
| `state` | string | `"green"`, `"flashing_green"`, `"yellow"`, `"red"` |
| `remain_s` | double | 剩余时间（s） |

**注意**：v2.0.0 起，红绿灯统一由 `scene.entities` 中的 `tl`（world 坐标）提供，`scene.traffic_lights`（ego-relative）字段已移除。

#### 5.3.4 ETC 门架（etc_gate）

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `"etc_gate"` |
| `id` | int | 实体 ID |
| `x` | double | 世界坐标 X |
| `y` | double | 世界坐标 Y（ENU North） |
| `heading` | double | 航向角（rad），门架 crossbar 垂直于该方向 |
| `state` | string | `"closed"`, `"opening"`, `"open"` |
| `progress` | double | 抬杆进度 [0, 1] |

#### 5.3.5 停止线（stop_line）

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `"stop_line"` |
| `id` | int | 实体 ID |
| `x` | double | 世界坐标 X |
| `y` | double | 世界坐标 Y（ENU North） |

### 5.3.6 `perception_entities`（v2.1.0 新增，感知输出）

由 `perception/tracked_objects`（object_tracker 的 KF 语义障碍物，车体系）经 monitor
用同一份 `vehicle/state` ego pose 逆变换回**世界 ENU 坐标**，并从世界速度矢量推导
`heading`/`speed`。`scene.source=="perception"` 时前端用它替代 `entities` 渲染障碍物，
实现可视化与仿真解耦。

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `id` | int | - | 跟踪目标 ID（跨帧唯一） |
| `type` | string | - | `"car"`（VEHICLE/CYCLIST/UNKNOWN 统一映射）或 `"pedestrian"`（PEDESTRIAN） |
| `x` / `y` | double | m | 世界 ENU 坐标（由车体系逆变换所得） |
| `z` | double | m | 高程，恒 0（地面场景） |
| `heading` | double | rad | 由世界速度矢量推导 `atan2(vy, vx)` |
| `speed` | double | m/s | 世界速度标量 `√(vx²+vy²)` |
| `vx` / `vy` | double | m/s | 世界系速度分量 |
| `length` / `width` | double | m | 包围盒尺寸（来自 KF track） |

**说明**：感知层无语义 `ai_state`（跟车/变道等行为标签是仿真特有的），前端在
`source=="perception"` 时对应标签缺省。坐标逆变换与 object_tracker 的 world→body
恰好互逆（都用同一份 ego pose），故感知实体落位与仿真真值一致（实测同车 KF 估计
vs 真值偏差 <0.5m）。

**BEV 鸟瞰渲染**：2D 俯视视图（`scene2d.js`）在 `scene.source=="perception"` 时，
用 `perception_entities` 的世界坐标（`project()` 投影）渲染感知障碍物角标（替代仿真
真值 `scene.obstacles`），叠加路网、规划轨迹（`trajectory_path`）、LiDAR 点云、检测
射线、ego。这是实车主流的 ADAS-HMI BEV 形态：感知 bbox + 规划轨迹 + 高精地图底图。

### 5.4 `trajectory_path`

Frenet 坐标数组：`[[s, d, spd], ...]` 或 `[[s, d, spd, edge_id], ...]`（v1.2.0 起第 4 元素可选）

| 元素 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `s` | double | m | 沿参考线的纵向距离（从 ego 当前位置起算） |
| `d` | double | m | 横向偏移，d > 0 在参考线左侧，d < 0 在右侧 |
| `spd` | double | m/s | 该点目标速度 |
| `edge_id` | int? | - | v1.2.0 可选。所在 edge 在 `road_network.edges` 数组中的下标。planning 升级后填充 |

`trajectory_edge_id`（v1.2.1 新增）是 scene 级别的字段，来自 `vehicle/state.road_id`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `trajectory_edge_id` | int | ego 所在道路 edge 的 id，前端用此优化起始曲线定位（优先于 per-point edge_id） |

**渲染约定**（v1.2.0 跨 edge 链式投影）：

- 有 `road_network` 时，前端将 ego 投影到最近 edge，沿 curve 前进 s 米，再横向偏移 d
- **s 超出当前 edge 剩余长度时**，按邻接表（端点重合 < 1.5m 判定连接）跳到下一条 edge 继续投影，避免弯道末端 clamp 堆积
- 若某点含 `edge_id`，直接定位到指定 edge 投影（未来 planning 填充后更精确）
- 无 `road_network` 时，退化为沿 ego heading 直线外推

> 注：planning 当前用单段弯道参考线（`road/geometry`），尚未消费 `road_network`，因此暂不输出 `edge_id`。前端跨 edge 链式投影已能消除弯道堆积；待 planning 重构参考线为多 edge 拼接后，可填充 `edge_id` 进一步对齐。

## 6. 兼容性规则

- **向后兼容**：生产者可以增加字段（`additionalProperties: true`），消费者必须忽略未知字段
- **破坏性变更**：删除字段、修改坐标系、修改字段语义 = 必须提升主版本号（如 1.x → 2.0）
- **非破坏性变更**：新增字段 = 提升次版本号（如 1.0 → 1.1）
- **纯渲染改动**：仅修改 `vis/view/*.js` 的颜色、材质、相机、模型，不触碰 schema = 不提升版本号

## 7. 当前已知的待改进项

以下改进需要修改 schema，因此需要 flowsim / planning 配合：

1. **轨迹参考线**：~~`trajectory_path` 未附带所属 `edge_id` 或参考线，弯道处前端投影可能不准~~ v1.2.0 已修复：前端实现跨 edge 链式投影；planning 输出 `edge_id` 待后续重构参考线后启用
2. **红绿灯来源冗余**：~~`scene.traffic_lights`（ego-relative）与 `scene.entities` 中的 `tl`（world）并存，建议统一为 world 坐标并移除 `scene.traffic_lights`~~ v2.0.0 已修复：移除 `scene.traffic_lights`，红绿灯统一由 `scene.entities` 中的 `tl` 提供
3. ~~版本号字段~~（v1.2.1 已在 `metrics.scene` 顶层增加 `schema_version: "1.0.0"`）
4. **trajectory_edge_id**（v1.2.1 新增）：`metrics.scene` 中的 `trajectory_edge_id` 来自 `vehicle/state` 的 `road_id` 字段（flowsim_node 发布），前端优先用该字段定位起始 road edge，避免多段路网搜索最近曲线时的歧义

## 8. 责任边界

| 改动类型 | 需要修改的模块 | 是否需要更新本契约 |
|----------|---------------|-------------------|
| 改颜色/材质/相机/模型 | `vis/view/*.js` | 否 |
| 改字段解释方式（不改 schema） | `vis/view/*.js` | 否 |
| 新增可选字段 | flowsim / planning / monitor + `vis/view/*.js` | 是（次版本号） |
| 修改已有字段语义 | flowsim / planning / monitor + `vis/view/*.js` | 是（主版本号） |
| 删除字段 | flowsim / planning / monitor + `vis/view/*.js` | 是（主版本号） |
