# 卷六 · 第六课：可视化——把世界画出来

## 本章问题

你有了车（卷一）、有了总线（卷二）、有了算法（卷三）、有了世界（卷四）、有了地图
（卷五）。但所有东西都在内存和 JSON 里——**你看不见它们。**

想象你在调试：车「应该」在路中间，但你知道它可能偏了。数据上怎么查？`/tmp/flow_topology.json`
里 `ego.x`、`ego.y` 是两个数——你能盯着这两个数看出「车偏了」吗？不能。你需要**眼睛**。

这一章就是：**把内存里的世界，画成屏幕上的世界。**

## 你自己的答案：把坐标映射到像素

最朴素的想法：世界坐标 `(x, y)` 直接当屏幕坐标画。

```js
// 车在世界 (100, 50)，把它画到 canvas
ctx.fillRect(100, 50, 20, 10);
```

看，一个方块出来了。但运行五分钟你会撞墙：

1. **方向反了**：屏幕的 y 轴朝下，世界的 y 轴朝上（北）。车往北开，在屏幕上却往下走。
   --坐标系符号问题。
2. **没有朝向**：一个方块怎么表示车头朝哪？你得画一个「带箭头的方块」，需要旋转。
3. **没有高度**：高架上的车和桥下的车，屏幕上都在同一层——立体感全无。
4. **视角**：你要「跟车视角」「俯视」「上帝视角」——每次切视角都要重新算一遍坐标。

**这四个问题，本质上是同一个：你需要在多个坐标系之间来回翻译。** 而这正是卷一的
transforms 派上用场的地方——可视化是坐标变换的最大消费方。

## 真正的方案 1：坐标约定 = 唯一事实源

先解决「方向反了」这个最基础的。世界用 ENU（东-北-上），Three.js 用右手系（y 朝上）。
两者差一个约定。**项目把坐标转换收敛到唯一一组纯函数**（`js/vis/math/Coord.js`）：

```
worldToThree(x, y, z)        ENU -> THREE 坐标（[x, z, -y]）
headingToRotationY(h)        heading -> THREE rotation.y
directionToRotationY(dx,dz)  2D 方向 -> rotation.y
forwardENU(heading)           heading -> ENU 单位前向向量
offsetAlongNormal(px,pz,nx,nz,d)  沿法线偏移
tangentToNormal(tx,tz)        切线 -> 单位法线
placeOnRoad(spine,s,lateral)  沿路参数 -> {位置, 朝向, 高度}
```

**为什么必须「唯一入口」？** 因为坐标转错是最隐蔽的 bug：车明明在世界 (100, 50)，你
转错了符号画在 (100, -50)，屏幕上车在路外——但你知道是「坐标转错」还是「车真的开歪了」
吗？**肉眼分辨不出来。** 所以项目立了一条铁律：所有位置/朝向/高度转换，只准走这几个
纯函数，禁止在 view 里手写 `-y`、手写 `Math.atan2`、手写 `position.set(魔法数)`。并且
用两套机器检查兜底：

- **grep 门禁**：扫 view/ 目录，出现裸 `-y` / 裸 `atan2` / 裸魔法数 `position.set` 就
  FAIL；
- **property-test**：验证 `worldToThree` 的映射表、`placeOnRoad` 永不浮空/埋地。

**这个教训要记住：有些 bug 不是「看不见」，而是「看见了也分不清真假」——对付它的办法
不是更小心，是把转换收敛到一个地方，然后测试它。**

## 真正的方案 2：数据流——从 5Hz 的 JSON 到 60fps 的画面

后端 flowmond 以 5-10Hz 推数据（SSE），但你的屏幕要 60fps 刷新。中间差了一个数量级，
怎么填？答案是**死推算（Dead Reckoning）**：

```
SSE tick (5-10Hz): update "real" position
  -> DeadReckon records position and velocity
Render frame (60fps):
  -> extrapolate: "last real position + velocity * elapsed time"
  -> smooth animation, wait for next SSE tick
```

