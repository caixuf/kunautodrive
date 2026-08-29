#pragma once

#include "kun/core/types.hpp"
#include <vector>
#include <string>

namespace kun {

struct PerformanceStats {
    double initial_capital{1000000.0};
    double final_equity{1000000.0};
    double total_return_pct{0.0};
    double annualized_return_pct{0.0};
    double max_drawdown{0.0};
    double max_drawdown_pct{0.0};
    double sharpe_ratio{0.0};
    double calmar_ratio{0.0};
    double total_commission{0.0};
    
    int total_trades{0};
    int winning_trades{0};
    int losing_trades{0};
    double win_rate_pct{0.0};
    double profit_factor{0.0};
    double total_profit{0.0};
    double total_loss{0.0};
};

class PerformanceAnalyzer {
public:
    static PerformanceStats calculate(
        double initial_capital,
        const std::vector<double>& equity_history,
        const std::vector<TradeData>& trades,
        int total_bars,
        double risk_free_rate = 0.02,
        int bars_per_year = 57600
    );

    static void print_report(const PerformanceStats& stats);
    static void print_ascii_equity_chart(const std::vector<double>& equity_history, int width = 50, int height = 10);
};

} // namespace kun
