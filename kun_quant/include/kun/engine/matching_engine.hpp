#pragma once

#include "kun/core/types.hpp"
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>

namespace kun {

using OrderCallback = std::function<void(const OrderData&)>;
using TradeCallback = std::function<void(const TradeData&)>;

/**
 * @brief 高性能内存撮合引擎 (MatchingEngine)
 * 用于仿真交易与超高速历史回测，精确模拟滑点、手续费与订单排队状态
 */
class MatchingEngine {
public:
    MatchingEngine(double slippage_points = 0.0, double commission_ratio = 0.0001);

    void set_callbacks(OrderCallback on_order, TradeCallback on_trade);
    void set_slippage(double slippage_points) { slippage_points_ = slippage_points; }
    void set_commission_ratio(double ratio) { commission_ratio_ = ratio; }
    void set_symbol_multiplier(const std::string& symbol, double multiplier) {
        std::lock_guard<std::mutex> lock(mutex_);
        multipliers_[symbol] = multiplier;
    }
    double get_multiplier(const std::string& symbol) const {
        auto it = multipliers_.find(symbol);
        return (it != multipliers_.end()) ? it->second : 10.0;
    }

    // 接收订单请求
    uint64_t submit_order(const OrderRequest& req);

    // 撤单请求
    bool cancel_order(uint64_t order_id);

    // 行情驱动撮合
    void match_tick(const TickData& tick);
    void match_bar(const BarData& bar);

    std::vector<OrderData> get_active_orders() const;
    const std::vector<OrderData>& get_all_orders() const { return all_orders_; }
    const std::vector<TradeData>& get_all_trades() const { return all_trades_; }

private:
    void execute_fill(OrderData& order, double fill_price, double fill_volume, int64_t timestamp_us, const std::string& datetime_str);

    mutable std::mutex mutex_;
    std::atomic<uint64_t> next_order_id_{10001};
    std::atomic<uint64_t> next_trade_id_{50001};

    double slippage_points_{0.0};
    double commission_ratio_{0.0001};

    OrderCallback on_order_cb_;
    TradeCallback on_trade_cb_;

    std::vector<OrderData> active_orders_;
    std::vector<OrderData> all_orders_;
    std::vector<TradeData> all_trades_;
    std::unordered_map<std::string, double> multipliers_;
};

} // namespace kun
