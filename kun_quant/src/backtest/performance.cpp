#include "kun/backtest/performance.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <unordered_map>

namespace kun {

PerformanceStats PerformanceAnalyzer::calculate(
    double initial_capital,
    const std::vector<double>& equity_history,
    const std::vector<TradeData>& trades,
    int total_bars,
    double risk_free_rate,
    int bars_per_year
) {
    PerformanceStats stats;
    stats.initial_capital = initial_capital;
    if (equity_history.empty()) return stats;

    stats.final_equity = equity_history.back();
    stats.total_return_pct = ((stats.final_equity - initial_capital) / initial_capital) * 100.0;

    double years = std::max(0.001, static_cast<double>(total_bars) / static_cast<double>(bars_per_year));
    stats.annualized_return_pct = (std::pow(std::max(0.001, stats.final_equity / initial_capital), 1.0 / years) - 1.0) * 100.0;

    // 最大回撤计算 (Max Drawdown)
    double peak = equity_history.front();
    double max_dd = 0.0;
    double max_dd_pct = 0.0;

    std::vector<double> returns;
    returns.reserve(equity_history.size());

    for (size_t i = 1; i < equity_history.size(); ++i) {
        if (equity_history[i] > peak) {
            peak = equity_history[i];
        }
        double dd = peak - equity_history[i];
        double dd_pct = (peak > 0) ? (dd / peak) * 100.0 : 0.0;
        if (dd > max_dd) max_dd = dd;
        if (dd_pct > max_dd_pct) max_dd_pct = dd_pct;

        if (equity_history[i - 1] > 0) {
            returns.push_back((equity_history[i] - equity_history[i - 1]) / equity_history[i - 1]);
        }
    }
    stats.max_drawdown = max_dd;
    stats.max_drawdown_pct = max_dd_pct;

    // 夏普比率 (Sharpe Ratio)
    if (!returns.empty()) {
        double sum_ret = std::accumulate(returns.begin(), returns.end(), 0.0);
        double mean_ret = sum_ret / returns.size();
        double sq_sum = 0.0;
        for (double r : returns) {
            sq_sum += (r - mean_ret) * (r - mean_ret);
        }
        double std_dev = std::sqrt(sq_sum / returns.size());
        if (std_dev > 1e-8) {
            double annual_std = std_dev * std::sqrt(static_cast<double>(bars_per_year));
            stats.sharpe_ratio = (stats.annualized_return_pct / 100.0 - risk_free_rate) / annual_std;
        }
    }

    // 卡玛比率 (Calmar Ratio)
    if (stats.max_drawdown_pct > 0.001) {
        stats.calmar_ratio = stats.annualized_return_pct / stats.max_drawdown_pct;
    }

    // 严谨的开平仓对冲匹配 (Round-trip Trade Pairing)
    struct OpenPos {
        Direction dir;
        double price;
        double volume;
    };
    std::unordered_map<std::string, std::vector<OpenPos>> open_queues;

    for (const auto& t : trades) {
        stats.total_commission += t.commission;
        int mult = 10;

        if (t.offset == Offset::OPEN) {
            open_queues[t.symbol].push_back({t.direction, t.price, t.volume});
        } else {
            // 平仓匹配
            auto& q = open_queues[t.symbol];
            double to_close = t.volume;
            while (to_close > 0.0 && !q.empty()) {
                auto& open_pos = q.front();
                double match_vol = std::min(open_pos.volume, to_close);

                double pnl = 0.0;
                if (open_pos.dir == Direction::LONG) {
                    pnl = (t.price - open_pos.price) * match_vol * mult;
                } else {
                    pnl = (open_pos.price - t.price) * match_vol * mult;
                }

                if (pnl > 0.0) {
                    stats.winning_trades++;
                    stats.total_profit += pnl;
                } else {
                    stats.losing_trades++;
                    stats.total_loss += std::abs(pnl);
                }

                open_pos.volume -= match_vol;
                to_close -= match_vol;
                if (open_pos.volume <= 0.0001) {
                    q.erase(q.begin());
                }
            }
        }
    }

    int closed_trips = stats.winning_trades + stats.losing_trades;
    stats.total_trades = closed_trips > 0 ? closed_trips : static_cast<int>(trades.size());
    if (closed_trips > 0) {
        stats.win_rate_pct = (static_cast<double>(stats.winning_trades) / closed_trips) * 100.0;
        stats.profit_factor = (stats.total_loss > 0.0) ? (stats.total_profit / stats.total_loss) : (stats.total_profit > 0 ? 99.9 : 0.0);
    }

    return stats;
}

void PerformanceAnalyzer::print_report(const PerformanceStats& stats) {
    std::cout << "\n=========================================================\n";
    std::cout << "               鲲量化 (KunQuant) 回测绩效报告               \n";
    std::cout << "=========================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << " 初始资金 (Initial Capital)     : " << std::setw(12) << stats.initial_capital << " RMB\n";
    std::cout << " 最终权益 (Final Equity)        : " << std::setw(12) << stats.final_equity << " RMB\n";
    std::cout << " 累计收益率 (Total Return)      : " << std::setw(12) << stats.total_return_pct << " %\n";
    std::cout << " 年化收益率 (Annualized Return) : " << std::setw(12) << stats.annualized_return_pct << " %\n";
    std::cout << " 最大回撤 (Max Drawdown)        : " << std::setw(12) << stats.max_drawdown << " RMB (" << stats.max_drawdown_pct << "%)\n";
    std::cout << " 夏普比率 (Sharpe Ratio)        : " << std::setw(12) << stats.sharpe_ratio << "\n";
    std::cout << " 卡玛比率 (Calmar Ratio)        : " << std::setw(12) << stats.calmar_ratio << "\n";
    std::cout << " 胜率 (Win Rate)                : " << std::setw(12) << stats.win_rate_pct << " % (" << stats.winning_trades << " 胜 / " << stats.losing_trades << " 负)\n";
    std::cout << " 盈亏比 (Profit Factor)         : " << std::setw(12) << stats.profit_factor << "\n";
    std::cout << " 总手续费 (Total Commission)    : " << std::setw(12) << stats.total_commission << " RMB\n";
    std::cout << " 平仓回合数 (Round-trip Trades) : " << std::setw(12) << stats.total_trades << "\n";
    std::cout << "=========================================================\n";
}

void PerformanceAnalyzer::print_ascii_equity_chart(const std::vector<double>& equity_history, int width, int height) {
    if (equity_history.empty()) return;

    double min_eq = *std::min_element(equity_history.begin(), equity_history.end());
    double max_eq = *std::max_element(equity_history.begin(), equity_history.end());
    if (std::abs(max_eq - min_eq) < 1e-5) max_eq += 1.0;

    std::cout << "\n[净值曲线走势图]\n";
    std::vector<std::string> grid(height, std::string(width, ' '));

    for (int col = 0; col < width; ++col) {
        size_t idx = col * (equity_history.size() - 1) / std::max(1, width - 1);
        double val = equity_history[idx];
        int row = static_cast<int>((max_eq - val) / (max_eq - min_eq) * (height - 1));
        row = std::clamp(row, 0, height - 1);
        grid[row][col] = '*';
    }

    for (int r = 0; r < height; ++r) {
        double level = max_eq - (r * (max_eq - min_eq) / (height - 1));
        std::cout << std::setw(10) << std::fixed << std::setprecision(1) << level << " | " << grid[r] << "\n";
    }
    std::cout << "           +" << std::string(width, '-') << "\n";
}

} // namespace kun
