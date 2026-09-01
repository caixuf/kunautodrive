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
        // 提取 4 大基础微观感知信号 (标准化与无量纲化)
        double inputs[4];
        inputs[0] = (prev_price_ > 0.0) ? std::clamp((tick.last_price - prev_price_) / prev_price_ * 200.0, -1.0, 1.0) : 0.0;
        inputs[1] = (prev_vol_ > 0.0) ? std::clamp((tick.volume - prev_vol_) / prev_vol_, -1.0, 1.0) : 0.0;
        inputs[2] = std::clamp((tick.ask_price1 - tick.bid_price1) / (tick.last_price > 0 ? tick.last_price * 0.001 : 1.0), -1.0, 1.0);
        double total_bid_ask_vol = std::abs(tick.bid_volume1 + tick.ask_volume1);
        inputs[3] = (total_bid_ask_vol > 1e-4) ? std::clamp((tick.bid_volume1 - tick.ask_volume1) / total_bid_ask_vol, -1.0, 1.0) : 0.0;

        prev_price_ = tick.last_price;
        prev_vol_ = tick.volume;

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
    double prev_price_{0.0};
    double prev_vol_{0.0};
};

} // namespace kun
