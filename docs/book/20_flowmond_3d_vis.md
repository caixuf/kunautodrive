# 第 20 章：在浏览器里看清这辆车

车在跑，可它到底跑得对不对？光盯着一堆日志很难判断：感知的点云是不是对齐了，规划出来的轨迹是不是太抖，控制有没有超调。这套系统配了一个轻量的 Web 可观测性套件 FlowBoard，底层用 C++ 写的监控守护进程 `flowmond` 采集拓扑和全总线遥测，中间经 `dashboard_bridge` 共享内存通道把结构化数据送出去，前端用 Three.js 渲染 3D 数字孪生，并靠前端航位推算（Dead Reckoning）插值把画面补顺。

## 数据从算法进程流到浏览器

```
  ┌─────────────────────────────────────────────────────────────┐
  │                 ADAS Pipeline 核心算法进程                  │
  │     [perception]    [fusion]    [planning]    [control]     │
  └──────────────┬──────────────────┬──────────────────┬────────┘
                 │                  │                  │
                 ▼                  ▼                  ▼
  ┌─────────────────────────────────────────────────────────────┐
  │        dashboard_bridge / stats_bridge (POSIX 共享内存)     │
  │     - 零拷贝聚合 Topic 统计 (FPS / p99 / 丢包数)            │
  │     - 大 JSON 拓扑分块重组传输协议                          │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │             flowmond 监控守护进程 (C++ 后台服务)            │
  │     内置 WebSocket / HTTP Server (Port 8800)                │
  └──────────────────────────────┬──────────────────────────────┘
                                 │ JSON 数据流 (WebSocket 50Hz)
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │              FlowBoard Web 3D 可视化前端 (Three.js)         │
  │  ├── 3D View 模块: RoadView, EgoVehicleView, ObstacleView   │
  │  ├── 状态机拓扑渲染: FSM State Visualizer                   │
  │  └── 前端航位推算 (Dead Reckoning 60FPS 平滑插值)           │
  └─────────────────────────────────────────────────────────────┘
```

## 前端 3D 场景是一组可插拔的 View

FlowBoard 的前端 3D 场景拆成了高内聚、松耦合的 View 模块（`tools/flowboard/js/views/`），每个 View 只管一件事：

| 视图模块 | 职责与渲染要素 | 关键技术 |
| :--- | :--- | :--- |
| **RoadView** | 道路路面、车道标线（白实线/虚线/黄线）、停止线 | 基于参考线法向扩展的三角面网格（Ribbon Mesh） |
| **EgoVehicleView** | 自车 3D 实体模型、车轮转动、刹车灯/转向灯状态 | GLTF 模型加载与动态材质贴图控制 |
| **ObstacleView** | 3D 边界框（Bounding Box）、速度矢量箭头、分类标签 | InstancedMesh 批量实例化渲染（百万级点云性能） |
| **TrajectoryView** | 规划期望轨迹线、历史行驶轨迹面包屑 | 动态 Catmull-Rom 曲线着色器 |
| **SafetyCorridorView** | 安全走廊包络、TTC 危险碰撞红色预警面 | 半透明动态多边形混合渲染 |

## 位姿更新太慢，画面就会抖

浏览器的渲染刷新率通常是 $60\text{ Hz}$ 或 $144\text{ Hz}$，而后端经 WebSocket 推位姿只有 $20\text{ Hz} \sim 50\text{ Hz}$。要是前端收到一帧才更新一次位置，画面就会明显一卡一抽（Jitter）。

FlowBoard 在 `tools/flowboard/js/app.js` 里做了一层前端航位推算插值：

```javascript
// 基于前向运动学的微秒级平滑插值
function updateEgoSmoothPose(deltaTimeSeconds) {
    if (!lastServerState) return;

    // 1. 沿当前航向推进局部位置
    const v = lastServerState.speed; // m/s
    const yaw = lastServerState.heading; // rad
    
    // 2. 局部积分推算
    predictedPose.x += v * Math.cos(yaw) * deltaTimeSeconds;
    predictedPose.y += v * Math.sin(yaw) * deltaTimeSeconds;
    
    // 3. 收到真实服务端帧时进行低通滤波软对齐 (LERP)
    predictedPose.x = THREE.MathUtils.lerp(predictedPose.x, serverPose.x, 0.3);
    predictedPose.y = THREE.MathUtils.lerp(predictedPose.y, serverPose.y, 0.3);
    predictedPose.yaw = THREE.MathUtils.lerp(predictedPose.yaw, serverPose.yaw, 0.3);

    // 4. 更新 Three.js 摄像机与自车模型 Transform
    egoMesh.position.set(predictedPose.x, predictedPose.y, 0);
    egoMesh.rotation.z = predictedPose.yaw;
}
```

## 3D 画面之外的那块内省面板

除了 3D 场景，FlowBoard 还挂了一块实时的系统内省面板：能看到 CPU、内存和线程亲和度；能看 Topic 拓扑连线图，实时显示发布者和订阅者的连接状态；还有一张延迟分布热力图，摊出每个算法节点耗时的 $P_{50}, P_{90}, P_{99}$。

## 前端崩过两次，都是显存惹的祸

第一次是页面开着开着就卡死。跑到 30 分钟左右，内存占用冲破 4GB，浏览器直接崩溃。翻代码发现，每帧都在动态生成新的轨迹曲线 `BufferGeometry`，却从没调用旧对象的 `geometry.dispose()` 和 `material.dispose()`，显存就这么被撑爆了。真正的改法是预分配一段定长顶点缓冲区，每帧只调 `positionAttribute.setXYZ()`，再把标记 `needsUpdate = true` 就行。

第二次跟显存无关，是依赖来源的问题。工业现场和车载局域网通常连不上外网，所以前端代码不能从公网 CDN 动态拉 `three.js`、`fonts` 或外部材质图片。所有依赖都得打包进 `tools/flowboard/vendor/` 这个本地目录，否则断网环境直接白屏。
