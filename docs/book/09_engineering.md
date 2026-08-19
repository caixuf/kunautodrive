# 卷九 · 第九课：怎么证明它没坏

## 本章问题

你写完了整套系统（卷一~卷八）。现在你改了一行代码——把变道的判定阈值从 25 米改成
20 米。

**你怎么知道这行改动没把别的东西弄坏？**

最朴素的想法：跑一遍 demo，看看车有没有撞。

但你会立刻遇到三个问题：

1. **不可复现**：demo 有随机性，这次没撞，不代表下次不撞。你没法说「这行改动是安全的」。
2. **看不清**：肉眼只能看出「撞没撞」，看不出「偏航多了 2 度」「变道多了 3 次」——而这
   些微小的退化，积累起来就是一次事故。
3. **没有历史**：你怎么知道「现在的表现」比「改动前」好还是差？你连「改动前是什么样」
   的记录都没有。

这一章是全书最重要的一课：**怎么证明系统没坏。**

## 你自己的答案：肉眼 + 感觉

「看起来没问题」——这是新手唯一的验证手段。它的全部问题，在于三个字：**不可复现、
不量化、无历史。** 一个「看起来没问题」的系统，可能已经悄悄退化了一周，只是你没察觉。

## 真正的方案 1：把「表现」变成数字（黑盒评分）

验证的第一步，是把「表现」从「感觉」变成「数字」。项目的方法是：

> **每个节点发布 debug JSON topic -> monitor 聚合到唯一公共数据源
> `/tmp/flow_topology.json` -> 评估器 / 仪表盘 / 追溯全部消费同一份数据。**

评估器（`demo_evaluator.py`）**黑盒**地采样这份数据，不看内部实现，只看行为指标：

| 指标 | 问的是什么 | 检测方法 | 阈值 |
|------|-----------|---------|------|
| 碰撞 | 撞了吗？ | entity bbox 重叠检测 | 0 次（WARN） |
| 路沿偏离 | 开出路面了吗？ | road_network cross-track | > 2m（FAIL） |
| 停滞 | 停着不动了吗？ | 连续 N 帧 speed < 0.5 | > 10s（WARN） |
| 变道次数 | 变道太频繁吗？ | lateral offset 变化计数 | > 5 次/分钟（WARN） |
| 偏航抖动 | 方向盘抖吗？ | heading 二阶差分 | > 0.1 rad/s^2（WARN） |
| topic 频率 | 每个节点还在按时发消息吗？ | 相邻消息时间戳差 | 超时 2x（FAIL） |
| NPC 瞬移 | NPC 突然跳位置了吗？ | 相邻帧位置差 | > v_max * dt（FAIL） |

**为什么必须是「黑盒」？** 因为黑盒评分不关心「你改了哪一行」——它只关心「车还是不是
那辆正常的车」。**评估器是唯一的裁判，不看动机，只看行为。**

### 评估结果格式

```json
{
  "summary": "PASS",
  "score": 92.5,
  "checks": [
    { "name": "collision", "status": "PASS", "detail": "0 collisions" },
    { "name": "road_departure", "status": "PASS", "detail": "max offset 1.2m" },
    { "name": "stagnation", "status": "WARN", "detail": "stuck 8s at red light (expected)" },
    { "name": "lateral_jerk", "status": "PASS", "detail": "max 0.08 rad/s^2" },
    { "name": "topic_frequency", "status": "PASS", "detail": "all topics alive" }
  ]
}
```

**WARN vs FAIL**：WARN 是「已知问题可忽略」（比如红灯停车被判为停滞），FAIL 是
「必须修复」。CI 门禁只阻断 FAIL。

## 真正的方案 2：分层验证阶梯（从秒级到分钟级）

不同改动需要不同强度的验证。项目把验证分成层，从便宜到贵：

```
L0   pipeline_check.py                    seconds: pipe integrity (no demo launch)
L1   demo_evaluator.py --duration 45      45s: behavioral regression
L1.5 scenario_regression.py               batch: multiple scenarios
L2   test_param_regression.py             A/B: before/after param change
L3   quick_verify.py                      interactive: tune while running
```

**改完代码的默认顺序**：先跑 L0（几秒），再跑 L1（45 秒），涉及场景的跑 L1.5，改参数
的跑 L2。**不是每次都要跑全**——但「一次都不跑」是绝对不行的。

**这就是「分层」的意义**：便宜的检查先拦住明显错误，贵的检查用于深度验证。全部跳过的
代价，是某个你没想到的地方悄悄坏了。

### L0：管道完整性检查（秒级）

```bash
python3 tools/pipeline_check.py
```

