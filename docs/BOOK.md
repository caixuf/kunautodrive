# KunAutoDrive 技术全书

> **这是什么**：一本**教你亲手造一辆自动驾驶中间件**的书。按「课」组织——每一课从
> 一个问题出发，先让你想自己的答案，再带你读真实代码，最后动手实践。正文在
> `docs/book/` 下，本页是封面 + 目录。
>
> **怎么读**：读一课，做一课，跑一课。每课结尾有练习，「先想，再看答案」。

---

## 总序

[**前言：这本书教你造一辆会自己开的车**](book/00_preface.md) —— 这本书不是什么、
问题驱动的写法、五条职责铁律、贯穿全书的超车例子。**先读。**

全书的骨架是这句话：**规划是大脑、控制是手脚、安全是安全员、仿真是被控对象、行为
只决定「做什么」**——每一课都在验证它。

## 全书目录（九课 + 展望）

| 课 | 主题 | 正文 | 你会亲手经历什么 |
|----|------|------|------------------|
| 一 | 让车动起来 | [book/01_getting_started.md](book/01_getting_started.md) | 坐标系基础、运动学模型、跑通 demo |
| 二 | 手写消息总线 | [book/02_middleware.md](book/02_middleware.md) | Pub/Sub、队列指针、IPC、统一时钟、flowcoro 协程 |
| 三 | 让车学会超车 | [book/03_adas_pipeline.md](book/03_adas_pipeline.md) | 感知→决策→规划→控制→安全，四个真实 bug |
| 四 | 造一个仿真世界 | [book/04_simulation.md](book/04_simulation.md) | 场景即剧本、NPC、digest + invariant |
| 五 | 地图从哪来 | [book/05_map_engine.md](book/05_map_engine.md) | DSL 单一枢纽、高度、A* 路由 |
| 六 | 把世界画出来 | [book/06_visualization.md](book/06_visualization.md) | 坐标唯一入口、死推算、3D 门禁 |
| 七 | 让车学会自己开 | [book/07_learning_loop.md](book/07_learning_loop.md) | 影子模式、四阶段闭环、DAgger |
| 八 | 真车与硬件 | [book/08_hardware.md](book/08_hardware.md) | sim2real 鸿沟、部署五步、RC 小车 |
| 九 | 怎么证明它没坏 | [book/09_engineering.md](book/09_engineering.md) | 黑盒评分、分层门禁、门禁有效 |
| 后记 | 技术展望 | [book/10_future.md](book/10_future.md) | 五轴地图：VLA / 世界模型 / 3DGS / 占据网络 |

## 附录

| 内容 | 位置 |
|------|------|
| A. 术语表 | [GLOSSARY.md](GLOSSARY.md) |
| B. 全书索引（任务 / 主题 / 文件三类） | [INDEX.md](INDEX.md) |
| C. 权威文档导航 | [README.md](README.md) |
| D. API 速查 | [API_QUICK_REFERENCE.md](API_QUICK_REFERENCE.md) |

---

## 阅读路线

想快速上手的按顺序读一到四课；想先看全貌的可以直接读课九（验证体系）。按角色：

### 路线一 · 新手（0 → 跑通 → 理解链路）
课一 → 课二 → 课三 → 课九。
**产出**：能跑 demo、改场景、看懂节点输入输出、知道改完怎么验证。

### 路线二 · 造中间件的人
课二 → 课五 → 课六。
**产出**：能回答「加一个新节点/新 topic/新模块，改动面在哪」。

### 路线三 · 做算法的人
课三 → 课四 → 课九。
**产出**：能按「Python 仿真先行 → 移植 C++ → 评估器验证」流程改算法。

### 路线四 · 做前端 3D 的人
课六 → 课九（门禁）。
**产出**：能新增 View、修渲染 bug、通过 `npm run vis:check:all`。

### 路线五 · 做部署的人
课一 → 课八 → 课七（影子推理）。
**产出**：能把系统部署到真车并调试。

---

## 维护约定

- **改运行时行为** → 更新对应模块的权威文档；`docs/book/` 的正文只补原理与经验，
  不承载最新契约。
- **改 API / JSON 字段** → 更新对应契约文档（`road_network` 只在
  [FLOWBOARD_SCENE_CONTRACT.md](FLOWBOARD_SCENE_CONTRACT.md) 定义）。
- **新增 / 删除 / 重命名文档** → 同步维护本目录 + [INDEX.md](INDEX.md)，避免脱节。
- 每课正文结尾的「练习」与「深入阅读」指向权威文档；权威文档字段以自身为准。
- 本书正文不复制权威文档正文，代码示例遵循项目编码规范（cJSON / clock_now_us /
  node_pump）。