没有死推算，画面会一跳一跳（5Hz 的帧率）。**但死推算也有坑**（卷六故障表里有）：
外推要用**世界系速度**（vx/vy，含切向分量），不能用简单的 `speed*(cos,sin)` -- 否则
车掉头时，外推的位置沿直线走，而车身在旋转，「中心直线漂移 + 旋转」解耦，看起来就是
**车尾横甩**。这个坑和卷一那个「中心参考点切向项」是同一件事——物理真相从仿真一路
传到渲染。

## 真正的方案 3：渲染栈——从 C 后端到浏览器

完整的数据路径：

```
flowmond (C daemon, :8800)
  |-- HTTP: /api/topology (JSON)
  |-- SSE: /api/topology/stream (push)
  |
  v
flowboard (browser, localhost:8800)
  |-- app.js: EventSource subscribe
  |-- SceneDirector: validate + route to views
  |-- View tree: RoadView / VehicleView / ...
  |-- THREE.WebGLRenderer: 60fps canvas
```

**flowmond** 是 C 守护进程，从 monitor_node 的 IPC 通道或 /tmp/flow_topology.json
文件读取拓扑数据，通过 HTTP 和 SSE 推给浏览器。SSE 用单行 JSON（`data: ...\n\n`），
避免 EventSource 按行丢弃的 bug。

**SceneDirector** 是前端的「导演」：接收数据帧，校验必填字段（`validateFrame`），
计算 `roadNetworkHash`（只在路网变化时重建 view 树），分发给各 view。

### Layer 树

```
SceneDirector
  +-- Layer 'env'     (GroundView, ViaductView)
  +-- Layer 'road'    (RoadView, ConnectorView, BarrierView, StreetlightView)
  +-- Layer 'agent'   (VehicleView, LabelView, PerceptionView)
  +-- Layer 'infra'   (TrafficLightView, ETCGateView)
```

每个 Layer 是一个 THREE.Group，frustumCulled = true。大地图渲染时只画摄像机可见的
tile（500m 网格分桶），屏外 tile 自动跳过。

### View 注册表

View 通过 `ViewRegistry.register(name, view)` 注册，SceneDirector 通过
`ViewRegistry.safeCall(name, method, ...args)` 调用。关键：每个调用都包在 try/catch
里——一个 View 崩了只 log，不影响其他 View。

```
ViewRegistry.safeCall('road', 'build', rn)
  -> RoadView.build(rn)   // 可能抛错
  -> catch: log error, skip

ViewRegistry.safeCall('vehicle', 'update', ego, entities)
  -> VehicleView.update(ego, entities)
```

## 真正的方案 4：关键 View 详解

### RoadView（1121 行）

最复杂的 View，负责路面 ribbon + 车道线 + 路肩 + 路缘石 + 人行道 + 绿化带。

**路面生成**：每条 edge 独立生成 ribbon 几何体。`sampleEdgeNodes` 按弧长采样 24-32
个点（弯道更密），`buildSpine` 计算法线，`ribbonGeo` 按 halfWidth 向两侧展开。

**标线系统**：每个 lane 的 markings[] 驱动：
- `double_yellow`：两条实线（间距 0.12m），黄色材质
- `solid_white`：外缘实线，白色材质
- `dashed_white`：虚线（3m 实线 + 6m 间隔），白色材质

标线通过 `filterSpineOutsideJunctions` 在路口边界自动截断——路口内不画标线，
由 ConnectorView 的 junction patch 覆盖。

**车道对齐**：`laneGroupEnvelope()` 从 lane centerline 实测车道组相对 road centerline
的横向偏移，修正路面 ribbon 的中心位置。没有这个修正，OSM 单向路的路面会偏 3-7m。

