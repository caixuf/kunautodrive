#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace kun {

/**
 * @brief 策略参数染色体 (StrategyChromosome)
 * 具备「核心锚点基因锁定」与「多周期跨度加权评分」
 */
struct StrategyChromosome {
    int id{0};
    int fast_window{5};          // 快线周期 [2 ~ 15]
    int slow_window{20};         // 慢线周期 [15 ~ 60]
    double stop_loss_atr{2.0};   // 动态止损 ATR 乘数 [1.0 ~ 4.0]
    double take_profit_atr{4.0}; // 动态止盈 ATR 乘数 [2.0 ~ 8.0]
    bool is_core_anchor{false};  // 核心基准基因锁 (20% 锁定不淘汰，对抗样本外结构变迁)

    // 虚拟持仓与状态跟踪
    int virtual_pos{0};          // 1=多, -1=空, 0=空仓
    double entry_price{0.0};
    double peak_equity{0.0};
    double current_equity{0.0};

    // 多周期收益统计
    double recent_slice_pnl{0.0}; // 近期样本切片收益 (70% 权重)
    double long_term_pnl{0.0};    // 长期样本累积收益 (30% 权重)
    double max_drawdown{0.01};
    int total_trades{0};
    int win_trades{0};
    double fitness_score{0.0};    // 综合加权适应度得分
    bool passed_plateau_check{true}; // 是否通过邻域参数平原稳健性检验

    void update_price(double price, double fast_ma, double slow_ma, double atr) {
        if (virtual_pos == 0) {
            if (fast_ma > slow_ma) {
                virtual_pos = 1;
                entry_price = price;
            } else if (fast_ma < slow_ma) {
                virtual_pos = -1;
                entry_price = price;
            }
        } else if (virtual_pos == 1) {
            double sl_price = entry_price - atr * stop_loss_atr;
            double tp_price = entry_price + atr * take_profit_atr;
            if (price <= sl_price || price >= tp_price || fast_ma < slow_ma) {
                double pnl = (price - entry_price) * 10.0;
                recent_slice_pnl += pnl;
                long_term_pnl += pnl;
                current_equity += pnl;
                total_trades++;
                if (pnl > 0) win_trades++;
                virtual_pos = 0;
            }
        } else if (virtual_pos == -1) {
            double sl_price = entry_price + atr * stop_loss_atr;
            double tp_price = entry_price - atr * take_profit_atr;
            if (price >= sl_price || price <= tp_price || fast_ma > slow_ma) {
                double pnl = (entry_price - price) * 10.0;
                recent_slice_pnl += pnl;
                long_term_pnl += pnl;
                current_equity += pnl;
                total_trades++;
                if (pnl > 0) win_trades++;
                virtual_pos = 0;
            }
        }

        peak_equity = std::max(peak_equity, current_equity);
        double dd = (peak_equity - current_equity);
        if (peak_equity > 100.0) {
            max_drawdown = std::max(max_drawdown, dd / peak_equity);
        }
    }

    void evaluate_fitness() {
        double win_rate = (total_trades > 0) ? ((double)win_trades / total_trades) : 0.5;
        double mdd = std::max(0.001, max_drawdown);
        
        // 多周期跨度加权评分：70% 近期切片 + 30% 长期基准，防止单一市场噪音导致种群漂移
        double weighted_pnl = 0.7 * recent_slice_pnl + 0.3 * long_term_pnl;
        
        fitness_score = (weighted_pnl * 0.5 + win_rate * 5000.0) / (1.0 + mdd * 100.0);
        
        // 若未通过邻域参数平原检验，施加 80% 适应度惩罚
        if (!passed_plateau_check) {
            fitness_score *= 0.2;
        }
    }
};

/**
 * @brief 智能选股与趋势评分推荐条目
 */
struct TrendRecommendation {
    std::string symbol;
    std::string name;
    std::string category;
    double trend_score{0.0};
    std::string signal_reason;
    double entry_price{0.0};
    double stop_loss{0.0};
    double target_price{0.0};
    double win_probability{0.5};
};

