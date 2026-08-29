#include "kun/strategy/walk_forward_engine.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <algorithm>

namespace kun {

std::string WalkForwardReport::summary() const {
    std::ostringstream ss;
    ss << "\n=========================================================\n";
    ss << "     KunQuant Walk-Forward 滚动前向优化与稳健性报告        \n";
    ss << "=========================================================\n";
    ss << " 前向样本外有效率 (WFE)   : " << std::fixed << std::setprecision(2) << (walk_forward_efficiency * 100.0) << "%\n";
    ss << " 累计样本外总收益 (OOS PnL): " << overall_oos_pnl << " RMB\n";
    ss << " 策略稳健性判定           : " << (is_robust ? "[ROBUST] 样本外具有真实泛化能力" : "[OVERFITTING] 存在过拟合风险") << "\n";
    ss << " 参数平原检验             : " << (is_parameter_plateau ? "[PASS] 邻域稳定参数平原" : "[ALERT] 孤立尖刺参数") << "\n";
    ss << "\n[滚动切片详细表现]\n";
    for (const auto& s : slices) {
        ss << " 切片 #" << s.slice_index << " | IS夏普=" << s.is_sharpe 
           << " -> OOS夏普=" << s.oos_sharpe << " (最佳参数: 快=" << s.best_fast_window 
           << ", 慢=" << s.best_slow_window << ")\n";
    }
    ss << "=========================================================\n";
    return ss.str();
}

WalkForwardReport WalkForwardEngine::run_walk_forward(
    const std::vector<BarData>& all_bars,
    int num_slices,
    double is_ratio
) {
    WalkForwardReport report;
    if (all_bars.size() < 20) {
        return report;
    }

    size_t total_bars = all_bars.size();
    size_t slice_len = total_bars / num_slices;
    if (slice_len < 10) slice_len = 10;

    double sum_is_sharpe = 0.0;
    double sum_oos_sharpe = 0.0;

    BacktestConfig cfg;
    cfg.initial_capital = 1000000.0;
    cfg.slippage_points = 1.0;
    cfg.commission_ratio = 0.0001;

    for (int i = 0; i < num_slices; ++i) {
        size_t start_idx = i * (total_bars - slice_len) / std::max(1, num_slices - 1);
        size_t end_idx = std::min(total_bars, start_idx + slice_len);

        size_t cur_len = end_idx - start_idx;
        size_t is_len = static_cast<size_t>(cur_len * is_ratio);
        if (is_len < 5 || (cur_len - is_len) < 3) continue;

        std::vector<BarData> is_bars(all_bars.begin() + start_idx, all_bars.begin() + start_idx + is_len);
        std::vector<BarData> oos_bars(all_bars.begin() + start_idx + is_len, all_bars.begin() + end_idx);

        // 1. IS 参数扫描
        int best_fast = 5;
        int best_slow = 20;
        double best_is_sharpe = -999.0;
        double best_is_pnl = 0.0;

        std::vector<std::pair<int, int>> candidates = {{3, 10}, {5, 15}, {5, 20}, {8, 25}, {10, 30}};
        for (const auto& [f, s] : candidates) {
            BacktestEngine is_engine(cfg);
            is_engine.set_history_bars(is_bars);
            is_engine.set_strategy(std::make_shared<DualMaStrategy>("IS_DualMA", "rb2405", f, s, 1.0));
            auto perf = is_engine.run();
            if (perf.sharpe_ratio > best_is_sharpe) {
                best_is_sharpe = perf.sharpe_ratio;
                best_is_pnl = (perf.total_return_pct / 100.0) * 1000000.0;
                best_fast = f;
                best_slow = s;
            }
        }

        // 2. OOS 盲测验证
        BacktestEngine oos_engine(cfg);
        oos_engine.set_history_bars(oos_bars);
        oos_engine.set_strategy(std::make_shared<DualMaStrategy>("OOS_DualMA", "rb2405", best_fast, best_slow, 1.0));
        auto oos_perf = oos_engine.run();

        WalkForwardSliceResult slice_res;
        slice_res.slice_index = i + 1;
        slice_res.is_sharpe = best_is_sharpe;
        slice_res.is_pnl = best_is_pnl;
        slice_res.oos_sharpe = oos_perf.sharpe_ratio;
        slice_res.oos_pnl = (oos_perf.total_return_pct / 100.0) * 1000000.0;
        slice_res.best_fast_window = best_fast;
        slice_res.best_slow_window = best_slow;

        report.slices.push_back(slice_res);
        report.overall_oos_pnl += slice_res.oos_pnl;
        sum_is_sharpe += std::max(0.01, best_is_sharpe);
        sum_oos_sharpe += slice_res.oos_sharpe;
    }

    if (!report.slices.empty()) {
        double mean_is = sum_is_sharpe / report.slices.size();
        double mean_oos = sum_oos_sharpe / report.slices.size();
        report.walk_forward_efficiency = (mean_is > 0.0) ? (mean_oos / mean_is) : 0.0;
        report.is_robust = (report.walk_forward_efficiency >= 0.40) && (report.overall_oos_pnl > 0.0);
        report.is_parameter_plateau = check_parameter_plateau(all_bars, 5, 20);
    }

    return report;
}

bool WalkForwardEngine::check_parameter_plateau(
    const std::vector<BarData>& bars,
    int optimal_fast,
    int optimal_slow
) {
    if (bars.size() < 10) return true;

    std::vector<std::pair<int, int>> neighbors = {
        {optimal_fast, optimal_slow},
        {std::max(2, optimal_fast - 1), optimal_slow},
        {optimal_fast + 1, optimal_slow},
        {optimal_fast, std::max(5, optimal_slow - 2)},
        {optimal_fast, optimal_slow + 2}
    };

    BacktestConfig cfg;
    cfg.initial_capital = 1000000.0;

    std::vector<double> sharpes;
    for (const auto& [f, s] : neighbors) {
        BacktestEngine eng(cfg);
        eng.set_history_bars(bars);
        eng.set_strategy(std::make_shared<DualMaStrategy>("Plateau_Test", "rb2405", f, s, 1.0));
        auto perf = eng.run();
        sharpes.push_back(perf.sharpe_ratio);
    }

    double opt_sharpe = sharpes[0];
    if (opt_sharpe <= 0.0) return true;

    for (size_t i = 1; i < sharpes.size(); ++i) {
        if (sharpes[i] < opt_sharpe * 0.35) {
            return false;
        }
    }
    return true;
}

} // namespace kun
