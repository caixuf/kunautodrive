#ifndef INFERENCE_LATENCY_GUARD_H
#define INFERENCE_LATENCY_GUARD_H

#include <algorithm>
#include <array>
#include <cstdint>

namespace adas_infer {

/**
 * 推理延迟滑动窗口监控（快慢双系统确定性预算看护）：
 * 记录最近 kWindow 帧推理耗时（µs），计算均值与 P99；
 * 当 P99 超出实时预算或单帧超出硬上限时触发降级标记，交由快系统接管。
 */
class InferenceLatencyGuard {
public:
    static constexpr int kWindow = 128;

    void record(uint64_t dt_us) {
        samples_[head_] = dt_us;
        head_ = (head_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        last_us_ = dt_us;
    }

    uint64_t last_us() const { return last_us_; }
    int count() const { return count_; }

    double mean_us() const {
        if (count_ == 0) return 0.0;
        uint64_t sum = 0;
        for (int i = 0; i < count_; ++i) sum += samples_[i];
        return static_cast<double>(sum) / static_cast<double>(count_);
    }

    uint64_t p99_us() const {
        if (count_ == 0) return 0;
        std::array<uint64_t, kWindow> sorted{};
        for (int i = 0; i < count_; ++i) sorted[i] = samples_[i];
        std::sort(sorted.begin(), sorted.begin() + count_);
        int idx = static_cast<int>(0.99 * static_cast<double>(count_ - 1));
        if (idx < 0) idx = 0;
        if (idx >= count_) idx = count_ - 1;
        return sorted[idx];
    }

    bool p99_exceeds_budget(uint64_t budget_us) const {
        return count_ >= 10 && p99_us() > budget_us;
    }

    bool hard_timeout(uint64_t hard_budget_us) const {
        return count_ > 0 && last_us_ > hard_budget_us;
    }

private:
    std::array<uint64_t, kWindow> samples_{};
    int head_{0};
    int count_{0};
    uint64_t last_us_{0};
};

}  // namespace adas_infer

#endif  // INFERENCE_LATENCY_GUARD_H
