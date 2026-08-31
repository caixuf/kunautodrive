#pragma once

#include "kun/core/types.hpp"
#include "kun/engine/storage_manager.hpp"
#include <string>
#include <vector>
#include <sstream>

namespace kun {

struct SettlementTradeRecord {
    std::string trade_id;
    std::string symbol;
    Direction direction;
    Offset offset;
    double price{0.0};
    double volume{0.0};
    double commission{0.0};
};

struct SettlementPositionRecord {
    std::string symbol;
    Direction direction;
    double volume{0.0};
    double avg_price{0.0};
    double margin{0.0};
};

struct ReconciliationDiscrepancy {
    std::string item_name;
    std::string symbol;
    double local_value{0.0};
    double counter_value{0.0};
    std::string description;
};

struct ReconciliationReport {
    std::string account_id;
    std::string trade_date;
    bool is_matched{true};
    double total_local_pnl{0.0};
    double total_counter_pnl{0.0};
    double local_commission{0.0};
    double counter_commission{0.0};
    std::vector<ReconciliationDiscrepancy> discrepancies;

    std::string summary_text() const;
};

/**
 * @brief 每日柜台结算单自动对账工具 (SettlementReconciler)
 * 实盘生命线：自动比对本地 SQLite 成交流水/持仓 vs 期货柜台结算单
 */
class SettlementReconciler {
public:
    static ReconciliationReport reconcile(
        const std::string& account_id,
        const std::string& trade_date,
        const std::vector<TradeData>& local_trades,
        const std::vector<PositionData>& local_positions,
        const std::vector<SettlementTradeRecord>& counter_trades,
        const std::vector<SettlementPositionRecord>& counter_positions,
        double counter_balance_pnl,
        double counter_total_commission
    );

    // 解析标准 CSV 格式结算单
    static bool parse_counter_csv(
        const std::string& csv_content,
        std::vector<SettlementTradeRecord>& out_trades,
        std::vector<SettlementPositionRecord>& out_positions
    );

    // 严格会计资金恒等式穿透检验: CurrentBalance == InitialBalance + Sum(RealizedPnL) - Sum(Commission)
    static bool validate_balance_invariant(
        double initial_balance,
        double current_balance,
        const std::vector<TradeData>& trades,
        double& out_calc_balance,
        double& out_diff
    );
};

} // namespace kun
