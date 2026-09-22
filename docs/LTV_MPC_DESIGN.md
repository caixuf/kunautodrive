# LTV MPC 横向控制器 — 设计文档

> `src/algorithms/ltv_mpc.c` 是横向控制的核心算法（绕参考轨迹线性化的
> LTV 模型 + 离散 Riccati 递归）。本文沉淀其数学形式、实现细节、
> 失败兜底、与同类实现的对比（避免下次又有人问"为什么不用 CVXPY"），
> 以及踩过的坑与防回归护栏。改 `ltv_mpc.c` 之前先读本文。

## 1. 算法定位

- **唯一职责**：在已知参考轨迹（`v_ref`、`kappa_ref`）和当前状态
  （`e_y`、`e_psi`、`delta`、`v`）下，求解最优转向速率增量 `ddelta`。
- **不直接给油门/刹车**——油门由独立纵向控制器处理，本控制器只跟轨迹。
- **失败不静默**：4 个错误码 + `LTV_MPC_DEGRADED` 状态机兜底；调用方
  `control_node.cpp:976-987` 失败时立即切 Stanley 横向 / 机动模式，
  不会让车辆在无控状态下继续行驶。

## 2. 数学形式

### 2.1 状态空间

绕参考轨迹线性化的**误差动力学**（error dynamics）：

```
状态  x = [e_y, e_psi, delta]^T            (3 维)
控制  u = ddelta                           (1 维)
```

线性化点：参考速度 `v(k)`、参考曲率 `kappa(k)`。

### 2.2 离散模型（一阶 Euler，dt=0.025s）

```
e_y[k+1]   = e_y[k]   + dt * (v(k)*e_psi[k] + 0.5*v(k)*delta[k])
e_psi[k+1] = e_psi[k] + dt * (v(k)/L * delta[k]  - kappa(k)*v(k))
delta[k+1] = delta[k] + dt * u[k]
```

对应实现 `ltv_mpc.c:181-190` 的 `A, B, c` 构造：

```c
A[0][0] = 1.0;  A[0][1] = v_safe*dt;  A[0][2] = 0.5*v_safe*dt;
A[1][1] = 1.0;  A[1][2] = v_safe/L*dt;
A[2][2] = 1.0;
B[0] = 0.0;  B[1] = 0.0;  B[2] = dt;
c[1] = -kk*v_safe*dt;  /* 曲率前馈项 */
```

`v_safe = max(v(k), 0.01)`（`ltv_mpc.c:175,294`）——避免低速除零，
不是 LTV 本身的限制。

### 2.3 代价函数

```
J = sum_{k=0..N-1} [ x_k^T Q x_k + u_k^T R u_k ] + x_N^T Qf x_N
```

```
Q  = diag(q_y, q_psi, q_delta)
R  = r_ddelta                    (标量)
Qf = diag(qf_y, qf_psi, 0)       (delta 终端不约束)
```

默认值（`ltv_mpc.c:118-131`）：

| 权重 | 值 | 含义 |
|------|------|------|
| `q_y`    | 10  | 横向误差 |
| `q_psi`  | 80  | 航向误差（高权重抑制点头） |
| `q_delta`| 1   | 大转角惩罚 |
| `r_ddelta`| 1  | 转向速率（平顺性） |
| `qf_y`   | 20  | 终端横向 |
| `qf_psi` | 160 | 终端航向 |

标定来源：2026-08-15 control_sim 扫参（变道三场景 3/3 + 弯道 6/6 全过）。

### 2.4 求解：离散 Riccati 后向递推 + 前向 rollout

- 后向 `k=N-1 → 0`：求 `K[k]` 反馈增益 + `kff[k]` 前馈项。
  - Quu = R + B^T P B；Quu_inv = 1/Quu
  - K[k] = -Quu_inv · B^T P A
  - kff[k] = -Quu_inv · B^T (P c + p)
  - P_k = Q + (A+BK)^T P_{k+1} (A+BK) + K^T R K
- 前向 `k=0 → N-1`：u[k] = K[k] x[k] + kff[k]；前向积分得 x[k+1]。

**3×3 矩阵运算全程内联**（`ltv_mpc.c:21-49`），O(N) 时间，典型 N=60
单次求解 < 1ms。

### 2.5 控制输出转角

`ltv_mpc_solve` 输出 `ddelta`（转向速率，rad/s），不是转角本身。
调用方负责积分：

```cpp
// control_node.cpp:980
steer = g.prev_steer + mpc_steer_delta * g.ltv_mpc_cfg.dt;
```

