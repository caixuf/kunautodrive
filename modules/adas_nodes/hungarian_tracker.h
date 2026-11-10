/**
 * hungarian_tracker.h — 64 槽 Kuhn-Munkres 匈牙利二分图匹配纯算法头（全仓单一事实源）
 *
 * 供 src/algorithms/kalman_tracker.c、modules/adas_nodes/perception_fusion_node.cpp
 * 及单元测试共享。纯 C/C++ 兼容，定长栈工作数组，零动态内存分配 (zero malloc)。
 */

#ifndef FLOW_HUNGARIAN_TRACKER_H
#define FLOW_HUNGARIAN_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUNGARIAN_TRACKER_MAX 64

/* ── Kuhn-Munkres 匈牙利二分图最优匹配算法 (O(N^3)) ──────────
 *
 * @param n_rows      行数 (如参与关联的 track 数量, <= 64)
 * @param n_cols      列数 (如参与关联的 detection 数量, <= 64)
 * @param cost        代价矩阵 (cost[row][col])
 * @param gate_thresh 关联门限 (cost > gate_thresh 视为不可达)
 * @param row_match   输出匹配结果，row_match[r] 为匹配到的 col 索引，未匹配为 -1
 * @return 成功匹配对数
 */
static inline int hungarian_solve(int n_rows, int n_cols,
                                  const double cost[HUNGARIAN_TRACKER_MAX][HUNGARIAN_TRACKER_MAX],
                                  double gate_thresh,
                                  int row_match[HUNGARIAN_TRACKER_MAX])
{
    if (row_match) {
        for (int i = 0; i < HUNGARIAN_TRACKER_MAX; i++) row_match[i] = -1;
    }
    if (!cost || !row_match || n_rows <= 0 || n_cols <= 0) return 0;

    if (n_rows > HUNGARIAN_TRACKER_MAX) n_rows = HUNGARIAN_TRACKER_MAX;
    if (n_cols > HUNGARIAN_TRACKER_MAX) n_cols = HUNGARIAN_TRACKER_MAX;

    int n = (n_rows > n_cols) ? n_rows : n_cols;
    const double BIG = 1e9;
    const double INF = 1e18;

    /* 1-indexed 方形化填充矩阵 C，确保 n_rows > n_cols 时存在虚拟列绝不死循环 */
    double C[HUNGARIAN_TRACKER_MAX + 1][HUNGARIAN_TRACKER_MAX + 1];
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= n; j++) {
            C[i][j] = BIG;
        }
    }
    for (int i = 0; i < n_rows; i++) {
        for (int j = 0; j < n_cols; j++) {
            double c = cost[i][j];
            C[i + 1][j + 1] = (c <= gate_thresh) ? c : BIG;
        }
    }

    double u[HUNGARIAN_TRACKER_MAX + 1] = {0.0};
    double v[HUNGARIAN_TRACKER_MAX + 1] = {0.0};
    int    p[HUNGARIAN_TRACKER_MAX + 1] = {0};
    int    way[HUNGARIAN_TRACKER_MAX + 1] = {0};

    for (int i = 1; i <= n; i++) {
        p[0] = i;
        int j0 = 0;
        double minv[HUNGARIAN_TRACKER_MAX + 1];
        for (int j = 0; j <= n; j++) minv[j] = INF;
        int used[HUNGARIAN_TRACKER_MAX + 1] = {0};

        do {
            used[j0] = 1;
            int i0 = p[j0];
            double delta = INF;
            int j1 = 0;

            for (int j = 1; j <= n; j++) {
                if (!used[j]) {
                    double cur = C[i0][j] - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }

            for (int j = 0; j <= n; j++) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j]    -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    int match_count = 0;
    for (int j = 1; j <= n; j++) {
        if (p[j] > 0 && p[j] <= n_rows) {
            int r = p[j] - 1;
            int c = j - 1;
            if (c < n_cols && cost[r][c] <= gate_thresh) {
                row_match[r] = c;
                match_count++;
            }
        }
    }

    return match_count;
}

#ifdef __cplusplus
}
#endif

#endif /* FLOW_HUNGARIAN_TRACKER_H */
