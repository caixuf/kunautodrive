/**
 * @file rt_heartbeat_demo.cpp
 * @brief 20Hz flowcoro::rt::RtExecutor 心跳 — 测 tick 间隔 / tardiness。
 *
 * 对齐 flowcoro examples/autonomous_driving/rt_control_loop_demo.cpp 的精神：
 * 单线程 RtExecutor + sleep_until 对齐周期，跑几秒后 request_stop / shutdown
 * 干净退出。不碰 C scheduler，不重写 coroutine_task.h。
 *
 * 与生产节点共用 TLS g_node_exec；宿主按 next_timer_deadline() 等待，
 * 而不是 node_pump 的 200µs 轮询（那会把测量分辨率钉在 200µs）。
 *
 * 构建（优先 sibling flowcoro）:
 *   git clone https://github.com/caixuf/flowcoro.git ../flowcoro
 *   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build --target rt_heartbeat_demo
 *   ./build/bin/rt_heartbeat_demo [秒数=2]
 */

#include "coroutine_task.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

using SteadyClock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

flowcoro::rt::RtTask heartbeat_20hz(std::chrono::microseconds period,
                                    std::vector<int64_t>& intervals_us,
                                    std::vector<int64_t>& late_us,
                                    std::atomic<int>& ticks) {
    const auto origin = SteadyClock::now();
    SteadyClock::time_point prev_tp{};
    bool have_prev = false;
    int i = 0;
    while (!co_await flowcoro::rt::stop_requested()) {
        ++i;
        const auto deadline = origin + period * i;
        co_await flowcoro::rt::sleep_until(deadline);
        const auto now = SteadyClock::now();
        ticks.fetch_add(1, std::memory_order_relaxed);

        auto tardiness = now - deadline;
        if (tardiness < SteadyClock::duration::zero()) tardiness = SteadyClock::duration::zero();
        late_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(tardiness).count());
        if (have_prev) {
            intervals_us.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(now - prev_tp).count());
        }
        have_prev = true;
        prev_tp = now;
    }
}

bool drive(flowcoro::rt::RtExecutor& exec, SteadyClock::time_point until) {
    while (SteadyClock::now() < until && !exec.is_finished()) {
        exec.run();
        if (exec.has_local_work()) continue;
        if (auto next = exec.next_timer_deadline()) {
            std::this_thread::sleep_until(*next);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    exec.request_stop();
    const auto drain_by = SteadyClock::now() + 2s;
    while (!exec.is_finished() && SteadyClock::now() < drain_by) {
        exec.run();
        if (exec.has_local_work()) continue;
        if (auto next = exec.next_timer_deadline()) {
            std::this_thread::sleep_until(*next);
        }
    }
    if (!exec.is_finished()) exec.shutdown();
    return exec.is_finished();
}

struct Stats {
    int n = 0;
    int64_t min_us = 0;
    int64_t max_us = 0;
    double mean_us = 0;
    int64_t p50_us = 0;
    int64_t p99_us = 0;
};

Stats summarize(std::vector<int64_t> samples) {
    Stats s;
    s.n = static_cast<int>(samples.size());
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.min_us = samples.front();
    s.max_us = samples.back();
    const int64_t sum = std::accumulate(samples.begin(), samples.end(), int64_t{0});
    s.mean_us = static_cast<double>(sum) / static_cast<double>(s.n);
    s.p50_us = samples[static_cast<size_t>(s.n * 0.50)];
    s.p99_us = samples[std::min(s.n - 1, static_cast<int>(s.n * 0.99))];
    return s;
}

void print_report(const char* tag, const Stats& r) {
    std::cout << std::fixed << std::setprecision(3)
              << "  " << std::setw(10) << tag
              << " n=" << r.n
              << " p50=" << (r.p50_us / 1000.0) << "ms"
              << " p99=" << (r.p99_us / 1000.0) << "ms"
              << " max=" << (r.max_us / 1000.0) << "ms"
              << " mean=" << (r.mean_us / 1000.0) << "ms\n";
}

}  // namespace

int main(int argc, char** argv) {
    int run_seconds = 2;
    if (argc >= 2) run_seconds = std::max(1, std::atoi(argv[1]));

    constexpr int kHz = 20;
    const auto period = std::chrono::microseconds(1'000'000 / kHz);

    std::cout << "\nFlowEngine rt heartbeat demo\n";
    std::cout << "  duration : " << run_seconds << " s\n";
    std::cout << "  control  : " << kHz << " Hz (rt::sleep_until aligned)\n";
    std::cout << "  shutdown : request_stop + drain + shutdown\n\n";

    std::vector<int64_t> intervals;
    std::vector<int64_t> late;
    std::atomic<int> ticks{0};
    intervals.reserve(static_cast<size_t>(run_seconds * kHz + 8));
    late.reserve(static_cast<size_t>(run_seconds * kHz + 8));

    flowcoro::rt::RtExecutor exec(flowcoro::rt::RtExecutor::Config{
        .pin_cpu = -1,
        .idle_sleep_us = 0,  // 本 demo 自己按 next_timer_deadline 等待
    });
    g_node_exec = &exec;
    exec.spawn(heartbeat_20hz(period, intervals, late, ticks), "heartbeat");

    const bool ok = drive(exec, SteadyClock::now() + std::chrono::seconds(run_seconds));
    g_node_exec = nullptr;

    const auto iv = summarize(intervals);
    const auto lt = summarize(late);
    const int n = ticks.load();
    const int expect_min = (run_seconds * kHz * 3) / 4;  // ≥75% of nominal 20Hz
    const bool rate_ok = n >= expect_min;
    const bool interval_ok = iv.n == 0 || (iv.mean_us >= 35000.0 && iv.mean_us <= 70000.0);

    std::cout << "  ticks=" << n
              << " expect>=" << expect_min
              << " finished=" << (ok ? "yes" : "NO") << "\n";
    print_report("interval", iv);
    print_report("late", lt);
    std::cout << "\n  墙钟抖动，不是硬实时证书。"
              << " 对照: flowcoro rt_control_loop_demo.cpp\n\n";

    if (!ok) {
        std::cerr << "FAIL: executor did not finish after request_stop/shutdown\n";
        return 1;
    }
    if (!rate_ok) {
        std::cerr << "FAIL: tick count " << n << " < " << expect_min
                  << " (not ~" << kHz << " Hz over " << run_seconds << "s)\n";
        return 1;
    }
    if (!interval_ok) {
        std::cerr << "FAIL: mean interval " << (iv.mean_us / 1000.0)
                  << " ms not near 50 ms\n";
        return 1;
    }
    return 0;
}