> ⚠️ **历史 bug**：早期版本漏乘 `dt`，每帧舵量被放大 ~20×
> （`798523b` 之前的状态）。现在由 control_node.cpp:980 严格处理。

## 3. 失败兜底与状态机

### 3.1 错误码（`ltv_mpc.h:36-40`）

| 错误码 | 含义 | 触发条件 | 调用方处理 |
|--------|------|----------|-----------|
| `LTV_MPC_OK` (=0) | 求解成功 | Riccati 收敛 + 前向 rollout 无 NaN | 取 `*steer_out` 继续 |
| `LTV_MPC_ERR_N` (=-1) | N 越界 | `N < 1` 或 `N > 80` | status=DEGRADED；切换兜底 |
| `LTV_MPC_ERR_SINGULAR` (=-2) | Hessian 病态 | `|Quu| < 1e-12` | status=DEGRADED；切换兜底 |
| `LTV_MPC_ERR_ITER` (=-3) | 迭代耗尽 | 当前实现未使用 | (预留) |
| `LTV_MPC_ERR_NAN` (=-4) | 解发散 | `isnan(x)` 或 `isnan(u)` | status=DEGRADED；切换兜底 |

### 3.2 状态机（`ltv_mpc.h:60-64`）

```c
typedef enum {
    LTV_MPC_ACTIVE = 0,     // 正常运行
    LTV_MPC_DEGRADED = 1,   // 不收敛，等待降级路径
    LTV_MPC_INIT = 2,       // 未初始化
} LtvMpcStatus;
```

### 3.3 调用方 fallback 链

```
control_node.cpp 主循环:
    int rc = ltv_mpc_solve(g.ltv_mpc, &mpc_steer_delta);
    if (rc == LTV_MPC_OK) {
        steer = g.prev_steer + mpc_steer_delta * dt;  // 积分
        mpc_used = true;
    }
    // mpc_used==false → Stanley 横向 / 机动模式 / park 兜底
```

降级不是"重新求解"——是**架构层 failover**到几何横向控制器，跟
mcarfagno 的 `print + emergency_controls` 单线降级完全不同。

## 4. 与 mcarfagno/mpc_python 的对比（2026-09 借鉴评估）

| 维度 | 我们 `ltv_mpc` | mcarfagno/mpc_python |
|------|----------------|----------------------|
| 状态空间 | 误差系 `[e_y, e_psi, delta]` (3维) | 全局 `[x, y, v, θ]` (4维) |
| 控制量 | 转向速率 `ddelta` (1维) | 加速度+转角 `[a, δ]` (2维) |
| 求解 | **Riccati 递推 O(N)，<1ms** | CVXPY + Clarabel SOCP，毫秒级 |
| 避障 | 单独在 `st_graph.c` DP 速度规划 | MPC 内嵌半平面（仅单障碍物） |
| 失败兜底 | **4 错误码 + DEGRADED + 架构 failover** | 打印 + 紧急刹车序列 |
| 适用 | 嵌入式 / 实时 / 真车 | 研究 / Python 验证 / 仿真 |

**借鉴评估结论**（详见本会话 handoff）：

- ✅ **直接借鉴**：Riccati 早退（Issue B，待做）+ 失败兜底思路
  （已实现，比 mcarfagno 更彻底）
- ❌ **不适用**：sqrt-Q trick（Riccati 不构造 sum_squares）、
  iMPC 外层迭代（待评估）、CVXPY 整体（不是生产代码路径）

## 5. 经验坑速查（改 ltv_mpc.c 必读）

1. **`P_{k+1}` 与 `P_k` 覆盖陷阱**：`ltv_mpc.c:230-237` 必须在更新
   `solver->P` 之前 `memcpy(P_old, solver->P)`——因为后向递推里
   `kff[k] = -Quu_inv · B^T (P c + p)` 用的还是 `P_{k+1}`，被覆盖后就
   会引用错位的 P_k。2026-09 p-递推修复 commit
   （`99c1124` 之前的历史 bug）就是这个原因——`u[0]` 偏离精确解
   ~0.173×~5.8×。**Issue A7 用例锁定此不变量**。
2. **p 递推零参考的特殊形式**：零参考（x_ref=0）时 p_N=0，
   `p_k = A_cl^T (P_{k+1} c_k + p_{k+1})`，**没有** `K^T R kff` 项。
   多这一项就会让前馈偏移，弯道偏差更明显。`ltv_mpc.c:262-273`
   的注释里写明了这点，**别抄错的 affine-LQR 公式**。
3. **NaN 必须显式拦截**：`ltv_mpc.c:308-311` 每步前向 rollout 后
   `isnan` 检查——不是优化，是为了阻止 NaN 沿 k=0..N 扩散到控制
   输出。**Case A6 用例固定此行为**。
