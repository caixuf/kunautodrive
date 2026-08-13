# FlowBoard 3D 渲染 — 现状与经验沉淀

> 沉淀 `tools/flowboard/js/vis/` 的 3D 渲染高价值经验：车辆动画（车轮/贴地/SU7）、
> camera 跟随、性能降档、map preview 隔离。改这些代码前先读本文，避免重踩
> 浮点精度、双低通抖动、降档误触发、车牌定位等坑。

## 1. 模块地图

| 文件 | 职责 |
|------|------|
| `js/vis/view/VehicleView.js` | 车辆 glTF 渲染 + 动画（车轮/贴地/车牌/方向盘） |
| `js/vis/core/CameraRig.js` | chase/top/driver/front/map/orbit 相机跟随 |
| `js/vis/core/PerfMonitor.js` | 独立 watchdog 性能自监控 / 降档 |
| `js/vis/math/RoadHeight.js` | 贴地高度 `roadHeightAt` |
| `js/mapPreview.js` + `map_preview.html` | 隔离 3D 地图预览 |

## 2. 车辆动画（VehicleView.js）

### 车轮转向/滚动（L608-691）
- **前轮绕主销（kingpin）转向而非旋转 axle**（L628-643）：整体旋转 `axle_front` 会让左右前轮一起绕轴心平移，掉头满舵(0.58 rad)下各轮 ~0.47m 脱离轮拱变"车轮漂移"；真实前轮绕各自 kingpin 转向、轮心不动，故只设前轮 `rotation.y`。fallback 走 `w.userData.kingpin.rotation.y`（kingpin 父组在轮心）。
- **steer 1:1 映射**：后端 step_bicycle 的 steer 就是前轮转角 δ，直接显示；旧乘子 0.6 把前轮角缩小 40%，变道 2-3.5° 肉眼不可见 →"前轮不会动"。加 `|steer|<0.005` 死区防巡航微动。
- **滚动锁定只作用于前轴**：转向与滚动是两个独立自由度，打方向时前轮照常滚动，不再锁死停转（旧实现为避免欧拉组合漂移在转向时暂停前轮滚动 → 观感"打方向轮子就不动了"）。
- **modulo 2π 保持角度**（L682-687）：`rotation += angularSpeed*dt` 无限累加会在欧拉角/矩阵转换时浮点精度退化（动画忽快忽慢），`% TAU` 控制在 `[0,2π)`。半径 glTF 固定 0.35（L676），fallback 读 `wheelRadius || 0.33`。

### 车辆贴地（L745-759）
- 用**道路高度而非数据 z**：`ty = roadHeightAt(store,ex,ey) + VEHICLE_GROUND_Y`，保证匝道/高架高度变化时车辆始终贴路面，不陷入不悬空。
- `VEHICLE_GROUND_Y = 0.10`（L36）与 `RoadView.Y_ROAD=0.10`（L43）对齐，防车道线/边线 z-fight。
- `roadHeightAt`（RoadHeight.js:74-98）：投影到最近中心线，直道线性插值 / 弯道 Catmull-Rom 采样高度 z。

### SU7 特殊处理
- **原厂方向盘门控**（L600-603）：`if (type !== 'su7') scene.add(_createSteeringWheel())`。SU7 是授权高精度模型，驾驶位自带原厂方向盘（`interior.009` 左舵），不能叠程序化丑方向盘；sedan/suv/truck 由 gen_models.py 生成只有四轮无内饰，需程序化补 `_createSteeringWheel()`（L414-473）。
- **车牌渲染 `_applyNewEnergyPlate`**（L364-384，详见 §SU7 车牌）与 `_cleanSu7Exterior`（L273-292，移除 ChePai/logo 贴纸 + car_body 的 AO 贴图）。
- **glTF 复用不重复 clone**（L586-605 注释）：`getModel()` 返回即 THREE.Group 直接使用不 clone，只在首次切到 glTF 时 clone，避免每帧 clone 169k 三角 SU7 层级。

## 3. SU7 车牌渲染（GA 36-2018 + 模型实测定位）

### 纹理 `_makeNewEnergyPlateTexture`（L300-354）
- Canvas 1px=1mm，按 **GA 36-2018**：外廓 440×140mm、8 字符、字高 90mm、前两字宽 45/后六字 43mm、字距 9mm、第 2/3 字间 8mm 间隔圆点、黑框 2mm、上亮下深渐变绿底、黑字；逐字符测量字宽横向压缩避免挤出板面（L338-348）。Linux 需 Noto Sans CJK SC 字体（L337）。

### 定位来自模型实测 ChePai 节点（L356-363）
- **前牌**：垂直平面贴前保险杠凹槽（x≈2.627、y≈0.546、z≈0，0.428×0.133），`front.rotation.y = π/2` 正面朝 +X。
- **后牌**：随尾门倾斜（底边靠后 x≈-2.584、顶边靠前 x≈-2.536，中心(-2.560,0.502,0)，0.428×0.135）。
- 尺寸取 **ChePai 实测值而非国标 0.48×0.14**，恰好落进原厂预留框。
- **Matrix4.makeBasis 旋转**（L376-382）：用 `right=(0,0,1)`、`up=(0.048,0.128,0).normalize()`、`normal=cross(right,up)` 建基底矩阵把平面旋到贴合尾门斜面（法线朝后略向上），`addScaledVector(normal, 0.003)` 前出 3mm 收进框内防 z-fight。

