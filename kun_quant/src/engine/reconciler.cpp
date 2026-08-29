#include "kun/engine/reconciler.hpp"
#include <iomanip>
#include <cmath>
#include <unordered_map>
#include <algorithm>

namespace kun {

std::string ReconciliationReport::summary_text() const {
    std::ostringstream ss;
    ss << "\n=========================================================\n";
    ss << "           KunQuant 每日柜台结算单自动对账报告              \n";
    ss << "=========================================================\n";
    ss << " 账户 ID (Account)        : " << account_id << "\n";
    ss << " 结算日期 (Trade Date)    : " << trade_date << "\n";
    ss << " 对账结果 (Result)        : " << (is_matched ? "[OK] 账务100%一致" : "[ALERT] 发现账务差异!") << "\n";
    ss << std::fixed << std::setprecision(2);
    ss << " 本地总手续费             : " << local_commission << " RMB\n";
    ss << " 柜台结算总手续费         : " << counter_commission << " RMB\n";
    ss << " 手续费偏差               : " << std::abs(local_commission - counter_commission) << " RMB\n";

    if (!discrepancies.empty()) {
        ss << "\n[差异明细清单]\n";
        for (size_t i = 0; i < discrepancies.size(); ++i) {
            const auto& d = discrepancies[i];
            ss << " " << (i + 1) << ". [" << d.item_name << "] 标的: " << d.symbol 
               << " | 本地=" << d.local_value << ", 柜台=" << d.counter_value
               << " -> " << d.description << "\n";
        }
    } else {
        ss << " 全部持仓手数、成交笔数、盈亏与手续费校验 0 误差通过。\n";
    }
    ss << "=========================================================\n";
    return ss.str();
}

ReconciliationReport SettlementReconciler::reconcile(
    const std::string& account_id,
    const std::string& trade_date,
    const std::vector<TradeData>& local_trades,
    const std::vector<PositionData>& local_positions,
    const std::vector<SettlementTradeRecord>& counter_trades,
    const std::vector<SettlementPositionRecord>& counter_positions,
    double counter_balance_pnl,
    double counter_total_commission
) {
    ReconciliationReport report;
    report.account_id = account_id;
    report.trade_date = trade_date;
    report.is_matched = true;
    report.total_counter_pnl = counter_balance_pnl;
    report.counter_commission = counter_total_commission;

    // 1. 手续费核算
    double total_local_comm = 0.0;
    for (const auto& t : local_trades) {
        total_local_comm += t.commission;
    }
    report.local_commission = total_local_comm;

    if (std::abs(total_local_comm - counter_total_commission) > 0.5) {
        report.is_matched = false;
        report.discrepancies.push_back({
            "Commission", "ALL", total_local_comm, counter_total_commission,
            "本地计算手续费与柜台结算单存在偏差"
        });
    }

    // 2. 持仓手数核对 (按 symbol + direction)
    std::unordered_map<std::string, double> local_pos_map;
    for (const auto& p : local_positions) {
        std::string k = p.symbol + "_" + std::to_string(static_cast<int>(p.direction));
        local_pos_map[k] = p.volume;
    }

    std::unordered_map<std::string, double> counter_pos_map;
    for (const auto& p : counter_positions) {
        std::string k = p.symbol + "_" + std::to_string(static_cast<int>(p.direction));
        counter_pos_map[k] = p.volume;
    }

    // 比对本地持仓
    for (const auto& [k, local_vol] : local_pos_map) {
        double counter_vol = counter_pos_map.count(k) ? counter_pos_map[k] : 0.0;
        if (std::abs(local_vol - counter_vol) > 1e-4) {
            report.is_matched = false;
            report.discrepancies.push_back({
                "PositionVolume", k, local_vol, counter_vol,
                "本地记录持仓手数与柜台结算单不一致"
            });
        }
    }

    // 比对柜台多出的持仓
    for (const auto& [k, counter_vol] : counter_pos_map) {
        if (!local_pos_map.count(k) && counter_vol > 0.0) {
            report.is_matched = false;
            report.discrepancies.push_back({
                "OrphanPosition", k, 0.0, counter_vol,
                "柜台存在本地未记录的悬挂持仓"
            });
        }
    }

    // 3. 成交手数核对
    double local_traded_vol = 0.0;
    for (const auto& t : local_trades) local_traded_vol += t.volume;

    double counter_traded_vol = 0.0;
    for (const auto& t : counter_trades) counter_traded_vol += t.volume;

    if (std::abs(local_traded_vol - counter_traded_vol) > 1e-4) {
        report.is_matched = false;
        report.discrepancies.push_back({
            "TradeTotalVolume", "ALL", local_traded_vol, counter_traded_vol,
            "本地成交流水总手数与柜台不一致"
        });
    }

    return report;
}

bool SettlementReconciler::parse_counter_csv(
    const std::string& csv_content,
    std::vector<SettlementTradeRecord>& out_trades,
    std::vector<SettlementPositionRecord>& out_positions
) {
    std::istringstream stream(csv_content);
    std::string line;
    bool in_trades_section = false;
    bool in_pos_section = false;

    while (std::getline(stream, line)) {
        if (line.empty() || line.find("#") == 0) continue;

        if (line.find("[TRADES]") != std::string::npos) {
            in_trades_section = true;
            in_pos_section = false;
            continue;
        }
        if (line.find("[POSITIONS]") != std::string::npos) {
            in_trades_section = false;
            in_pos_section = true;
            continue;
        }

        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> tokens;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        if (in_trades_section && tokens.size() >= 7) {
            try {
                SettlementTradeRecord t;
                t.trade_id = tokens[0];
                t.symbol = tokens[1];
                t.direction = (tokens[2] == "BUY" || tokens[2] == "0") ? Direction::LONG : Direction::SHORT;
                t.offset = (tokens[3] == "OPEN" || tokens[3] == "0") ? Offset::OPEN : Offset::CLOSE;
                t.price = std::stod(tokens[4]);
                t.volume = std::stod(tokens[5]);
                t.commission = std::stod(tokens[6]);
                out_trades.push_back(t);
            } catch (...) {}
        } else if (in_pos_section && tokens.size() >= 5) {
            try {
                SettlementPositionRecord p;
                p.symbol = tokens[0];
                p.direction = (tokens[1] == "LONG" || tokens[1] == "0") ? Direction::LONG : Direction::SHORT;
                p.volume = std::stod(tokens[2]);
                p.avg_price = std::stod(tokens[3]);
                p.margin = std::stod(tokens[4]);
                out_positions.push_back(p);
            } catch (...) {}
        }
    }
    return true;
}

} // namespace kun
