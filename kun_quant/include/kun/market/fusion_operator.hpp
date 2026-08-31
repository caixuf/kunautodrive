#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>

namespace kun {

struct MarketObservation {
    std::string source_name;
    QuantTickMsg tick;
    uint64_t timestamp_us{0};
    double weight{1.0};
    bool is_outlier{false};
};

/**
 * @brief 生产级多源行情加权融合与刺针过滤通用算子 (MarketFusionOperator)
 * 遵循 DRY 原则：作为唯一真理源，被 MarketFusionNode 与 CoroMarketFusionTask 共享复用。
 */
class MarketFusionOperator {
public:
    explicit MarketFusionOperator(double outlier_pct_threshold = 0.03)
        : outlier_pct_threshold_(outlier_pct_threshold) {}

    /**
     * @brief 校验价格是否为异常离群刺针，并在非异常时记入中值参考滑窗
     */
    bool check_and_record_price(double price) {
        bool is_outlier = false;
        if (price_window_.size() >= 3) {
            std::vector<double> sorted(price_window_.begin(), price_window_.end());
            std::sort(sorted.begin(), sorted.end());
            double median = sorted[sorted.size() / 2];
            if (median > 1e-6) {
                double dev = std::abs(price - median) / median;
                if (dev > outlier_pct_threshold_) {
                    is_outlier = true;
                    outliers_rejected_++;
                }
            } else {
                // 基准中值异常近零，直接判为异常防止除以零
                is_outlier = true;
                outliers_rejected_++;
            }
        }
        if (!is_outlier) {
            price_window_.push_back(price);
            if (price_window_.size() > 30) price_window_.pop_front();
        }
        return is_outlier;
    }

    /**
     * @brief 多源加权融合计算 (价格加权平均、真实盘口累加、合法價差兜底)
     */
    bool compute_fused_tick(
        const std::string& symbol,
        const std::unordered_map<std::string, MarketObservation>& observations,
        QuantTickMsg& out_fused_tick,
        uint32_t& out_valid_sources,
        double& out_confidence
    ) const {
        double sum_weighted_price = 0.0;
        double sum_weight = 0.0;
        double best_bid = 0.0;
        double best_ask = 9999999.0;
        double best_bid_vol = 0.0;
        double best_ask_vol = 0.0;
        double total_vol = 0.0;
        double total_oi = 0.0;
        uint32_t valid_sources = 0;

        for (const auto& [name, obs] : observations) {
            if (obs.is_outlier) continue;

            sum_weighted_price += obs.tick.last_price * obs.weight;
            sum_weight += obs.weight;
            total_vol = std::max(total_vol, obs.tick.volume);
            total_oi = std::max(total_oi, obs.tick.open_interest);

            if (obs.tick.bid_price1 > best_bid) {
                best_bid = obs.tick.bid_price1;
                best_bid_vol = obs.tick.bid_volume1;
            } else if (best_bid > 0.0 && std::abs(obs.tick.bid_price1 - best_bid) < 1e-4) {
                best_bid_vol += obs.tick.bid_volume1;
            }

            if (obs.tick.ask_price1 > 0.0 && obs.tick.ask_price1 < best_ask) {
                best_ask = obs.tick.ask_price1;
                best_ask_vol = obs.tick.ask_volume1;
            } else if (best_ask < 9999990.0 && std::abs(obs.tick.ask_price1 - best_ask) < 1e-4) {
                best_ask_vol += obs.tick.ask_volume1;
            }

            valid_sources++;
        }

        if (sum_weight <= 0.0 || valid_sources == 0) return false;

        double fused_price = sum_weighted_price / sum_weight;
        // 当源数据完全缺失单边盘口时，保留合法单 tick 价差兜底，防止 crossed-market 拒单
        if (best_ask >= 9999990.0) best_ask = fused_price + 1.0;
        if (best_bid <= 0.0) best_bid = (fused_price > 1.0) ? (fused_price - 1.0) : fused_price;

        // 强校验：严禁产生 crossed-market (best_bid >= best_ask)
        if (best_bid >= best_ask) {
            best_ask = best_bid + 1.0;
        }

        std::memset(&out_fused_tick, 0, sizeof(out_fused_tick));
        std::strncpy(out_fused_tick.symbol, symbol.c_str(), sizeof(out_fused_tick.symbol) - 1);
        std::strncpy(out_fused_tick.exchange, "SHFE", sizeof(out_fused_tick.exchange) - 1);
        out_fused_tick.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        out_fused_tick.last_price = fused_price;
        out_fused_tick.volume = total_vol;
        out_fused_tick.open_interest = total_oi;
        out_fused_tick.bid_price1 = best_bid;
        out_fused_tick.bid_volume1 = best_bid_vol;
        out_fused_tick.ask_price1 = best_ask;
        out_fused_tick.ask_volume1 = best_ask_vol;

        out_valid_sources = valid_sources;
        out_confidence = (valid_sources >= 2) ? 0.95 : 0.50; // 单源时明示仅有基础置信度
        return true;
    }

    uint32_t get_outliers_rejected() const { return outliers_rejected_; }

private:
    double outlier_pct_threshold_{0.03};
    std::deque<double> price_window_;
    uint32_t outliers_rejected_{0};
};

} // namespace kun
