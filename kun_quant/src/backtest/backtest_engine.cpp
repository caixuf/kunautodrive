#include "kun/backtest/backtest_engine.hpp"
#include "kun/core/logger.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace kun {

BacktestEngine::BacktestEngine(BacktestConfig config)
    : config_(config),
      pos_mgr_(config.initial_capital),
      matching_engine_(config.slippage_points, config.commission_ratio),
      risk_mgr_(pos_mgr_) {
    
    pos_mgr_.set_symbol_info(config_.symbol_info);

    // 绑定撮合回调至持仓与策略
    matching_engine_.set_callbacks(
        [this](const OrderData& order) {
            if (strategy_) {
                strategy_->on_order(order);
            }
        },
        [this](const TradeData& trade) {
            pos_mgr_.on_trade(trade);
            if (strategy_) {
                strategy_->on_trade(trade);
            }
        }
    );
}

void BacktestEngine::set_strategy(std::shared_ptr<IStrategy> strategy) {
    strategy_ = strategy;
    strategy_->set_context(this);
}

void BacktestEngine::set_history_bars(std::vector<BarData> bars) {
    history_bars_ = std::move(bars);
}

bool BacktestEngine::load_csv_data(const std::string& csv_filepath) {
    std::ifstream file(csv_filepath);
    if (!file.is_open()) {
        KUN_LOG_ERROR("BacktestEngine", "Failed to open CSV data file: " + csv_filepath);
        return false;
    }

    history_bars_.clear();
    std::string line;
    // 读取表头
    if (!std::getline(file, line)) {
        return false;
    }

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> cols;

        while (std::getline(ss, token, ',')) {
            cols.push_back(token);
        }

        // 支持格式: datetime, open, high, low, close, volume (或带有 symbol)
        if (cols.size() >= 6) {
            BarData bar;
            size_t idx = 0;
            if (cols.size() >= 7 && !isdigit(cols[0][0])) {
                bar.symbol = cols[idx++];
            } else {
                bar.symbol = config_.symbol_info.symbol;
            }
            bar.datetime_str = cols[idx++];
            bar.open_price = std::stod(cols[idx++]);
            bar.high_price = std::stod(cols[idx++]);
            bar.low_price = std::stod(cols[idx++]);
            bar.close_price = std::stod(cols[idx++]);
            bar.volume = std::stod(cols[idx++]);
            if (idx < cols.size()) {
                bar.open_interest = std::stod(cols[idx++]);
            }
            bar.exchange = config_.symbol_info.exchange;
            history_bars_.push_back(bar);
        }
    }

    KUN_LOG_INFO("BacktestEngine", "Loaded " + std::to_string(history_bars_.size()) + " bars from " + csv_filepath);
    return !history_bars_.empty();
}

PerformanceStats BacktestEngine::run() {
    if (!strategy_) {
        KUN_LOG_ERROR("BacktestEngine", "No strategy assigned for backtest!");
        return {};
    }
    if (history_bars_.empty()) {
        KUN_LOG_ERROR("BacktestEngine", "No historical bar data to backtest!");
        return {};
    }

    KUN_LOG_INFO("BacktestEngine", "Starting backtest with " + std::to_string(history_bars_.size()) + " bars...");
    strategy_->on_init();
    strategy_->on_start();

    equity_history_.clear();
    equity_history_.reserve(history_bars_.size());
    decision_log_.clear();

    for (size_t i = 0; i < history_bars_.size(); ++i) {
        const auto& bar = history_bars_[i];
        current_bar_ = bar;
        current_bar_index_ = static_cast<int>(i);

        // 1. 先使用当前 Bar 的开盘价模拟撮合上一根 Bar 产生未成交的挂单
        matching_engine_.match_bar(bar);

        // 2. 更新持仓盯市盈亏
        pos_mgr_.on_bar(bar);

        // 3. 触发策略逻辑
        strategy_->on_bar(bar);

        // 4. 记录当前权益
        auto account = pos_mgr_.get_account();
        equity_history_.push_back(account.dynamic_equity());
    }

    strategy_->on_stop();
    KUN_LOG_INFO("BacktestEngine", "Backtest finished successfully.");

    PerformanceStats stats = PerformanceAnalyzer::calculate(
        config_.initial_capital,
        equity_history_,
        matching_engine_.get_all_trades(),
        static_cast<int>(history_bars_.size())
    );

    return stats;
}

uint64_t BacktestEngine::send_order(const OrderRequest& req) {
    // 决策日志: 在风控检查前记录 (含被风控拒绝的决策), 供未来函数探测比对
    decision_log_.emplace_back(current_bar_index_, req);

    auto active_orders = matching_engine_.get_active_orders();
    auto [passed, reason] = risk_mgr_.check_order(req, active_orders);
    if (!passed) {
        KUN_LOG_WARN("RiskManager", "Order rejected by risk control: " + reason);
        return 0;
    }
    return matching_engine_.submit_order(req);
}

bool BacktestEngine::cancel_order(uint64_t order_id) {
    return matching_engine_.cancel_order(order_id);
}

PositionData BacktestEngine::get_position(const std::string& symbol, Direction direction) const {
    return pos_mgr_.get_position(symbol, direction);
}

AccountData BacktestEngine::get_account() const {
    return pos_mgr_.get_account();
}

const TickData* BacktestEngine::get_latest_tick(const std::string& symbol) const {
    return nullptr;
}

} // namespace kun
