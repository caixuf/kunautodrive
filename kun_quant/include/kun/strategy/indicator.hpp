#pragma once

#include <vector>
#include <deque>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace kun {

/**
 * @brief 高性能技术指标算子库 (Indicator Core)
 * 借鉴 Hikyuu 纯算子设计：零堆内存抖动、支持流式 update() 与离线向量计算，
 * 回测引擎 (BacktestEngine)、实盘协程 (FlowCoro) 与在线进化引擎 (Evolution) 100% 共享。
 */
namespace ind {

/**
 * @brief 简单移动平均 (Simple Moving Average)
 */
class SMA {
public:
    explicit SMA(int period = 20) : period_(period) {}

    void reset() {
        buffer_.clear();
        sum_ = 0.0;
    }

    double update(double val) {
        buffer_.push_back(val);
        sum_ += val;
        if (static_cast<int>(buffer_.size()) > period_) {
            sum_ -= buffer_.front();
            buffer_.pop_front();
        }
        return value();
    }

    double value() const {
        if (buffer_.empty()) return 0.0;
        return sum_ / buffer_.size();
    }

    bool is_ready() const { return static_cast<int>(buffer_.size()) >= period_; }
    int period() const { return period_; }

private:
    int period_{20};
    double sum_{0.0};
    std::deque<double> buffer_;
};

/**
 * @brief 指数移动平均 (Exponential Moving Average)
 */
class EMA {
public:
    explicit EMA(int period = 20) : period_(period), alpha_(2.0 / (period + 1.0)) {}

    void reset() {
        ema_ = 0.0;
        initialized_ = false;
    }

    double update(double val) {
        if (!initialized_) {
            ema_ = val;
            initialized_ = true;
        } else {
            ema_ = alpha_ * val + (1.0 - alpha_) * ema_;
        }
        return ema_;
    }

    double value() const { return ema_; }
    bool is_ready() const { return initialized_; }

private:
    int period_{20};
    double alpha_{0.0};
    double ema_{0.0};
    bool initialized_{false};
};

/**
 * @brief 真实波幅均值 (Average True Range)
 */
class ATR {
public:
    explicit ATR(int period = 14) : period_(period), alpha_(1.0 / period) {}

    void reset() {
        last_close_ = 0.0;
        atr_ = 0.0;
        count_ = 0;
    }

    double update(double high, double low, double close) {
        double tr = high - low;
        if (count_ > 0) {
            double tr1 = std::abs(high - last_close_);
            double tr2 = std::abs(low - last_close_);
            tr = std::max({tr, tr1, tr2});
        }
        last_close_ = close;

        if (count_ == 0) {
            atr_ = tr;
        } else if (count_ < period_) {
            atr_ = (atr_ * count_ + tr) / (count_ + 1);
        } else {
            atr_ = (atr_ * (period_ - 1) + tr) / period_;
        }
        count_++;
        return atr_;
    }

    double value() const { return atr_; }
    bool is_ready() const { return count_ >= period_; }

private:
    int period_{14};
    double alpha_{0.0};
    double last_close_{0.0};
    double atr_{0.0};
    int count_{0};
};

/**
 * @brief 布林带 (Bollinger Bands)
 */
struct BollingerBand {
    double mid{0.0};
    double upper{0.0};
    double lower{0.0};
    double bandwidth{0.0};
    double pct_b{0.5};
};

class Bollinger {
public:
    Bollinger(int period = 20, double num_std = 2.0) : period_(period), num_std_(num_std) {}

    void reset() {
        buffer_.clear();
    }

    BollingerBand update(double val) {
        buffer_.push_back(val);
        if (static_cast<int>(buffer_.size()) > period_) {
            buffer_.pop_front();
        }

        BollingerBand b;
        if (buffer_.empty()) return b;

        double sum = std::accumulate(buffer_.begin(), buffer_.end(), 0.0);
        b.mid = sum / buffer_.size();

        if (buffer_.size() < 2) {
            b.upper = b.mid;
            b.lower = b.mid;
            return b;
        }

        double sq_sum = 0.0;
        for (double x : buffer_) {
            sq_sum += (x - b.mid) * (x - b.mid);
        }
        double std_dev = std::sqrt(sq_sum / buffer_.size());

        b.upper = b.mid + num_std_ * std_dev;
        b.lower = b.mid - num_std_ * std_dev;
        b.bandwidth = (b.upper - b.lower) / (b.mid > 0 ? b.mid : 1.0);
        double range = b.upper - b.lower;
        b.pct_b = (range > 0) ? (val - b.lower) / range : 0.5;

        latest_ = b;
        return b;
    }

    BollingerBand value() const { return latest_; }
    bool is_ready() const { return static_cast<int>(buffer_.size()) >= period_; }

private:
    int period_{20};
    double num_std_{2.0};
    std::deque<double> buffer_;
    BollingerBand latest_{};
};

/**
 * @brief 异同移动平均线 (Moving Average Convergence Divergence)
 */
struct MacdOutput {
    double dif{0.0}; // 快线 EMA(fast) - EMA(slow)
    double dea{0.0}; // 慢线 EMA(dif, signal)
    double bar{0.0}; // 柱线 2 * (dif - dea)
};

class MACD {
public:
    MACD(int fast = 12, int slow = 26, int signal = 9)
        : fast_ema_(fast), slow_ema_(slow), signal_ema_(signal) {}

    void reset() {
        fast_ema_.reset();
        slow_ema_.reset();
        signal_ema_.reset();
    }

    MacdOutput update(double price) {
        double f = fast_ema_.update(price);
        double s = slow_ema_.update(price);
        double dif = f - s;
        double dea = signal_ema_.update(dif);
        latest_ = {dif, dea, 2.0 * (dif - dea)};
        return latest_;
    }

    MacdOutput value() const { return latest_; }

private:
    EMA fast_ema_;
    EMA slow_ema_;
    EMA signal_ema_;
    MacdOutput latest_{};
};

} // namespace ind
} // namespace kun