**性能**：所有同类几何体合并成单个 BufferGeometry（`mergeGeometries`），按 500m
网格分桶（`addMergedByTile`），每桶一个 mesh。frustumCulled = true，屏外 tile 跳过。

### VehicleView（~700 行）

车辆渲染：glTF 模型加载 + 程序化 fallback + 车灯 + 方向盘 + 车牌。

**模型优先级**：glTF 优先（高精度），找不到时程序化生成（box + 4 轮）。SU7 有特殊
处理：原厂方向盘门控（`type !== 'su7'` 才创建程序化方向盘）、ChePai 节点解包定位。

**车轮动画**：前轮绕 kingpin 转向（不是旋转 axle），后轮只滚动。steer 1:1 映射
（后端 steer = 前轮转角 delta），加 `|steer|<0.005` 死区防微动。

**贴地**：`roadHeightAt(store, ex, ey)` 投影到最近中心线取高度，不用数据 z（防止
匝道高度跳变时车悬空）。

### ConnectorView（~700 行）

路口连接件：路面补齐 + 转向导流 + 桥墩 + 防撞桶。

**junction patch**：在每个检测到的路口中心铺一块 Chaikin 圆角化的多边形沥青路面
+ 四个方向的斑马线 + 停止线。junction 中心来自 JunctionDetect 的端点聚类 +
connector 质心修正。

**转向导流线**：从 fork 记录解析 incoming -> connector -> target 的转向路径，
用二次贝塞尔画左转引导虚线。通过 connector lane successors 反查目标真实道路。

**桥墩**：高架 edge（`bridge=true`）每隔 20m 落一根圆柱桥墩，从地面到路面。

**防撞桶**：只在真断头（孤端）端点放置红白圆柱。通过 byId 判定：不属于 >=3 臂
路口且无邻接端点才是真断头。

### TrafficLightView

红绿灯渲染：灯柱 + 三色灯头（红/黄/绿）+ emissive 切换。状态来自
`scene/entities` 中 `type='tl'` 的实体，`light_state` 字段驱动颜色。

## 地图预览：独立于仪表盘的 3D 预览

`tools/flowboard/map_preview.html` 是独立的 3D 地图预览页，不依赖 C 后端。
它直接 fetch `/api/map/preview`（POST map id），拿到 map.json + routes.json，
在浏览器端构建 roadNetwork 并渲染。

用途：
- 地图开发时快速看效果（不用启动完整 demo）
- 检查 junction patch 对齐、标线连续性、建筑位置

```bash
# 启动预览服务器
python3 tools/preview_map.py --map osm_lujiazui_v2
# 浏览器打开 http://localhost:8011/tools/flowboard/map_preview.html?map=osm_lujiazui_v2
```

## 仪表盘 API

flowmond 提供的 HTTP API：

| 端点 | 方法 | 返回 | 用途 |
|------|------|------|------|
| `/api/topology` | GET | JSON | 当前拓扑快照（ego/entities/road/traffic） |
| `/api/topology/stream` | GET | SSE | 实时推送（5-10Hz） |
| `/api/map/preview` | POST | JSON | 地图预览数据（map + routes） |
| `/` | GET | HTML | FlowBoard 仪表盘主页 |

拓扑 JSON 结构：
```json
{
  "t_us": 1234567890,
  "ego": { "x": 100, "y": -1.75, "heading": 0.1, "speed": 15.0, ... },
  "entities": [...],
  "road_network": { "edges": [...], "junctions": [...] },
  "traffic_lights": [...],
  "trajectory_path": [[x,y,z], ...]
}
```

## 性能优化

### 软件渲染检测

WSL/云 VM 没有 GPU，浏览器用 SwiftShader/llvmpipe 软件渲染。项目通过
`WEBGL_debug_renderer_info` 检测，检测到直接 low 档启动（禁后处理/阴影/DPR=1），
不等自动降级的 6-9s 延迟。

### 纹理缓存有界