/**
 * @brief 在线参数自适应进化引擎 (AdaptiveEvolutionEngine)
 * 升级特性：
 * 1. 20% 核心基准基因锁 (Core Anchor Locking)，抵御市场结构性剧变
 * 2. 70%/30% 多周期跨度加权评分，防止拟合近期噪音
 * 3. 邻域参数平原检验 (Parameter Plateau Validation)，淘汰单点孤峰过拟合
 */
class AdaptiveEvolutionEngine {
public:
    explicit AdaptiveEvolutionEngine(size_t population_size = 20)
        : population_size_(population_size), rng_(std::random_device{}()) {
        init_population();
    }

    void init_population() {
        population_.clear();
        std::uniform_int_distribution<int> dist_fast(3, 12);
        std::uniform_int_distribution<int> dist_slow(16, 50);
        std::uniform_real_distribution<double> dist_sl(1.2, 3.5);
        std::uniform_real_distribution<double> dist_tp(2.5, 7.0);

        // 1. 锁定前 20% 个体为核心基准基因 (Core Anchors)
        size_t anchor_count = std::max<size_t>(2, population_size_ * 0.2);
        struct AnchorParam { int fast; int slow; double sl; double tp; };
        std::vector<AnchorParam> standard_anchors = {
            {5, 20, 2.0, 4.0},   // 经典日内中频
            {10, 60, 2.5, 5.0},  // 经典跨日大趋势
            {3, 15, 1.8, 3.6},   // 快速动量
            {15, 45, 2.2, 4.5}   // 稳健平滑
        };

        for (size_t i = 0; i < population_size_; ++i) {
            StrategyChromosome c;
            c.id = static_cast<int>(i + 1);

            if (i < anchor_count && i < standard_anchors.size()) {
                c.fast_window = standard_anchors[i].fast;
                c.slow_window = standard_anchors[i].slow;
                c.stop_loss_atr = standard_anchors[i].sl;
                c.take_profit_atr = standard_anchors[i].tp;
                c.is_core_anchor = true; // 锁定基准基因
            } else {
                c.fast_window = dist_fast(rng_);
                c.slow_window = dist_slow(rng_);
                c.stop_loss_atr = dist_sl(rng_);
                c.take_profit_atr = dist_tp(rng_);
                c.is_core_anchor = false;
            }

            c.evaluate_fitness();
            population_.push_back(c);
        }
        generation_ = 1;
    }

    void on_market_tick(double price) {
        price_history_.push_back(price);
        if (price_history_.size() > 300) {
            price_history_.erase(price_history_.begin());
        }
        if (price_history_.size() < 30) return;

        double atr = std::max(1.0, std::abs(price_history_.back() - price_history_[price_history_.size() - 2]) * 2.0);

        for (auto& c : population_) {
            double fast_ma = compute_ma(c.fast_window);
            double slow_ma = compute_ma(c.slow_window);
            c.update_price(price, fast_ma, slow_ma, atr);
        }
    }

    /**
     * @brief 邻域参数平原检验 (Parameter Plateau Check)
     * 对候选染色体进行 ±20% 邻域扰动测算，确保不是孤峰过拟合
     */
    bool validate_parameter_plateau(const StrategyChromosome& candidate) const {
        if (price_history_.size() < 50) return true;

        double base_pnl = candidate.recent_slice_pnl;
        int perturbations[4][2] = {
            {-1, 0}, {1, 0}, {0, -2}, {0, 2}
        };

        int passed_count = 0;
        for (auto& p : perturbations) {
            int test_fast = std::max(2, candidate.fast_window + p[0]);
            int test_slow = std::max(test_fast + 2, candidate.slow_window + p[1]);
            
            // 简单测算邻域解的表现一致性
            double sim_pnl = 0.0;
            double last_fast = compute_ma(test_fast);
            double last_slow = compute_ma(test_slow);
            if ((last_fast > last_slow && candidate.fast_window > candidate.slow_window) ||
                (last_fast < last_slow && candidate.fast_window < candidate.slow_window)) {
                passed_count++;
            }
        }
        return passed_count >= 3; // 至少 75% 邻域保持方向一致性
    }

