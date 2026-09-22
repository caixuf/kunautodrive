/**
 * test_ltv_mpc.c — LTV-MPC Riccati 递归 + KKT 单测
 *
 * 历史 bug（2026-09 handoff §4.4 控制 W1）：
 *   p 递推里：
 *     1. 用刚被覆盖的 solver->P（=P_k）当作 P_{k+1}
 *     2. 漏掉 + p_{k+1} 项
 *     3. 多加 K'R·kff 项
 *   → u[0] 是精确解的 ~0.5×（kappa≠0 时偏差更大，实测 0.173×）。
 *
 * 用例：
 *   1. 直线 kappa=0, x0=(0.01,0,0)：u[0] 应 = K0[0]*0.01
 *      → 修前/修后数值一致（c=0 时 bug 不显现，是 sanity check）
 *   2. 直线 kappa=0, x0=(0,0,0.01)：u[0] 应 = K0[2]*0.01
 *      → sanity check（同上）
 *   3. 曲率 kappa=0.005, x0=(0.01,0,0)：u[0] 应 = K0[0]*0.01 + kff0[0]
 *      → **关键 bug 暴露**：修前 u[0]=0.0075 vs 修后 0.0434（5.8× 偏差）
 *   4. 零状态：kappa=0 → u[0]=0；kappa=0.005 → u[0]=kff0[0]
 *      → 边界确定性
 *   5. Riccati 收敛：N=1, 5, 20, 60 时 P_0 应单调收敛
 *      → 数值稳定性
 *
 * 数值精度：所有比较用 1e-6 容差（C 求解器是 double 精度 Riccati，无截断误差累积）。
 */

#include "ltv_mpc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while(0)

#define CHECK_NEAR(actual, expected, eps, name) do { \
    double _a = (actual), _e = (expected); \
    if (fabs(_a - _e) > (eps)) { \
        printf("  FAIL %s  %.10f vs %.10f (|Δ|=%.2e > %.0e)\n", \
               name, _a, _e, fabs(_a - _e), (double)(eps)); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while(0)

static int g_pass = 0, g_fail = 0;

/* 静态参考值（纯 Python 标准 Riccati 算出，对照算法见 handoff §4.4 控制 W1） */
static const double K0_REF[3] = {
    -2.746482270899862, -12.059188446534960, -11.205137281374395
};
static const double KFF0_KAPPA0   = 0.0;
static const double KFF0_KAPPA005 = 0.070914650933649;

static LtvMpcConfig default_cfg(void) {
    LtvMpcConfig cfg = ltv_mpc_default_config();
    cfg.max_steer  = 0.16;
    cfg.max_dsteer = 0.5;
    return cfg;
}

/* 用例 1+2：直线 kappa=0 */
static void test_kappa0_small_state(void) {
    printf("\n=== [Case 1+2] kappa=0, x0=(0.01,0,0)/(0,0,0.01) ===\n");

    LtvMpcConfig cfg = default_cfg();
    LtvMpcSolver* solver = ltv_mpc_create(&cfg);

    double v_ref[LTV_MPC_MAX_HORIZON];
    double kappa_ref[LTV_MPC_MAX_HORIZON];
    for (int i = 0; i < cfg.horizon; i++) {
        v_ref[i] = 10.0;
        kappa_ref[i] = 0.0;
    }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, cfg.horizon);

    double steer_out = 0.0;

    /* Case 1: x0 = (0.01, 0, 0) — 预期 u[0] = K0[0]*0.01 = -0.0274648... */
    ltv_mpc_set_state(solver, 0.01, 0.0, 0.0, 10.0);
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
    double u1_expected = K0_REF[0] * 0.01 + KFF0_KAPPA0;
    CHECK_NEAR(steer_out, u1_expected, 1e-9, "x0=(0.01,0,0) u[0]");

    /* Case 2: x0 = (0, 0, 0.01) — 预期 u[0] = K0[2]*0.01 = -0.112051... */
    ltv_mpc_set_state(solver, 0.0, 0.0, 0.01, 10.0);
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
    double u2_expected = K0_REF[2] * 0.01 + KFF0_KAPPA0;
    CHECK_NEAR(steer_out, u2_expected, 1e-9, "x0=(0,0,0.01) u[0]");

    ltv_mpc_destroy(solver);
}

/* 用例 3：曲率 kappa=0.005 — **暴露 p 递推 bug 的关键场景** */
static void test_kappa_nonzero(void) {
    printf("\n=== [Case 3] kappa=0.005, x0=(0.01,0,0) — p 递推 bug 暴露 ===\n");

    LtvMpcConfig cfg = default_cfg();
    LtvMpcSolver* solver = ltv_mpc_create(&cfg);

    double v_ref[LTV_MPC_MAX_HORIZON];
    double kappa_ref[LTV_MPC_MAX_HORIZON];
    for (int i = 0; i < cfg.horizon; i++) {
        v_ref[i] = 10.0;
        kappa_ref[i] = 0.005;  /* 非零曲率让 c≠0，p 递推项才非零 */
    }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, cfg.horizon);

    double steer_out = 0.0;
    ltv_mpc_set_state(solver, 0.01, 0.0, 0.0, 10.0);
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);

    double u3_expected = K0_REF[0] * 0.01 + KFF0_KAPPA005;
    /* 修前实测 u[0]=0.0075（5.8× 偏差）；修后应 = 0.0434498... */
    CHECK_NEAR(steer_out, u3_expected, 1e-9, "kappa=0.005 u[0] (p 递推修复证据)");

    ltv_mpc_destroy(solver);
}

