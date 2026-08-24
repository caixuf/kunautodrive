# 第 21 章：黑盒自动化回归评估体系（Demo Evaluator & CI/CD）

> **本章导读**：
> 在自动驾驶大型工程中，代码的“小改动”极易引发不可预知的“大倒退”（Regression）。例如：修改了横向控制的滤波系数，可能在直线道路上表现更平滑，但在急弯处却导致车辆冲出车道；微调了感知阈值，可能降低了静态假目标误检，却在暴雨场景下导致行人漏检。
>
> KunAutoDrive 构建了完备的 **分层自动化回归评估体系**：从毫秒级的 **L0 静态流水线校验（`pipeline_check.py`）**，到秒级的 **L1 单元测试（CTest）**，再到分钟级的 **端到端黑盒仿真评估器（`demo_evaluator.py`）** 与 **参数敏感度扫描**，筑牢软件质量的护城河。

---

## 1. 软件质量守护：三层金字塔验证阶梯

```
KunAutoDrive 质量验证阶梯:
  ┌─────────────────────────────────────────────────────────────┐
  │  L2 / End-to-End 黑盒回归评估 (demo_evaluator.py, 3~5 分钟) │
  │  • 多场景全栈仿真 (高速、城区、十字路口、掉头、匝道)         │
  │  • 动力学边界考核 (0 碰撞, 0 越界, 舒适度 Jerk, 变道成功率)  │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  L1 / 单元测试与集成测试 (CTest / GTest, 10~30 秒)          │
  │  • 28+ CTest 测试套件 (MessageBus, EKF, FlowCoro, IPC)      │
  │  • 内存泄漏扫描 (Valgrind / ASan / UBSan 内存越界检测)      │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  L0 / 极速静态与流水线门禁 (pipeline_check.py, < 1 秒)      │
  │  • 30 项硬性门禁 (符号命名规范、头文件包含、ABI 门禁版本号)  │
  │  • 架构铁律检查 (禁止业务插件直接依赖全局 g_bus)             │
  └─────────────────────────────────────────────────────────────┘
```

---

## 2. L0 极速流水线门禁机制（`tools/pipeline_check.py`）

L0 门禁在每次 `git commit` 或 PR 提交时秒级执行，拦截常见的底层代码异味与架构违规：

```python
# 核心校验规则举例:
# 1. 节点插件 ABI 版本一致性
assert plugin.api_version == NODE_PLUGIN_API_VERSION, "ABI 版本不匹配"

# 2. 严禁在插件内直接引用全局变量
assert "extern MessageBus g_bus" not in code, "违背依赖注入铁律"

# 3. 前端离线环境资源合规检查 (严禁外部 CDN 外链)
assert "http://" not in frontend_code and "https://" not in frontend_code
```

---

## 3. L2 黑盒回归评估器（`tools/demo_evaluator.py`）

`demo_evaluator.py` 是评估 ADAS 栈整体表现的核心引擎。它以黑盒方式拉起完整的多进程/多线程 Pipeline，驱动场景回放，并逐帧记录车辆轨迹与物理指标。

### 3.1 核心考核指标与评分权重
评估器在测试结束后自动输出结构化评分报告：

| 指标类别 | 考核项目 | 达标红线 (Pass/Fail) | 权重 |
| :--- | :--- | :--- | :--- |
| **安全性 (Safety)** | 碰撞次数 (Collisions) | **严格等于 0** | 40% |
| **安全性 (Safety)** | 道路越界 (Road Departure) | **严格等于 0** (横偏 $< 1.75\text{ m}$) | 30% |
| **通过性 (Mobility)** | 任务到达率与平均车速 | 耗时在理论基准 $+15\%$ 以内 | 15% |
| **舒适度 (Comfort)** | 加速度与加加速度 Jerk | $\|a_x\| \le 3.0\text{ m/s}^2, \|J\| \le 2.5\text{ m/s}^3$ | 10% |
| **控制精度 (Tracking)** | 稳态横向偏差 (CTE) | $P_{95} < 0.15\text{ m}, \text{Max} < 0.30\text{ m}$ | 5% |

### 3.2 运行与生成测试报告
```bash
# 1. 针对全量场景执行端到端回归评估
python3 tools/demo_evaluator.py --scenarios all --timeout 60

# 2. 导出 HTML / JSON 诊断摘要
python3 tools/demo_evaluator.py --report build/evaluation_report.json
```

---

## 4. 参数敏感度扫描（Sensitivity Grid Search）

在规控算法中，存在大量需要调优的超参数（如 Stanley 增益 $k$、LTV MPC 预测时域 $N_p$、TTC 阈值）。
KunAutoDrive 支持基于网格搜索（Grid Search）的参数敏感度自动扫描：

```bash
# 自动扫描 Stanley 增益 k 从 0.5 到 2.0 对横向误差与 Jerk 的影响
python3 tools/param_sweep.py \
  --param control.stanley_k \
  --range 0.5,2.0,0.1 \
  --scenario scenarios/zhongkai_road_full.json
```

扫描工具自动绘制出 **参数-性能热力图（Sensitivity Pareto Curve）**，辅助工程师选定鲁棒性最强的参数区间。

---

## 5. 工业级避坑指南

### 避坑 1：非确定性测试引发的“间歇性失败（Flaky Tests）”
- **隐患**：若测试脚本依赖操作系统的真实睡眠（如 `time.sleep(1)`），当 CI 服务器 CPU 负载过高时，某些断言可能会因为偶发性超时而报错。
- **解决方案**：测试必须全面接入统一时钟服务（`ClockService`），通过离散逻辑时钟推进，消除对物理时钟的依赖。

### 避坑 2：回归测试场景的覆盖度不足
- 仅用直线场景（`infinite_straight.json`）无法测试掉头和路口让行逻辑。CI/CD 流水线中必须强制覆盖包含 **急弯、汇入、多车交互、掉头与信号灯** 的综合场景集。

---

*第四卷完结。下一章将进入【第五卷：真车部署与硬件落地】，深入探讨 SocketCAN 与 PWM 底盘执行器的真车连接。*