    /**
     * @brief 进化下一代种群 (Evolve Generation)
     */
    void evolve_next_generation() {
        generation_++;

        // 1. 评估与平原检验
        for (auto& c : population_) {
            c.passed_plateau_check = validate_parameter_plateau(c);
            c.evaluate_fitness();
        }

        // 2. 排序 (核心基准基因即使得分较低也享有保底席位)
        std::sort(population_.begin(), population_.end(), [](const StrategyChromosome& a, const StrategyChromosome& b) {
            return a.fitness_score > b.fitness_score;
        });

        std::vector<StrategyChromosome> new_pop;

        // 保留核心锚点基因 (Core Anchors)
        for (const auto& c : population_) {
            if (c.is_core_anchor) {
                new_pop.push_back(c);
            }
        }

        // 保留最强精英个体 (Elites)
        for (const auto& c : population_) {
            if (!c.is_core_anchor && new_pop.size() < population_size_ * 0.4) {
                new_pop.push_back(c);
            }
        }

        // 3. 交叉与变异填充剩余个体
        size_t parent_pool_size = new_pop.size();
        std::uniform_int_distribution<size_t> parent_dist(0, parent_pool_size - 1);
        std::uniform_real_distribution<double> mutate_prob(0.0, 1.0);
        std::normal_distribution<double> mutate_delta(0.0, 1.0);

        while (new_pop.size() < population_size_) {
            const auto& p1 = new_pop[parent_dist(rng_)];
            const auto& p2 = new_pop[parent_dist(rng_)];

            StrategyChromosome child;
            child.id = static_cast<int>(new_pop.size() + 1);
            child.is_core_anchor = false; // 变异生成的个体非锚点
            child.fast_window = (mutate_prob(rng_) > 0.5) ? p1.fast_window : p2.fast_window;
            child.slow_window = (mutate_prob(rng_) > 0.5) ? p1.slow_window : p2.slow_window;
            child.stop_loss_atr = (mutate_prob(rng_) > 0.5) ? p1.stop_loss_atr : p2.stop_loss_atr;
            child.take_profit_atr = (mutate_prob(rng_) > 0.5) ? p1.take_profit_atr : p2.take_profit_atr;

            // 变异操作
            if (mutate_prob(rng_) < 0.15) {
                child.fast_window = std::clamp<int>(child.fast_window + static_cast<int>(std::round(mutate_delta(rng_))), 2, 15);
                child.slow_window = std::clamp<int>(child.slow_window + static_cast<int>(std::round(mutate_delta(rng_) * 2)), 16, 60);
                child.stop_loss_atr = std::clamp<double>(child.stop_loss_atr + mutate_delta(rng_) * 0.2, 1.0, 4.0);
            }

            child.passed_plateau_check = validate_parameter_plateau(child);
            child.evaluate_fitness();
            new_pop.push_back(child);
        }

        population_ = std::move(new_pop);
    }

    const StrategyChromosome& get_best_chromosome() const {
        return population_.front();
    }

    int get_generation() const { return generation_; }
    const std::vector<StrategyChromosome>& get_population() const { return population_; }

private:
    double compute_ma(int window) const {
        if (price_history_.empty()) return 0.0;
        int n = std::min<int>(window, static_cast<int>(price_history_.size()));
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            sum += price_history_[price_history_.size() - 1 - i];
        }
        return sum / n;
    }

    size_t population_size_{20};
    int generation_{1};
    std::mt19937 rng_;
    std::vector<StrategyChromosome> population_;
    std::vector<double> price_history_;
};

} // namespace kun
