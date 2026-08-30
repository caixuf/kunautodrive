#pragma once

#include "kun/core/types.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>

namespace kun {

struct SymbolInfo {
    std::string symbol;
    std::string exchange;
    int multiplier{10};           // 合约乘数 (如螺纹钢10吨/手, 沪深300为300元/点)
    double price_tick{1.0};       // 最小变动价位
    double margin_ratio{0.10};    // 保证金比例 10%
    double commission_ratio{0.0001}; // 手续费率 (万一)
    std::string sina_code;        // 新浪行情代码 (策略覆盖项, 如 "nf_AU0"; 空则用通用规则推导)
};

/**
 * @brief 持仓与资产管理器 (PositionManager)
 * 严格支持国内期货多空双向持仓、今昨仓分离、逐日/盯市盈亏与保证金计算
 */
class PositionManager {
public:
    PositionManager(double initial_balance = 1000000.0);

    void set_symbol_info(const SymbolInfo& info);
    const SymbolInfo* get_symbol_info(const std::string& symbol) const;

    // 处理成交回报，更新持仓
    void on_trade(const TradeData& trade);

    // 接收最新行情，计算盯市浮动盈亏
    void on_tick(const TickData& tick);
    void on_bar(const BarData& bar);

    // 查询多头/空头持仓
    PositionData get_position(const std::string& symbol, Direction direction) const;
    std::vector<PositionData> get_all_positions() const;

    // 查询账户状态
    AccountData get_account() const;

    // 冻结资金/解冻资金 (报单/撤单时使用)
    bool freeze_margin(double amount);
    void unfreeze_margin(double amount);

    // 冻结持仓手数/解冻持仓手数 (平仓挂单/撤单时使用)
    bool freeze_position(const std::string& symbol, Direction direction, double volume);
    void unfreeze_position(const std::string& symbol, Direction direction, double volume);

    /**
     * @brief 上期所/能源中心 (SHFE/INE) 昨今仓平仓决策器 (借鉴 vn.py 工业级算法)
     * 自动拆分并优先平昨仓以极小化手续费，返回平仓拆单指令序列 [(Offset, Volume)]
     */
    std::vector<std::pair<Offset, double>> resolve_close_orders(
        const std::string& symbol, const std::string& exchange,
        Direction close_direction, double req_volume) const;

private:
    void update_pnl(const std::string& symbol, double current_price);

    mutable std::mutex mutex_;
    AccountData account_;
    std::unordered_map<std::string, SymbolInfo> symbol_infos_;
    
    // key: symbol + "_" + Direction (e.g. "rb2405_LONG", "rb2405_SHORT")
    std::unordered_map<std::string, PositionData> positions_;
    std::unordered_map<std::string, double> latest_prices_;
};

} // namespace kun