检查 9 类 32 项指标：节点注册、topic 连接、参数完整性、scenario 引用……不启动 demo，
不跑仿真，纯静态分析。**改完代码第一件事就是跑它。**

### L1：45 秒行为回归

```bash
python3 ci/evaluators/demo_evaluator.py --duration 45 --interval 0.5
```

启动完整管线，跑 45 秒 straight_road 场景，采样 `/tmp/flow_topology.json`，
输出评分矩阵。**这是 CI 的默认门禁——每次 push 都跑。**

### L1.5：场景矩阵回归

```bash
python3 ci/evaluators/scenario_regression.py --baseline
```

批量跑多个场景（straight_road, curve_road, dense_npc, multi_light, oncoming,
osm_lujiazui_v2 等），输出每个场景的 PASS/FAIL 矩阵。**改了规划/控制/行为逻辑
必须跑这个。**

### L2：参数回归

```bash
# 保存当前参数的基线表现
python3 ci/evaluators/test_param_regression.py --save-baseline

# ...改参数...

# 对比：退化就 FAIL
python3 ci/evaluators/test_param_regression.py
```

### L3：交互式验证

```bash
python3 tools/quick_verify.py
# 实时仪表盘 + 即时 eval 评分
# 改参数 -> 立刻看到效果 -> 确认无退化
```

## 真正的方案 3：门禁有效——最重要的一句话

现在讲全书最重要的一句话，请放慢读：

> **门禁抓不住已知故障，它的 PASS 就不值得信。**

什么意思？假设你的评估器号称「检测碰撞」，但某次你故意制造一次碰撞，评估器却报了
PASS——那这个评估器的「无碰撞 PASS」就没有任何意义。你永远不知道它什么时候悄悄失效
了。

所以项目给门禁加了三条配套保障：

1. **liveness gate（死信号 FAIL）**：如果采样到的数据是「死的」（比如只读了 1 帧快照，
   所有时序指标都是无效值），必须 FAIL 而不是假装通过。**无法判定 != 通过。**
2. **require 语义**：某个指标「算不出来」，要明确标 `unavailable`，不能拿别的数据
   冒充。比如 ADE/FDE 只有同时给了预测轨迹和真值轨迹才算 `computed`；没有就明确说
   没有——**绝不用错误的数据填出一个假 PASS。**
3. **test_evaluator_gate.py（门禁自测）**：专门写测试验证「这个门禁能抓住已知故障」。
   C 侧有，3D 侧也有（卷六的 REAL_THREE 白名单）。

**这三条保障的本质**：验证工具自己也要被验证。你相信评估器，和评估器值得相信，是
两回事。

## 真正的方案 4：改参数也要验证

回到开头的例子：你改了变道阈值。这不是代码 bug，是「调参」。参数改动同样需要验证
——用参数回归：

```bash
# 先保存基线（当前参数的表现）
python3 ci/evaluators/test_param_regression.py --save-baseline

# ...改参数...

# 对比：退化就 FAIL
python3 ci/evaluators/test_param_regression.py
```

**为什么改参数也要回归？** 因为参数是「隐形的代码」——它不改变逻辑，但改变行为。
一个参数漂移一点点，可能看起来没事，但配合另一个参数，会在某个场景引爆。**参数回归
就是给参数装上版本控制。**

### 调参工具链

项目提供完整的调参工具：

```bash
# auto_tune_mpc.py：自动扫参，找最优 MPC 参数
python3 tools/auto_tune_mpc.py --sweep r_ddelta:0.5-3.0:0.5 r_steer:0.5-2.0:0.5

# flowctl param：运行时热调参（不用重启）
flowctl param list                              # 看当前参数
flowctl param set control.mpc_r_ddelta 2.0      # 改参数，下一帧生效
```

**热调参的三步验证**（CLAUDE.md 铁律）：
1. `params_json` 里加 `cJSON_GetObjectItemCaseSensitive` 解析
2. `param_register_*` 默认值用 `g.<field>` 而非硬编码
3. 逐帧 tick 里 `param_get_float` 重读

**三步缺一步 = 参数改不动或被盖掉。**

## 排查方法论：行为异常从哪下手

最后，当你真的遇到「该停不停/该走不走/刹停到 0/改了代码现象不变」这类 bug 时，项目
沉淀了一套方法论（完整见 `.claude/skills/debugging.md`），核心一句话：**别凭直觉猜层，
用分层探针 + 值传播验证。**

1. **分层探针**：从数据源头（flowsim 真值）逐层往下看——`vehicle/state` -> 感知 ->
   行为 -> 规划轨迹 -> 控制指令 -> safety 输出。每层问：「上游的值有没有传到这层？」