LabelView 曾按 speed 文本每帧换纹理 -> 每帧新建 CanvasTexture 且不释放 -> 内存
几 GB。修复：缓存有界（`_CACHE_MAX=300`）+ 换纹理前 dispose 旧 map。

### Tile 合批

RoadView 按 500m 网格分桶，每桶合并成单个 BufferGeometry。frustumCulled = true，
屏外 tile 自动跳过。大地图（郑东 42k 路段）只渲染相机附近的 tile。

## 3D 侧门禁

`npm run vis:check:all` 是合并门槛，四件事：

```bash
npm run vis:check          # 1. 模块加载 + ESLint + tick 冒烟
npm run vis:check:invariant # 2. 坐标 property-test
npm run vis:check:grep     # 3. grep 门禁（裸 -y / atan2 / 魔法数）
npm run vis:check:junction # 4. 路口标线几何回归
```

260+ 测试，0 fail 才可合并。

**测试环境教训**：有的测试断言网格几何（`position.count`、`material.color`），
在 three shim（Proxy 桩）环境下永远数不到真实值——Proxy 的 count 返回的不是数字。
这类测试必须走**真实 three.js**（REAL_THREE 白名单），否则假红假绿。

## 动手实践

```bash
# 1. 启动 demo，看 3D 仪表盘
bash scripts/demo.sh

# 2. 跑 3D 门禁
npm run vis:check:all

# 3. 启动地图预览，单独看一张地图
python3 tools/preview_map.py --map osm_lujiazui_v2

# 4. 在浏览器控制台看 scene 对象
#    打开 DevTools -> Console -> 输入 scene.children 遍历 layer 树
```

## 常见陷阱

1. **坐标转换必须走 Coord.js**：手写 `-y` / `atan2` 会被 grep 门禁拦下。
2. **死推算用世界系速度**：`speed*(cos,sin)` 掉头时车尾横甩。
3. **软件渲染直接降档**：不等自动检测的 6s 延迟。
4. **纹理缓存有界**：无界缓存 = 内存泄漏。
5. **测试用真实 three.js**：shim 环境下几何断言是假的。

## 小结

这一章你学会了把世界画出来：

- **坐标变换是可视化的核心**：方向、朝向、高度全靠 transforms，收敛到唯一入口 +
  grep/property-test 双门禁，因为「坐标转错的 bug 肉眼分不清真假」。
- **5Hz 数据到 60fps 画面靠死推算**：外推用世界系速度，否则掉头会车尾横甩。
- **渲染栈**：flowmond (C) -> SSE -> SceneDirector -> View tree -> THREE.js。
- **对象树 + 错误隔离**：一个 View 崩了不黑屏。
- **性能要检测 + 有界**：软件渲染直接降档、纹理缓存设上限、tile 合批 + frustumCulled。
- **3D 侧门禁与 C 侧对称**：vis:check:all 260+ 测试，合并门槛。

## 练习（选做）

1. **读代码**：`tools/flowboard/js/vis/math/Coord.js`，找到 `worldToThree`，验证它做的
   就是卷一的「旋转 + 平移」公式（只是多了 z）。
2. **思考题**：为什么「坐标转换的唯一入口」比「每个 view 自己转换」更安全？提示：想象
   20 个 view 各自手写 `-y` 翻转，其中 1 个写错了符号——你怎么找到它？
3. **挑战**：5Hz 的 SSE 数据，你只有「位置 + 速度」，怎么外推出「朝向」？提示：方向
   速度的反正切。（再想想：如果车静止，这个外推还成立吗？）
4. **进阶**：读 `SceneDirector.js` 的 `update` 函数，理解 `roadNetworkHash` 是怎么
   决定「是否重建 view 树」的。如果 hash 算错了（每帧都变），会发生什么？

---

**下一卷预告**：车会开了、世界有了、看得见了。现在让它**学会自己开**——卷七讲数据
闭环与车端学习：怎么让车从「被代码开着」变成「自己学会开」。