### 车牌坑
1. **不能硬编码**：旧 `x=±2.20, y=0.52` 导致嵌进保险杠/太高。
2. **必须解包 meshopt 压缩后读模型自带 ChePai 节点实测顶点**：ChePai 节点曾因 32 顶点合并成单个 bounding box 被误读为"车底 5.2m 长饰条"。
3. **后牌在尾门斜面**而非垂直面，需 makeBasis 旋转贴合。
4. **画布宽度不足会显示不全**：必须逐字符压缩字宽，避免字符挤出板面。

## 4. camera 跟随（CameraRig.js）

- **chase 刚性锁定**（L48-59 复盘注释 + L111-118）：位置刚性锁定 ego 显示位姿，不做二次平滑。旧实现给"已被 DeadReckon λ=8 平滑过的 ego"再叠一层 λ=12 指数平滑 → 双低通相位差表现为"车相对画面前后蹿"。heading 保留轻量平滑 λ=12（L81-87 + 最短角插值）。
- **orbit/free 跟随 ego**（ce7e8ad，L62-64 + L150-172）：`_orbitPrevEgo` 记录上一帧 ego 位置，按 ego 位移整体平移 `camera.position` + `orbitControls.target`，让用户自由环绕/缩放时仍贴着移动中的 ego。位移阈值 `>1e-6` 防抖动（L160）。进入 orbit 时 `needsControlSnap` 先 snap target 到当前 ego。

## 5. 性能降档（PerfMonitor.js）

- **独立 watchdog**（L4-8）：用独立 `setInterval`(1s) 而非 rAF 循环做降级——GPU 被后处理/阴影卡死时 rAF 被节流仍能采样真实帧率。
- **6000ms 开局暖机**（L25-31, L111）：glTF 异步加载 / PMREM 环境烘焙 / 材质编译在开局造成低帧，是"加载开销"非 GPU 卡死，暖机期不计低帧。
- **document.hidden 重置**（L115-118）：切标签页/应用时 rAF 暂停 → frames=0 → fps≈0，是"主动省 GPU"非卡死；`document.hidden` 时 `_consecutive=0` 直接 return。
- **setActive(false) 屏蔽**（L62-68）：3D 场景不可见（切 analyze/operate 工作区主动降帧）时置 false，比 document.hidden 更细一层。
- 降档判定（L120-129）：连续 `downgradeWindows=3` 个 1s 窗口 FPS<30 才降档并重置。**教训**：旧逻辑连续 3 秒 FPS<30 永久降档不恢复，被开局加载卡顿 + tab 隐藏误触发。

## 6. map preview（隔离 3D 预览）

- 入口 `app.js:119-132`：读 map/route 下拉 → `setRenderPaused(true)` → iframe 载入 `/tools/flowboard/map_preview.html?map=...&route=...&v=isolated2`；关闭 `closeMapPreview`（:134-140）恢复主视图。**隔离设计**：预览抛错不影响主渲染。
- `mapPreview.js`：`buildRoutePath`（L31-42）按 route `road_chain` 沿 road 选车道 centerline 拼 spine，`lane_direction` 过滤、去重；`routeFocus`（L44-70）包围盒中心 + 首两点 heading + 自适应高度 `max(80,min(380,span*0.72))`；`toTopo`（L72-110）velocity 归零防 DeadReckon 漂移。

## 7. 经验坑速查

1. **Euler 浮点精度**：滚动角无限累加 → 动画忽快忽慢，必须 `% TAU`。
2. **转向 vs 滚动独立自由度**：作用同一 wheel mesh 会欧拉组合大转角漂移；旧实现"转向时锁前轮滚动"导致"打方向轮子不动"。
3. **steer 1:1**：后端 steer 即前轮转角 δ，别乘子缩小；加死区。
4. **贴地**：`roadHeightAt + 0.10`，与 Y_ROAD 对齐，用道路高度非数据 z。
5. **车牌**：解包 meshopt 实测 ChePai、后牌 makeBasis 贴尾门斜面、尺寸取实测、字宽逐字压缩。
6. **SU7 方向盘**：`type !== 'su7'` 门控。
7. **camera chase 刚性锁定**：别二次平滑；orbit 用 `_orbitPrevEgo` 位移跟随。
8. **降档三重防误触发**：6000ms 暖机 + document.hidden 重置 + setActive(false)；独立 setInterval watchdog。
9. **glTF 不重复 clone**：getModel() 返回即 Group。

## 8. 文档覆盖现状

- `docs/VISUALIZATION_ARCHITECTURE.md`：总体架构/数据流/SceneDirector/Layer 树/DeadReckon，VehicleView 的 SU7 模型/车漆/灯光有简略说明（L590-607）。**未覆盖**：车轮 kingpin 转向/滚动/modulo、车牌 GA36-2018、贴地 roadHeightAt、camera 跟随细节、PerfMonitor 降档、map preview。
- `docs/VIS_MODULE_GUIDE.md`："怎么写模块"规范，不含具体车辆/相机/性能经验；module 清单表格已过时（MapOverlayView 已更名 MinimapHUD）。
- `docs/TROUBLESHOOTING_3D_DASHBOARD.md`：3D 加载失败根因（SSE 多行 JSON 被 EventSource 丢弃），与渲染层无关。
- `docs/FLOWBOARD_CONTRACT.md`：纯数据层契约。