2. **值传播验证**：锁死一个可疑输入，看输出是否按预期变化。比如「planning 输出 0
   速度」——是行为层 `target_speed=0` 传下来了，还是 planning 自己算错了？
3. **状态锁死检查**：很多 bug 是「自维持闭锁」——`v=0 -> target=0 -> 油门=0 -> v=0`，
   一个环把自己锁死。检查有没有「恢复路径」。
4. **缓存层检查**：改了代码但现象不变——先查缓存（浏览器 immutable 缓存、轨迹缓存
   重放、参数热加载没生效）。

配套工具：

```bash
# 事故逐层追溯（碰撞/出路沿复盘）
python3 tools/trace_incident.py

# 轨迹动画分析（可视化规划/控制输出）
python3 tools/motion_analyzer.py
```

## CI 流水线全貌

项目的 CI 有 11 个 job，覆盖从编译到行为的全链路：

```
push/PR 触发:
  scenario-file-gate     -- scenario JSON 引用合法？
  clock-service-gate     -- clock_now_us() 使用正确？
  map-connectivity-gate  -- 车道连通性回归？
  build-release          -- Release 编译 + ctest
  build-asan             -- Address Sanitizer 编译 + test
  build-windows-mingw    -- Windows 交叉编译
  integration-test       -- 插件构建 + 15s 压力 + ctest
  evaluator              -- 45s demo + demo_evaluator.py 评分
  viz                    -- flowmond 冒烟 + pytest
  vis-js-tests           -- vis:check:all + 模型验证

nightly:
  debug + UBSAN + coverage + stress + stability
  benchmark + scenario_regression + driving_school_exam
```

**每次 push 至少跑 11 个 job**。Nightly 额外跑压力测试、覆盖率、基准性能。

## 动手实践

```bash
# 1. L0：不启动 demo，检查管道完整性
python3 tools/pipeline_check.py

# 2. L1：45 秒行为回归
python3 ci/evaluators/demo_evaluator.py --duration 45 --interval 0.5

# 3. 看评估结果里每个指标：碰撞/偏航/停滞/频率
#    注意区分 WARN（已知问题可忽略）和 FAIL（必须修复）

# 4. 挑战：故意制造一个故障，验证门禁能抓住它
#    （比如把 pipeline.json 里某节点的 topic 名改错，跑 L0 看它 FAIL）

# 5. 读评估器源码
cat ci/evaluators/demo_evaluator.py | head -50
```

## 常见陷阱

1. **WARN 不等于 PASS**：WARN 是「已知问题可忽略」，FAIL 是「必须修复」。
2. **一次都不跑 = 裸奔**：哪怕 L0 也要跑。
3. **门禁自测**：评估器自己也要被验证（test_evaluator_gate.py）。
4. **参数回归**：改参数和改代码一样需要回归。
5. **缓存陷阱**：改了代码现象不变 -> 先查缓存。

## 小结

这一课是整个系统的方法论支柱：

- **验证 = 把「表现」变成数字**：黑盒评分，唯一公共数据源 `/tmp/flow_topology.json`。
- **分层阶梯**：L0（秒级）-> L1（行为）-> L1.5（场景）-> L2（参数），从便宜到贵。
- **门禁有效**：门禁抓不住已知故障，PASS 就不值得信——所以有 liveness、require、
  门禁自测。
- **参数也是代码**：改参数用参数回归，给它装版本控制。
- **排查**：分层探针 + 值传播验证 + 状态锁死 + 缓存检查，别凭直觉猜层。

## 练习（选做）

1. **思考题**：为什么「门禁抓不住已知故障 = 它的 PASS 不可信」？提示：如果一个病毒
   扫描器漏报了一次，你还敢相信它的「无病毒」报告吗？
2. **挑战**：你设计了一个「停滞检测」：车 10 秒没动就报 FAIL。但红灯场景下，车本来
   就要停 10 秒以上——你的门禁会误报。怎么修？（提示：卷九的「门禁有效性」；想想
   红灯下「停车」和「卡死」怎么区分）
3. **读代码**：打开 `ci/evaluators/demo_evaluator.py`，找到碰撞检测的逻辑。它看的是
   `sim/collision` topic 还是自己算？为什么？

---

**结语**：九课讲完了。从一辆会动的车（卷一），到一条完整的验证体系（卷九）——你
亲手（在指导下）走完了造一辆自动驾驶中间件的全过程。回到[前言](00_preface.md)，
再看一遍「五条职责铁律」和「验证的哲学」——如果你现在读懂了它们，这本书的目的就
达到了。
