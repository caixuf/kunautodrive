#pragma once

#include "kun/cellular/cellular_genome.hpp"
#include "kun/core/types.hpp"

namespace kun {

/**
 * @brief 量化金融细胞演化交易适配器 (QuantCellularAdapter)
 * 将高频市场 Tick 行情转化为感知受体电位，将动作细胞电位映射为交易信号与风控指令
 */
class QuantCellularAdapter {
public:
    explicit QuantCellularAdapter(CellularOrganism organism)
        : organism_(std::move(organism)) {}

    struct TradeDecision {
        enum class Action { HOLD = 0, BUY_OPEN = 1, SELL_OPEN = 2, CLOSE_ALL = 3, RISK_LOCKED = 4 };
        Action action{Action::HOLD};
        double target_price{0.0};
        double confidence{0.0};
        std::string explanation;
    };

    TradeDecision process_tick(const QuantTickMsg& tick) {
        // 提取 4 大基础感知信号 (标准化缩放)
        double inputs[4];
        inputs[0] = tick.last_price; // 价格
        inputs[1] = tick.volume;     // 成交量
        inputs[2] = tick.ask_price1 - tick.bid_price1; // 盘口价差
        inputs[3] = (tick.bid_volume1 - tick.ask_volume1) / 
                    (std::abs(tick.bid_volume1 + tick.ask_volume1) > 1e-4 ? (tick.bid_volume1 + tick.ask_volume1) : 1.0); // 盘口不平衡度

        // 前向传导激发整个多细胞神经网络
        auto outputs = organism_.forward(inputs);

        TradeDecision decision;
        decision.target_price = tick.last_price;

        if (outputs.immune_lock) {
            decision.action = TradeDecision::Action::RISK_LOCKED;
            decision.explanation = "[CellularImmuneLock] 细胞网络自发触发免疫抑制刹车!";
            return decision;
        }

        if (outputs.defensive_reset > 0.5) {
            decision.action = TradeDecision::Action::CLOSE_ALL;
            decision.explanation = "[CellularClose] 细胞网络触发防御性清仓信号";
            return decision;
        }

        if (outputs.positive_action > 0.5) {
            decision.action = TradeDecision::Action::BUY_OPEN;
            decision.confidence = outputs.positive_action;
            decision.explanation = "[CellularBuy] 细胞网络产生正向多头兴奋脉冲";
        } else if (outputs.negative_action > 0.5 || outputs.positive_action < -0.5) {
            decision.action = TradeDecision::Action::SELL_OPEN;
            decision.confidence = std::abs(outputs.negative_action);
            decision.explanation = "[CellularSell] 细胞网络产生空头兴奋脉冲";
        }

        return decision;
    }

    const CellularOrganism& get_organism() const { return organism_; }
    CellularOrganism& get_organism() { return organism_; }

private:
    CellularOrganism organism_;
};

} // namespace kun
