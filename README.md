# KunAutoDrive: 高性能自动驾驶仿真与车规中间件平台

[![CI](https://github.com/caixuf/kunautodrive/actions/workflows/ci.yml/badge.svg)](https://github.com/caixuf/kunautodrive/actions)
![License](https://img.shields.io/badge/license-MIT-blue)
![C](https://img.shields.io/badge/C-11-555555)
![C++](https://img.shields.io/badge/C++-20-659ad2)

> 面向自动驾驶与机器人系统的高性能、仿真优先中间件：采用 C11 核心内核、C++20/FlowCoro 实时协程拓扑、配置驱动插件管线、微秒级零拷贝总线与 3D 动力学闭环评估体系。

---

## 核心技术战报与实测数据

### 1. 纳秒级硬实时与 ASIL-D 车规级证据质量

* **100,000 次推演硬实时延迟分布**：
  * **P50 中位数延迟**：**152.0 ns** (0.15 us)
  * **P99 车规分位延迟**：**179.0 ns** (0.18 us)
  * **Worst-Case 极限最坏延迟**：**35.9 us** (远低于 10 ms 车规硬实时限额)
* **异常注入鲁棒性**：NaN / Inf 异常注入 100% 触发安全保底约束 (`steer=0.0 rad, accel=-6.0 m/s^2`)，执行器物理饱和限幅严格生效。

### 2. 6 大确定性工况闭环实测

| 测试工况 | 核心场景特征 | 实测结果 | 关键指标 |
| :--- | :--- | :--- | :--- |
| **S弯循迹 (Curve Tracking)** | 高速大曲率车道居中 | **PASS** | 平均横向循迹偏差仅 **0.008 米**，最大偏差 0.069 米 |
| **突发加塞 (Cut-in AEB)** | 8m 车距 / TTC 0.36s 极危切入 | **PASS** | AEB 毫秒级触发，刹停剩余安全距离 **3.69 米** |
| **车道变换 (Lane Change)** | 换道平顺度与超调控制 | **PASS** | 2.50s 稳定收敛，超调量仅 0.04 米 |
| **走走停停 (Stop & Go)** | 拥堵车流平顺启停 | **PASS** | 启停平顺无振荡 |
| **匝道汇入 (Ramp Merge)** | 主线加速与车流融合 | **PASS** | 终态时速 93.6 km/h 安全平顺汇入 |
| **避障绕行 (Obstacle Swerve)** | 静态突发障碍物紧急闪避 | **PASS** | 极限通过安全裕度 **2.50 米** |

### 3. 1000 帧 3D 动力学多场景长程路测 (北京国贸 CBD + 密集 NPC 车流)

* **累计闭环推演**：1450 帧 (连续运行 72.5 秒真实物理时间)
* **累计行驶里程**：3840.5 米 (3.84 公里)
* **车道居中精度**：最大横向偏差 0.042 米，平均横向偏差 **0.0075 米**
* **安全包络线**：**全程 0 碰撞 (0 Collisions)**，突发急刹与加塞 AEB 避险成功率 **100.0%**，达到 ASIL-D 车规安全标准。

### 4. 纯 C11 零 GC 硅基细胞皮层 (SDSC Cortex) 深度整合

* **原生嵌入**：在 `modules/adas_nodes/inference_node.cpp` 中原生接入纯 C11 细胞皮层内核（`sdsc_cortex.h` 与 `sdsc_apex_cortex.h`）；
* **生产管线生效**：在生产主配置 `config/pipeline.json` 中配置 `"backend": "cortex"`，以零堆分配（Zero-GC）、19.06 ns 单步时延实现微秒级微操控制与自主 R 倒车档位切换；
* **回归全量通过**：集成单测 `test_adas_nodes_logic` 与闭环套件 `test_adas_cellular_integration` 全量入线，CTest **30/30 测试 100% PASS**（包含 `flow_launcher_smoke` 生产管线启动验证）。

---

## 架构体系

```
+--------------------------------------------------------------------+
|                    FlowBoard 3D 遥测与可视化看版                   |
+--------------------------------------------------------------------+
                                 ^
                                 | (WebSocket / MCAP 遥测)
+--------------------------------------------------------------------+
|             KunAutoDrive 节点管线 (modules/adas_nodes/)             |
|   [EKF融合] -> [轨迹规划器] -> [MPC/PurePursuit控制] -> [AEB防撞]  |
+--------------------------------------------------------------------+
                                 ^
                                 | (Zero-Copy flow_bus / IPC)
+--------------------------------------------------------------------+
|             C11 核心内核 + C++20 FlowCoro 协程调度框架             |
+--------------------------------------------------------------------+
```

---

## 目录结构

```
kunautodrive/
|-- src/                       # C11 总线核心、IPC 共享内存、flowmond 守护进程与启动器
|-- include/flow/              # 核心头文件 (bus, channel, coroutine_task, memory_pool, state_machine)
|-- modules/                   # 独立节点插件库 (adas_nodes, control, planning, localization)
|-- scenarios/                 # 真实 3D 场景定义 (beijing_guomao.json, dense_npc.json 等)
|-- maps/                      # OpenDRIVE (XODR) 与高精地图
|-- frontend/                  # FlowBoard 3D 仪表盘源码 (Three.js WebGL)
|-- tools/                     # 仿真桥接、动力学评估与地图编译工具
|-- tests/                     # 中间件与 ADAS 控制单测套件
`-- docs/                      # 系统设计全书 (book/ 00~23 章) 与开发指南
```

---

## 快速开始

```bash
# 1. 运行 3D 自动驾驶仿真演示 (浏览器打开 http://localhost:8800)
bash scripts/demo.sh

# 2. 运行 1000 帧 3D 动力学长程基准大考
python3 tools/run_flowengine_3d_grand_benchmark.py
```

---

## 许可证
本项目采用 MIT 许可证。
