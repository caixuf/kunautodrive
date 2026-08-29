#include "kun/strategy/compliance_checker.hpp"
#include <sstream>
#include <iostream>

namespace kun {

std::string ComplianceCheckResult::summary() const {
    std::ostringstream ss;
    ss << "=== 策略合规性与防作弊机试报告 ===\n";
    ss << " 合规结论: " << (passed ? "[PASS] 100%通过全部硬约束" : "[REJECT] 拦截违规或作弊策略") << "\n";
    ss << " 正常回测夏普比率: " << normal_sharpe << " | 平移测试夏普比率: " << shifted_sharpe << "\n";
    if (!violation_messages.empty()) {
        ss << " 违规拦截明细:\n";
        for (const auto& msg : violation_messages) {
            ss << "   - " << msg << "\n";
        }
    }
    return ss.str();
}

ComplianceCheckResult StrategyComplianceChecker::verify_strategy(
    StrategyFactory factory,
    const std::vector<BarData>& history_bars,
    double slippage,
    double commission_ratio,
    double stop_loss_pct
) {
    ComplianceCheckResult result;

    // 关卡 2.1：零成本违规检查
    if (slippage <= 0.0 || commission_ratio <= 0.0) {
        result.passed = false;
        result.zero_cost_violation = true;
        result.violation_messages.push_back("【硬约束拒绝】禁止设置 0 滑点或 0 手续费进行虚假回测。");
    }

    // 关卡 2.2：缺失止损参数检查
    if (stop_loss_pct <= 0.0) {
        result.passed = false;
        result.missing_stop_loss_violation = true;
        result.violation_messages.push_back("【硬约束拒绝】策略未声明硬性止损比例 (stop_loss_pct <= 0)，禁止无限扛单。");
    }

    // 关卡 3：数据平移未来函数探测
    double norm_sharpe = 0.0;
    double shift_sharpe = 0.0;
    bool has_leakage = detect_future_leakage(factory, history_bars, norm_sharpe, shift_sharpe);
    result.normal_sharpe = norm_sharpe;
    result.shifted_sharpe = shift_sharpe;

    if (has_leakage) {
        result.passed = false;
        result.future_leakage_detected = true;
        result.violation_messages.push_back("【未来函数拦截】数据平移测试失败：策略疑似引用当根 K 线收盘价或未来数据偷窥撮合。");
    }

    return result;
}

bool StrategyComplianceChecker::detect_future_leakage(
    StrategyFactory factory,
    const std::vector<BarData>& history_bars,
    double& out_normal_sharpe,
    double& out_shifted_sharpe
) {
    const size_t n = history_bars.size();
    if (n < 10) return false;

    BacktestConfig cfg;
    cfg.initial_capital = 1000000.0;
    cfg.slippage_points = 1.0;
    cfg.commission_ratio = 0.0001;

    // 1. 正常时间轴回测 (策略工厂拿到引擎真实使用的数据指针)
    BacktestEngine normal_engine(cfg);
    normal_engine.set_history_bars(history_bars);
    normal_engine.set_strategy(factory(&history_bars));
    auto norm_perf = normal_engine.run();
    out_normal_sharpe = norm_perf.sharpe_ratio;

    // 2. 未来扰动回测: 强突变最后一根 K 线 (价格 +30%, 类似一字涨停极端行情)
    //    诚实策略在 bar 0..N-2 的决策只依赖 <= 自身时刻的数据, 与最后一根 K 线无关,
    //    决策日志必须完全一致; 引用了未来数据的策略 (偷窥 bars[i+k]) 决策会改变。
    std::vector<BarData> perturbed_bars = history_bars;
    {
        BarData& last = perturbed_bars.back();
        const double shock = last.close_price * 0.30 + 1.0;
        last.open_price += shock;
        last.high_price += shock;
        last.low_price += shock;
        last.close_price += shock;
        last.volume *= 10.0;
    }

    BacktestEngine perturbed_engine(cfg);
    perturbed_engine.set_history_bars(perturbed_bars);
    perturbed_engine.set_strategy(factory(&perturbed_bars));
    auto pert_perf = perturbed_engine.run();
    out_shifted_sharpe = pert_perf.sharpe_ratio;

    // 3. 决策日志比对: 排除最后一根 bar 上的决策 (其输入确实包含被扰动的 bar)
    const int last_index = static_cast<int>(n) - 1;
    std::vector<std::pair<int, OrderRequest>> decisions_a, decisions_b;
    for (const auto& [bar_idx, req] : normal_engine.get_decision_log()) {
        if (bar_idx < last_index) decisions_a.emplace_back(bar_idx, req);
    }
    for (const auto& [bar_idx, req] : perturbed_engine.get_decision_log()) {
        if (bar_idx < last_index) decisions_b.emplace_back(bar_idx, req);
    }

    if (decisions_a.size() != decisions_b.size()) {
        return true; // 扰动改变了历史决策数量 → 策略看到了未来
    }
    for (size_t i = 0; i < decisions_a.size(); ++i) {
        const auto& a = decisions_a[i].second;
        const auto& b = decisions_b[i].second;
        const bool same = a.direction == b.direction && a.offset == b.offset &&
                          a.order_type == b.order_type && a.symbol == b.symbol &&
                          std::abs(a.price - b.price) < 1e-9 &&
                          std::abs(a.volume - b.volume) < 1e-9;
        if (!same) {
            return true; // 决策内容因未来数据而改变 → 未来函数实锤
        }
    }

    return false;
}

} // namespace kun
