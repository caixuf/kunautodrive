#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

namespace kun {

enum class MarketRegimeState {
    TRENDING_BULL = 0,
    TRENDING_BEAR = 1,
    RANGING = 2,
    UNCERTAIN_TRANSITION = 3 // 模糊过渡态 (自动触发头寸降级与防横跳保护)
};

inline std::string to_string(MarketRegimeState s) {
    switch (s) {
        case MarketRegimeState::TRENDING_BULL:       return "TRENDING_BULL";
        case MarketRegimeState::TRENDING_BEAR:       return "TRENDING_BEAR";
        case MarketRegimeState::RANGING:             return "RANGING";
        case MarketRegimeState::UNCERTAIN_TRANSITION: return "UNCERTAIN_TRANSITION";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 市场形态自适应与滞后滤波检测器 (HysteresisRegimeDetector)
 * 解决状态机临界点频繁误判、来回横跳与过渡态磨损的核心防护机制
 */
class HysteresisRegimeDetector {
public:
    explicit HysteresisRegimeDetector(int confirmation_bars = 3, double transition_lower = 0.45, double transition_upper = 0.65)
        : confirmation_bars_(confirmation_bars),
          transition_lower_(transition_lower),
          transition_upper_(transition_upper) {}

    /**
     * @brief 喂入单根 Bar 的指标特征更新形态
     * @param trend_strength 趋势强度指标 (0.0 ~ 1.0, 如 ADX / 均线斜率归一化)
     * @param slope 均线斜率
     * @param atr 真实波幅
     */
    MarketRegimeState update(double trend_strength, double slope, double atr) {
        (void)atr;
        raw_trend_strength_ = trend_strength;

        MarketRegimeState candidate_state;
        if (trend_strength >= transition_upper_) {
            candidate_state = (slope >= 0.0) ? MarketRegimeState::TRENDING_BULL : MarketRegimeState::TRENDING_BEAR;
        } else if (trend_strength <= transition_lower_) {
            candidate_state = MarketRegimeState::RANGING;
        } else {
            candidate_state = MarketRegimeState::UNCERTAIN_TRANSITION;
        }

        // 滞后滤波带逻辑 (Hysteresis Band)：
        // 若候选状态与当前确认状态不同，趋势/震荡需要连续 confirmation_bars_ 根确认;
        // 模糊过渡态 (UNCERTAIN_TRANSITION) 立即生效 —— 首次偏离即降仓防磨损,
        // 否则趋势↔震荡切换的临界第一根 Bar 仍以满仓硬扛边界噪音。
        if (candidate_state != current_state_) {
            if (candidate_state == MarketRegimeState::UNCERTAIN_TRANSITION) {
                current_state_ = candidate_state;
                pending_state_ = candidate_state;
                pending_count_ = 0;
            } else if (candidate_state == pending_state_) {
                pending_count_++;
                if (pending_count_ >= confirmation_bars_) {
                    current_state_ = candidate_state;
                    pending_count_ = 0;
                }
            } else {
                pending_state_ = candidate_state;
                pending_count_ = 1;
            }
        } else {
            pending_count_ = 0;
        }

        return current_state_;
    }

    MarketRegimeState get_current_regime() const { return current_state_; }

    /**
     * @brief 推荐仓位上限比例 (过渡态自动压降至 30% 头寸，避免硬刚磨损)
     */
    double get_target_position_ratio() const {
        if (current_state_ == MarketRegimeState::UNCERTAIN_TRANSITION) {
            return 0.30; // 模糊过渡态强制降仓至 30%
        } else if (current_state_ == MarketRegimeState::RANGING) {
            return 0.50; // 震荡市半仓
        } else {
            return 1.00; // 明确趋势市满额仓位
        }
    }

    /**
     * @brief 推荐止损容忍度乘数 (过渡态放大 1.5x ATR 避免被噪音扫损)
     */
    double get_stop_loss_multiplier() const {
        if (current_state_ == MarketRegimeState::UNCERTAIN_TRANSITION) {
            return 1.50; // 放大 50% 止损缓冲
        }
        return 1.00;
    }

private:
    int confirmation_bars_{3};
    double transition_lower_{0.45};
    double transition_upper_{0.65};

    MarketRegimeState current_state_{MarketRegimeState::RANGING};
    MarketRegimeState pending_state_{MarketRegimeState::RANGING};
    int pending_count_{0};
    double raw_trend_strength_{0.5};
};

} // namespace kun
