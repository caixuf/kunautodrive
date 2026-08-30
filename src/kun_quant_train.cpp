/**
 * @file kun_quant_train.cpp
 * @brief 鲲量化历史数据训练 CLI — 真实历史日线上执行 Walk-Forward 滚动验证与回测
 *
 * 用法:
 *   kun_quant_train [csv ...] [--slices 5] [--is-ratio 0.70] [--capital 1000000]
 *   默认加载 data/history/*.csv (由 tools/kunquant/fetch_history.py 抓取的真实日线)
 *
 * 流程 (全部基于真实历史数据, 严禁虚构):
 *   1. WalkForwardEngine 滚动 IS/OOS 切片优化 (拒绝全样本过拟合)
 *   2. 参数平原稳健性检验
 *   3. 以样本内最优参数做全样本回测, 输出真实绩效报告与净值曲线
 */
#include "kun/core/types.hpp"
#include "kun/backtest/backtest_engine.hpp"
#include "kun/strategy/dual_ma_strategy.hpp"
#include "kun/strategy/walk_forward_engine.hpp"
#include "kun/backtest/performance.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<kun::BarData> load_bars_csv(const std::string& path, const std::string& symbol) {
    std::vector<kun::BarData> bars;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Train] 无法打开 " << path << "\n";
        return bars;
    }
    std::string line;
    std::getline(file, line); // 表头
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> cols;
        while (std::getline(ss, token, ',')) cols.push_back(token);
        if (cols.size() < 6) continue;
        kun::BarData b;
        b.symbol = symbol;
        b.interval = "1d";
        b.datetime_str = cols[1];
        b.open_price = std::stod(cols[2]);
        b.high_price = std::stod(cols[3]);
        b.low_price = std::stod(cols[4]);
        b.close_price = std::stod(cols[5]);
        b.volume = std::stod(cols[6]);
        bars.push_back(b);
    }
    return bars;
}

} // namespace

int main(int argc, char* argv[]) {
    int slices = 5;
    double is_ratio = 0.70;
    double capital = 1000000.0;
    std::vector<std::string> csvs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--slices" && i + 1 < argc) slices = std::stoi(argv[++i]);
        else if (a == "--is-ratio" && i + 1 < argc) is_ratio = std::stod(argv[++i]);
        else if (a == "--capital" && i + 1 < argc) capital = std::stod(argv[++i]);
        else csvs.push_back(a);
    }
    if (csvs.empty()) {
        namespace fs = std::filesystem;
        for (const auto& entry : fs::directory_iterator("data/history")) {
            if (entry.path().extension() == ".csv") csvs.push_back(entry.path().string());
        }
        std::sort(csvs.begin(), csvs.end());
    }
    if (csvs.empty()) {
        std::cerr << "[Train] 无训练数据。请先运行: python3 tools/kunquant/fetch_history.py\n";
        return 1;
    }

    std::cout << "\n=========================================================\n";
    std::cout << "   鲲量化 (KunQuant) 历史数据训练 (Walk-Forward 真实验证)   \n";
    std::cout << "=========================================================\n\n";

    for (const auto& csv : csvs) {
        std::string symbol = std::filesystem::path(csv).stem().string();
        auto bars = load_bars_csv(csv, symbol);
        if (bars.size() < 60) {
            std::cerr << "[Train] " << symbol << " 数据不足 (" << bars.size() << " 根), 跳过\n";
            continue;
        }
        std::cout << "── " << symbol << " ── 真实日线 " << bars.size() << " 根 ("
                  << bars.front().datetime_str << " ~ " << bars.back().datetime_str << ")\n";

        // 1. Walk-Forward 滚动样本外验证
        auto report = kun::WalkForwardEngine::run_walk_forward(bars, slices, is_ratio);
        std::cout << report.summary() << "\n";
        for (const auto& s : report.slices) {
            std::cout << "  切片 " << s.slice_index
                      << " | IS  sharpe=" << s.is_sharpe << " pnl=" << s.is_pnl
                      << " | OOS sharpe=" << s.oos_sharpe << " pnl=" << s.oos_pnl
                      << " | 最优 MA(" << s.best_fast_window << "," << s.best_slow_window << ")\n";
        }
        std::cout << "  参数平原稳健性: " << (report.is_parameter_plateau ? "通过 (邻域参数绩效方差可接受)" : "不通过 (单点尖刺, 存在过拟合风险)") << "\n";

        // 2. 全样本真实回测 (样本内首个最优参数)
        int best_fast = report.slices.empty() ? 5 : report.slices.front().best_fast_window;
        int best_slow = report.slices.empty() ? 20 : report.slices.front().best_slow_window;

        kun::BacktestConfig cfg;
        cfg.initial_capital = capital;
        cfg.slippage_points = 1.0;
        cfg.commission_ratio = 0.0001;
        cfg.symbol_info = kun::SymbolInfo{symbol, "SHFE", 10, 1.0, 0.10, 0.0001};

        kun::BacktestEngine engine(cfg);
        engine.load_csv_data(csv);
        engine.set_strategy(std::make_shared<kun::DualMaStrategy>(
            "DualMA_Train_" + symbol, symbol, best_fast, best_slow, 1.0));
        auto stats = engine.run();

        std::cout << "\n  [全样本回测] DualMA(" << best_fast << "," << best_slow << ") 真实绩效:\n";
        kun::PerformanceAnalyzer::print_report(stats);
        kun::PerformanceAnalyzer::print_ascii_equity_chart(engine.get_equity_history(), 60, 10);
        std::cout << "\n";
    }

    std::cout << "=========================================================\n";
    std::cout << "  训练完成 — 以上全部指标均来自真实历史数据回放\n";
    std::cout << "=========================================================\n";
    return 0;
}