/* 用例 4：零状态 → u[0] 应 = kff[0]（=0 当 kappa=0，≠0 当 kappa≠0） */
static void test_zero_state(void) {
    printf("\n=== [Case 4] x0=(0,0,0) — 零初始响应 = kff[0] ===\n");

    LtvMpcConfig cfg = default_cfg();
    LtvMpcSolver* solver = ltv_mpc_create(&cfg);

    double v_ref[LTV_MPC_MAX_HORIZON];
    double kappa_ref[LTV_MPC_MAX_HORIZON];

    /* kappa=0：u[0] 应严格 = 0 */
    for (int i = 0; i < cfg.horizon; i++) {
        v_ref[i] = 10.0;
        kappa_ref[i] = 0.0;
    }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, cfg.horizon);
    ltv_mpc_set_state(solver, 0.0, 0.0, 0.0, 10.0);
    double steer_out = 0.0;
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
    CHECK_NEAR(steer_out, 0.0, 1e-12, "x0=0,kappa=0 → u[0]=0");

    /* kappa=0.005：u[0] 应 = kff0[0]（前馈曲率项） */
    for (int i = 0; i < cfg.horizon; i++) {
        v_ref[i] = 10.0;
        kappa_ref[i] = 0.005;
    }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, cfg.horizon);
    ltv_mpc_set_state(solver, 0.0, 0.0, 0.0, 10.0);
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
    CHECK_NEAR(steer_out, KFF0_KAPPA005, 1e-9, "x0=0,kappa=0.005 → u[0]=kff0[0]");

    ltv_mpc_destroy(solver);
}

/* 用例 5：Riccati 收敛性 — N=1 时 P_0 偏离代数解最远；N→∞ 时收敛到稳定值 */
static void test_horizon_convergence(void) {
    printf("\n=== [Case 5] Riccati 收敛 — u[0] 随 N 单调变化（N 越大越接近代数解）===\n");

    /* 测试 N=1, 5, 20, 60 四个 horizon 对同一状态的响应。
     * 由于解的初值固定（N=60），可只测一个 horizon=60 + sanity check。 */
    LtvMpcConfig cfg = default_cfg();
    cfg.horizon = 60;
    LtvMpcSolver* solver = ltv_mpc_create(&cfg);

    double v_ref[60], kappa_ref[60];
    for (int i = 0; i < 60; i++) { v_ref[i] = 10.0; kappa_ref[i] = 0.005; }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, 60);
    ltv_mpc_set_state(solver, 0.01, 0.0, 0.0, 10.0);

    double steer_out = 0.0;
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
    /* N=60 与参考值（也是 N=60 算出）一致 */
    CHECK_NEAR(steer_out, K0_REF[0] * 0.01 + KFF0_KAPPA005, 1e-9,
               "N=60 → 与参考值一致");

    ltv_mpc_destroy(solver);

    /* 验证 N 缩短时 u[0] 单调趋近 N=60 值（短时域 → 次优 → 与代数解偏差更大） */
    int Ns[] = {1, 5, 20, 60};
    double us[4];
    for (int t = 0; t < 4; t++) {
        cfg.horizon = Ns[t];
        solver = ltv_mpc_create(&cfg);
        for (int i = 0; i < Ns[t]; i++) { v_ref[i] = 10.0; kappa_ref[i] = 0.005; }
        ltv_mpc_set_reference(solver, v_ref, kappa_ref, Ns[t]);
        ltv_mpc_set_state(solver, 0.01, 0.0, 0.0, 10.0);
        CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);
        us[t] = steer_out;
        ltv_mpc_destroy(solver);
    }
    for (int t = 1; t < 4; t++) {
        /* |u[N] - u[N=60]| 应随 N 缩短而增大（N=60 已是参考解） */
        double diff_prev = fabs(us[t-1] - us[3]);
        double diff_cur  = fabs(us[t]   - us[3]);
        printf("  N=%2d  u=%.10f  |u-u(N=60)|=%.2e\n",
               Ns[t], us[t], diff_cur);
        CHECK(diff_cur <= diff_prev + 1e-12);
    }
}

/* 用例 6：限幅不破坏 Riccati 一致性 */
static void test_steer_clipping(void) {
    printf("\n=== [Case 6] 大初始状态 → u[0] 截幅到 max_dsteer ===\n");

    LtvMpcConfig cfg = default_cfg();
    LtvMpcSolver* solver = ltv_mpc_create(&cfg);

    double v_ref[LTV_MPC_MAX_HORIZON];
    double kappa_ref[LTV_MPC_MAX_HORIZON];
    for (int i = 0; i < cfg.horizon; i++) { v_ref[i] = 10.0; kappa_ref[i] = 0.0; }
    ltv_mpc_set_reference(solver, v_ref, kappa_ref, cfg.horizon);

    /* delta=0.5（接近 max_steer=0.5），让 K0[2]*0.5 远超 max_dsteer */
    ltv_mpc_set_state(solver, 0.0, 0.0, 0.5, 10.0);
    double steer_out = 0.0;
    CHECK(ltv_mpc_solve(solver, &steer_out) == LTV_MPC_OK);

    /* 修前 K0[2]*0.5 ≈ -5.6，截幅到 -max_dsteer=-0.5；
     * 修后一致（K 不变，仅 kff/p 影响截幅后值） */
    CHECK_NEAR(steer_out, -cfg.max_dsteer, 1e-9,
               "大 delta → u[0] 截到 -max_dsteer");

    ltv_mpc_destroy(solver);
}

int main(void) {
    printf("LTV-MPC Riccati + KKT 单测\n");

    test_kappa0_small_state();
    test_kappa_nonzero();
    test_zero_state();
    test_horizon_convergence();
    test_steer_clipping();

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}