4. **`max_steer` 是默认包络，运行时按速度收紧**：`cfg.max_steer=0.16`
   只是默认上限；`control_node` 用 `steer_limit_for_speed(v, lat_env)`
   逐帧覆盖，避免模型误以为有 0.35 舵量。改求解器 max 时**别动**
   `ltv_mpc_default_config` 默认值——它是模型假设，不是策略参数。
5. **`v_safe = max(v, 0.01)` 不是 bug**：低速死区，避免线性化矩阵
   全零导致 `v_safe / L * dt` 退化。改这个值要同步看 Case A1/A2。
6. **`r_ddelta = 0` 会触发 SINGULAR**（设计预期）：Riccati 后向
   递推 P_22[k] 退化到 0，末端 `Quu = 0 + BPB < 1e-12` → ERR_SINGULAR。
   **Case A5 锁定此行为**——别"修"它，它是兜底机制，不是 bug。
7. **重复 solve 不应有状态污染**：`solver->P` 每次从 `P_N = Qf`
   重新起算（`ltv_mpc.c:154`），其他都是局部变量。
   **Case A7 跑 1000 次验证 max |Δ| < 1e-15**。

## 6. 防回归护栏（2026-09 起）

`modules/adas_nodes/test_ltv_mpc.c`（注册为 ctest `ltv_mpc_kkt`）：

| 用例 | 验证什么 | 防什么 |
|------|----------|--------|
| 1+2 (kappa=0) | `u[0] = K0[0]*0.01` 精确值（1e-9） | K[k] 计算正确 |
| 3 (kappa=0.005) | `u[0] = K0[0]*0.01 + kff0` 精确值（1e-9） | **p 递推正确性**（暴露 5.8× 偏差 bug） |
| 4 (零状态) | x0=0,kappa=0 → u=0；kappa=0.005 → u=kff | 边界确定性 |
| 5 (horizon 收敛) | N=60 解与 N=20 / N=5 单调收敛 | 数值稳定性 |
| 6 (限幅) | 大 delta → u 截到 ±max_dsteer | Riccati 截幅不破坏一致性 |
| A1 (v_ref 死区) | v_ref=0,v=10 → 解稳定非 NaN | v_safe 钳位分支不爆炸 |
| A2 (v=0) | ref=0, v=0 → 输出 ≈0 | 停车边界 |
| A3 (horizon 边界) | N=0 → ERR_N+DEGRADED；N=1 → OK | 边界 N 值 |
| A4 (q_y=0) | Riccati 仍收敛，不 SINGULAR | q_psi 维持 PD |
| A5 (r_ddelta=0) | ERR_SINGULAR+DEGRADED | 1e-12 阈值兜底不是崩 |
| A6 (NaN) | ERR_NAN+DEGRADED | 不静默 OK |
| A7 (重复 1000 次) | max \|Δ\| < 1e-15 | 求解器无状态污染 |

**总计 1041 个 CHECK，0 fail**（5 旧 + 7 新，2026-09 起）。

修改 `ltv_mpc.c` 任何一行都要跑：

```bash
cmake --build build --target test_ltv_mpc
env -u LD_LIBRARY_PATH build/bin/test_ltv_mpc
env -u LD_LIBRARY_PATH ctest --test-dir build -R '^ltv_mpc_kkt$'
```

Case 3 的 `u3_expected = K0_REF[0] * 0.01 + KFF0_KAPPA005` 是**黄金参考**，
任何让 `u[0]` 偏离它的改动都视为回归（除非同时更新 K0_REF + 文档）。

## 7. 未来工作（候选）

- **Issue B**：外层 iMPC 迭代（沿猜测轨迹重新线性化 + 收敛判定早退）。
  护栏已就绪（A1-A7），动求解器核心前先确认这些用例不回归。
- **Issue C**：暴露 DEGRADED 计数到 dashboard（当前调用方吃掉）。
- **更精细的权重自适应**：`q_y/q_psi` 当前固定，速度耦合未做（弯道
  高速时航向误差容忍度应放宽）；plan 阶段已规划但未实施。

## 8. 文档覆盖现状

- `docs/CALIBRATION_GUIDE.md`：标定流程入口，含 control_sim 扫参命令
- `docs/ALGORITHM_STACK.md`：横向控制定位（与 Stanley/PID 并列）
- `docs/book/07_state_machine.md`：未涉及本模块
- **本文**：唯一详细设计文档

新增/修改 `ltv_mpc.c` 必须同步更新本文 §5（坑）和 §6（用例）。