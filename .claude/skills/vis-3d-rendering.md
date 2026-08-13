---
name: vis-3d-rendering
description: FlowBoard 3D 渲染经验：车辆动画（车轮 kingpin 转向/滚动/modulo 2π/贴地）、camera 跟随（chase 刚性锁定 + orbit 位移跟随）、PerfMonitor 降档三重防误触发、map preview 隔离。改动 tools/flowboard/js/vis/ 渲染相关代码时使用。
---

# FlowBoard 3D 渲染

沉淀自 vis 系列 commit。改动前先看 [VIS_3D_RENDERING.md](../../docs/VIS_3D_RENDERING.md)。

## 什么时候用

- 改 `js/vis/view/VehicleView.js`、`js/vis/core/CameraRig.js`、`js/vis/core/PerfMonitor.js`、`js/vis/math/RoadHeight.js`、`js/mapPreview.js`

## 车辆动画必守约定（VehicleView.js）

- **前轮绕 kingpin 转向，不旋转 axle**：整体旋转 `axle_front` 会让左右前轮一起绕轴心平移，满舵下脱离轮拱变"车轮漂移"。只设前轮 `rotation.y`（或 `kingpin.rotation.y`）。
- **steer 1:1 映射**：后端 steer 即前轮转角 δ，直接显示，别乘子缩小；加 `|steer|<0.005` 死区防巡航微动。
- **滚动只锁定前轴？不——滚动与转向是两个独立自由度**：打方向时前轮照常滚动，别"转向时锁前轮滚动"（旧实现导致"打方向轮子不动"）。
- **modulo 2π**：滚动角 `+= angularSpeed*dt` 无限累加会在欧拉角/矩阵转换浮点退化（动画忽快忽慢），必须 `% TAU`。
- **贴地**：`ty = roadHeightAt(ex,ey) + VEHICLE_GROUND_Y(0.10)`，与 `RoadView.Y_ROAD` 对齐防 z-fight；用道路高度而非数据 z。

## camera 跟随（CameraRig.js）

- **chase 位置刚性锁定 ego，不二次平滑**：旧实现给已平滑过的 ego 再叠一层指数平滑 → 双低通相位差 → "车相对画面前后蹿"。heading 保留轻量平滑 λ=12 + 最短角插值。
- **orbit/free 跟随**：用 `_orbitPrevEgo` 记录上一帧 ego，按位移整体平移 `camera.position` + `orbitControls.target`，让自由环绕仍贴 ego；位移阈值 `>1e-6` 防抖动。

## 性能降档（PerfMonitor.js）三重防误触发

1. **6000ms 开局暖机**：glTF 加载/PMREM 烘焙/材质编译在开局低帧是"加载开销"非 GPU 卡死，暖机期不计低帧。
2. **document.hidden 重置**：切标签页 rAF 暂停 → frames=0 → fps≈0，是"主动省 GPU"非卡死，`document.hidden` 时 `_consecutive=0`。
3. **setActive(false) 屏蔽**：3D 场景不可见（切工作区主动降帧）时置 false。

独立 `setInterval`(1s) watchdog（不依赖 rAF）；连续 3 窗口 FPS<30 才降档并重置。

## map preview（隔离 3D 预览）

- 独立 iframe 页面 + `setRenderPaused`，预览抛错不影响主渲染。
- 路线由 route `road_chain` + lane centerline 拼接；`toTopo` 时 velocity 归零防 DeadReckon 漂移。

## 改完必跑

```
npm run vis:check:all        # 3D 门禁
# 手动：刷新 flowboard 页面，确认车辆动画/相机/性能符合预期
```

## 关键坑速查

1. Euler 滚动角必须 `% TAU`。
2. 转向/滚动独立自由度，别锁前轮滚动。
3. steer 别乘子缩小，加死区。
4. 贴地用 `roadHeightAt + 0.10` 非数据 z。
5. camera chase 刚性锁定，别二次平滑。
6. 降档三重防误触发（暖机/hidden/setActive）。
7. glTF `getModel()` 返回即 Group，别重复 clone（SU7 169k 三角）。
