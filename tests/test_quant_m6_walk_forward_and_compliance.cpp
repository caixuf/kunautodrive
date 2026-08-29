#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <memory>
#include "kun/core/types.hpp"
#include "kun/strategy/dual_ma_strategy.hpp"
#include "kun/strategy/compliance_checker.hpp"
#include "kun/strategy/walk_forward_engine.hpp"
#include "kun/engine/shadow_account.hpp"

using namespace kun;

/**
 * 作弊策略: 偷窥下一根 K 线的收盘价做决策与挂单 (典型未来函数 bug)
 * viewed_bars 是工厂注入的引擎实际数据指针, 平移测试扰动它 → 偷窥者决策必然改变
 * 用于验证 §2-G 数据平移测试 (v2 未来扰动 + 决策日志比对) 能将其拦截
 */
class CheaterFutureStrategy : public IStrategy {
public:
    CheaterFutureStrategy(const std::vector<BarData>* bars, std::string sym)
        : IStrategy("Cheat_FuturePeek"), bars_(bars), symbol_(std::move(sym)) {}

    void on_bar(const BarData& bar) override {
        if (bar.symbol != symbol_ || !bars_) return;
        const int i = idx_++;
        if (i + 1 >= static_cast<int>(bars_->size()) || i % 2 != 0) return;

        // 违规: 引用 i+1 时刻 (未来) 的收盘价决定当前信号与挂单价
        // 注意: 挂单价必须编码未来数据 —— 若只影响方向, 方向在扰动下不变时决策日志无差异
        const double next_close = (*bars_)[i + 1].close_price;
        if (next_close > bar.close_price) {
            buy(symbol_, next_close * 0.999, 1.0);
        } else {
            short_sell(symbol_, next_close * 1.001, 1.0);
        }
    }

private:
    const std::vector<BarData>* bars_;
    std::string symbol_;
    int idx_{0};
};

std::vector<BarData> generate_test_market_data(int num_bars = 100) {
    std::vector<BarData> bars;
    double p = 3600.0;
    for (int i = 0; i < num_bars; ++i) {
        double chg = (std::sin(i * 0.2) * 5.0) + (i * 0.5); // 上升震荡走势
        p += chg;
        BarData b{};
        b.symbol = "rb2405";
        b.open_price = p - 2.0;
        b.high_price = p + 4.0;
        b.low_price = p - 3.0;
        b.close_price = p;
        b.volume = 1000.0 + (i * 10);
        b.datetime_str = "2026-08-30 09:30:00";
        bars.push_back(b);
    }
    return bars;
}

void test_strategy_compliance_checker() {
    std::cout << "[Test 1] 运行 §2-G 策略防作弊硬约束合规机试...\n";
    auto bars = generate_test_market_data(80);

    // 1. 尝试 0 成本回测作弊
    auto res_zero_cost = StrategyComplianceChecker::verify_strategy(
        [](const std::vector<BarData>*) { return std::make_unique<DualMaStrategy>("Cheat_ZeroCost", "rb2405", 5, 20, 1.0); },
        bars,
        0.0,    // 0 滑点
        0.0001, // 手续费
        0.02    // 止损
    );
    assert(!res_zero_cost.passed);
    assert(res_zero_cost.zero_cost_violation);
    std::cout << "  -> 成功拦截 0 滑点虚假回测作弊代码!\n";

    // 2. 尝试无止损无限扛单作弊
    auto res_no_stop = StrategyComplianceChecker::verify_strategy(
        [](const std::vector<BarData>*) { return std::make_unique<DualMaStrategy>("Cheat_NoStop", "rb2405", 5, 20, 1.0); },
        bars,
        1.0,
        0.0001,
        0.0     // 0 止损 (无限扛单)
    );
    assert(!res_no_stop.passed);
    assert(res_no_stop.missing_stop_loss_violation);
    std::cout << "  -> 成功拦截无止损无限扛单违规策略!\n";

    // 3. 正规策略合规通过
    auto res_clean = StrategyComplianceChecker::verify_strategy(
        [](const std::vector<BarData>*) { return std::make_unique<DualMaStrategy>("Clean_DualMA", "rb2405", 5, 20, 1.0); },
        bars,
        1.0,
        0.0001,
        0.02
    );
    assert(res_clean.passed);
    assert(!res_clean.future_leakage_detected);
    std::cout << "  -> 严格合规策略 100% 通过防作弊机试与数据平移检验!\n";

    // 4. 未来函数作弊策略: 偷窥下一根 K 线收盘价 → 必须被 v2 数据平移测试拦截
    auto res_cheat = StrategyComplianceChecker::verify_strategy(
        [](const std::vector<BarData>* viewed) { return std::make_unique<CheaterFutureStrategy>(viewed, "rb2405"); },
        bars,
        1.0,
        0.0001,
        0.02
    );
    assert(!res_cheat.passed);
    assert(res_cheat.future_leakage_detected);
    std::cout << "  -> 成功拦截偷窥未来 K 线的未来函数作弊策略 (v2 决策日志比对)!\n";
}

void test_walk_forward_and_plateau() {
    std::cout << "[Test 2] 运行 Walk-Forward 滚动样本外优化与参数平原稳健性测试...\n";
    auto bars = generate_test_market_data(120);

    auto report = WalkForwardEngine::run_walk_forward(bars, 5, 0.70);
    assert(report.slices.size() >= 3);
    assert(report.is_parameter_plateau);

    std::cout << "  -> Walk-Forward 滚动前向检验完成! 样本外切片数=" << report.slices.size()
              << " | 参数平原稳定性=" << (report.is_parameter_plateau ? "PASS" : "FAIL") << "\n";
}

void test_shadow_account_virtual_run() {
    std::cout << "[Test 3] 运行影子账户 (Shadow Account) 虚拟实盘试运行与热切换测试...\n";
    auto shadow = std::make_unique<ShadowAccountRunner>(
        "Candidate_DualMA_Opt",
        std::make_shared<DualMaStrategy>("Shadow_DualMA", "rb2405", 5, 20, 1.0),
        1000000.0,
        5,
        1.2
    );

    auto bars = generate_test_market_data(50);
    for (const auto& b : bars) {
        QuantTickMsg tick{};
        std::strncpy(tick.symbol, b.symbol.c_str(), sizeof(tick.symbol) - 1);
        tick.last_price = b.close_price;
        shadow->on_tick(tick);
        shadow->on_bar(b);
    }

    auto metrics = shadow->get_metrics();
    assert(metrics.candidate_id == "Candidate_DualMA_Opt");
    assert(metrics.current_equity > 0.0);
    assert(metrics.max_drawdown_pct < 10.0);

    std::cout << "  -> 影子账户试运行指标正常: 当前权益=" << metrics.current_equity 
              << " 最大回撤=" << metrics.max_drawdown_pct << "%\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << " KunQuant M6: Walk-Forward、影子账户与防作弊机试单测集    \n";
    std::cout << "=========================================================\n\n";

    test_strategy_compliance_checker();
    test_walk_forward_and_plateau();
    test_shadow_account_virtual_run();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 M6 进化验证与防作弊单测 100% 断言通过!        \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
