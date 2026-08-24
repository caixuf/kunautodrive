# 第 20 章：系统可观测性与 3D 数字孪生前端（flowmond & Three.js）

> **本章导读**：
> 复杂的自动驾驶系统若缺乏直观的可视化工具，工程师将难以在车辆运行过程中判断感知点云是否对齐、轨迹规划是否平滑、或者控制是否产生超调。
>
> KunAutoDrive 构建了完备的轻量级 Web 可观测性套件 **FlowBoard**：底层由高性能 C++ 监控守护进程 **`flowmond`** 采集拓扑与全总线遥测，中间层通过 **`dashboard_bridge` 共享内存通道** 传输结构化数据，前端利用 **Three.js** 实现微秒级帧率的 3D 数字孪生渲染与**前端航位推算（Dead Reckoning 插值）**。

---

## 1. 可观测性分层体系架构

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
  │     内置 WebSocket / HTTP Server (Port 8080)                │
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

---

## 2. 前端 3D View 模块化规范

FlowBoard 的前端 3D 场景采用高内聚、松耦合的 **View 模块化插件体系**（`tools/flowboard/js/views/`）：

| 视图模块 | 职责与渲染要素 | 关键技术 |
| :--- | :--- | :--- |
| **RoadView** | 道路路面、车道标线（白实线/虚线/黄线）、停止线 | 基于参考线法向扩展的三角面网格（Ribbon Mesh） |
| **EgoVehicleView** | 自车 3D 实体模型、车轮转动、刹车灯/转向灯状态 | GLTF 模型加载与动态材质贴图控制 |
| **ObstacleView** | 3D 边界框（Bounding Box）、速度矢量箭头、分类标签 | InstancedMesh 批量实例化渲染（百万级点云性能） |
| **TrajectoryView** | 规划期望轨迹线、历史行驶轨迹面包屑 | 动态 Catmull-Rom 曲线着色器 |
| **SafetyCorridorView** | 安全走廊包络、TTC 危险碰撞红色预警面 | 半透明动态多边形混合渲染 |

---

## 3. 前端航位推算（Dead Reckoning 插值算法）

Web 浏览器的渲染刷新率通常为 $60\text{ Hz}$ 或 $144\text{ Hz}$，而后端 WebSocket 推送车辆位姿的频率通常为 $20\text{ Hz} \sim 50\text{ Hz}$。如果前端直接在每收到一帧数据时更新位置，画面将呈现出明显的**卡顿与抽搐（Jitter）**。

FlowBoard 前端在 `tools/flowboard/js/app.js` 中实现了高精度的 **前端航位推算插值器**：

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

---

## 4. 拓扑与性能指标实时内省

除 3D 场景外，FlowBoard 还提供实时的系统内省面板：
- **实时 CPU/内存与线程亲和度**；
- **Topic 拓扑连线图**：动态显示发布者与订阅者的连接状态；
- **微秒级延迟分布热力图**：展示每个算法节点处理耗时的 $P_{50}, P_{90}, P_{99}$。

---

## 5. 工业级避坑指南

### 避坑 1：Three.js 内存泄漏（Geometry / Material 未释放）
- **现象**：浏览器运行 30 分钟后页面卡死崩溃，内存占用突破 4GB。
- **原因**：每帧动态生成新的轨迹曲线 `BufferGeometry` 时，未调用旧对象的 `geometry.dispose()` 和 `material.dispose()`，导致显存被彻底撑爆。
- **最佳实践**：预分配固定长度的定长顶点缓冲区，每帧仅调用 `positionAttribute.setXYZ()` 并标记 `needsUpdate = true`。

### 避坑 2：外部未隔离网络资源依赖（Air-Gapped Environment）
- **安全红线**：工业与车载局域网环境通常无法连接外网。前端代码**严禁**从公网 CDN 动态引入 `three.js`、`fonts` 或外部材质图片，所有依赖项必须全部打包在 `tools/flowboard/vendor/` 本地目录中。

---

*下一章预告：第 21 章将探讨自动驾驶质量保障终局——黑盒回归评估体系（Demo Evaluator）。*
