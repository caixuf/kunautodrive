#pragma once

#include "kun/core/types.hpp"
#include "kun/strategy/strategy_base.hpp"
#include "kun/engine/position_manager.hpp"
#include "kun/engine/matching_engine.hpp"
#include "kun/engine/risk_manager.hpp"
#include "kun/backtest/performance.hpp"
#include <string>
#include <vector>
#include <utility>
#include <memory>

namespace kun {

struct BacktestConfig {
    double initial_capital{1000000.0};
    double slippage_points{1.0};          // 1 跳滑点
    double commission_ratio{0.0001};      // 万分之一手续费
    SymbolInfo symbol_info{"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001};
};

class BacktestEngine : public IStrategyContext {
public:
    explicit BacktestEngine(BacktestConfig config = {});

    // 加载历史 K 线数据
    bool load_csv_data(const std::string& csv_filepath);
    void set_history_bars(std::vector<BarData> bars);

    // 设置与加载策略
    void set_strategy(std::shared_ptr<IStrategy> strategy);

    // 执行回测
    PerformanceStats run();

    // IStrategyContext 接口实现
    uint64_t send_order(const OrderRequest& req) override;
    bool cancel_order(uint64_t order_id) override;
    PositionData get_position(const std::string& symbol, Direction direction) const override;
    AccountData get_account() const override;
    const TickData* get_latest_tick(const std::string& symbol) const override;

    const std::vector<double>& get_equity_history() const { return equity_history_; }

    /**
     * @brief 策略决策日志: (决策发生时的 bar 序号, 订单请求)
     * 供 §2-G 数据平移测试 (未来函数探测) 比对使用, 在风控检查前记录
     */
    const std::vector<std::pair<int, OrderRequest>>& get_decision_log() const { return decision_log_; }

private:
    BacktestConfig config_;
    PositionManager pos_mgr_;
    MatchingEngine matching_engine_;
    RiskManager risk_mgr_;

    std::shared_ptr<IStrategy> strategy_;
    std::vector<BarData> history_bars_;
    std::vector<double> equity_history_;

    BarData current_bar_;
    int current_bar_index_{-1};
    std::vector<std::pair<int, OrderRequest>> decision_log_;
};

} // namespace kun
