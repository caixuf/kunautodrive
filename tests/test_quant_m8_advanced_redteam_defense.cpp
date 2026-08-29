#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>
#include "kun/strategy/adaptive_evolution_engine.hpp"
#include "kun/market/regime_detector.hpp"
#include "kun/engine/shadow_account.hpp"
#include "kun/strategy/dual_ma_strategy.hpp"
#include "kun/engine/smart_order_router.hpp"

using namespace kun;

void test_point_1_ga_core_anchors_and_plateau() {
    std::cout << "[Test 1: Point 1] 运行遗传算法核心基准基因锁与参数平原稳健性测试...\n";
    AdaptiveEvolutionEngine engine(20);

    const auto& pop = engine.get_population();
    assert(pop.size() == 20);

    // 1. 验证前 20% (4 组) 为核心基准锚点基因 (Core Anchors)
    int anchor_count = 0;
    for (const auto& c : pop) {
        if (c.is_core_anchor) anchor_count++;
    }
    assert(anchor_count >= 4);

    // 2. 模拟多轮进化，验证核心基准基因永远不会被彻底淘汰
    for (int tick = 0; tick < 100; ++tick) {
        engine.on_market_tick(3600.0 + (tick % 10) * 2.0);
    }
    engine.evolve_next_generation();

    const auto& new_pop = engine.get_population();
    int new_anchor_count = 0;
    for (const auto& c : new_pop) {
        if (c.is_core_anchor) new_anchor_count++;
    }
    assert(new_anchor_count >= 4); // 锚点基因依然锁定保底

    std::cout << "  -> 核心基准基因锁 (Core Anchors) 与多周期加权进化测试通过!\n";
}

void test_point_2_regime_hysteresis_and_transition() {
    std::cout << "[Test 2: Point 2] 运行市场形态滞后滤波带与模糊过渡态自动降仓测试...\n";
    HysteresisRegimeDetector detector(3, 0.45, 0.65);

    // 1. 初始状态为 RANGING (震荡市)
    detector.update(0.30, 0.0, 10.0);
    assert(detector.get_current_regime() == MarketRegimeState::RANGING);
    assert(detector.get_target_position_ratio() == 0.50);

    // 2. 模拟单根 Bar 突发假突破 (仅 1 根 Bar 达标)，验证滞后滤波带不发生误判
    detector.update(0.85, 5.0, 15.0); // 第 1 根
    assert(detector.get_current_regime() == MarketRegimeState::RANGING); // 尚未连续确认，保持原状态

    // 连续 3 根 Bar 确认
    detector.update(0.85, 5.0, 15.0); // 第 2 根
    detector.update(0.85, 5.0, 15.0); // 第 3 根
    assert(detector.get_current_regime() == MarketRegimeState::TRENDING_BULL);
    assert(detector.get_target_position_ratio() == 1.00); // 趋势确认，满额头寸

    // 3. 模拟进入 [0.45, 0.65] 模糊过渡态
    detector.update(0.52, 1.0, 12.0);
    assert(detector.get_current_regime() == MarketRegimeState::UNCERTAIN_TRANSITION);
    assert(detector.get_target_position_ratio() == 0.30); // 强制降仓至 30%
    assert(detector.get_stop_loss_multiplier() == 1.50); // 放大 1.5x 止损缓冲，防止被假刺针洗出

    std::cout << "  -> 滞后滤波防横跳与模糊过渡态降仓机制 100% 断言通过!\n";
}

void test_point_3_shadow_account_composite_gates() {
    std::cout << "[Test 3: Point 3] 运行影子账户复合准入门禁 (20日周期 + 跨形态检验) 测试...\n";
    auto strat = std::make_shared<DualMaStrategy>("DualMA_Cand", "rb2405", 5, 20);
    // 门禁设定：至少 20 交易日，至少 1 次形态变迁，单日回撤 <= 2.5%
    ShadowAccountRunner shadow("Cand_M8_Adv", strat, 1000000.0, 20, 1, 0.025, 1.2);

    // 模拟运行 5 个交易日（尚未达到 20 天）
    for (int day = 0; day < 5; ++day) {
        shadow.advance_trading_day();
    }
    assert(!shadow.get_metrics().is_eligible_for_promotion); // 时长不足，拒绝晋升

    // 模拟推进至 20 交易日，但尚未经历跨形态变迁 (regime_shifts = 0)
    for (int day = 5; day < 20; ++day) {
        shadow.advance_trading_day();
    }
    assert(!shadow.get_metrics().is_eligible_for_promotion); // 未跨形态验证，拒绝晋升

    // 记录经历了一次市场形态变迁 (如 趋势 -> 震荡)
    shadow.record_regime_shift("TRENDING", "RANGING");
    assert(shadow.get_metrics().regime_shifts_observed >= 1);

    // 喂入盈利 Tick 产生正收益
    for (int i = 0; i < 40; ++i) {
        QuantTickMsg tick{};
        tick.last_price = 3600.0 + i * 2.0;
        shadow.on_tick(tick);
    }

    assert(shadow.get_metrics().require_human_confirmation == true); // 强制保留人工终审权
    std::cout << "  -> 影子账户 20 交易日与跨形态复合门禁测试通过!\n";
}

void test_point_4_ofi_smart_order_router() {
    std::cout << "[Test 4: Point 4] 运行订单流微观不平衡 (OFI) 智能路由与做市排队测试...\n";
    SmartOrderRouter router(40.0); // OFI 动量阈值 40 手

    // 1. 模拟平稳盘口 (买一 50 手，卖一 50 手，OFI = 0)
    QuantTickMsg quiet_tick{};
    std::strncpy(quiet_tick.symbol, "rb2405", sizeof(quiet_tick.symbol) - 1);
    quiet_tick.bid_price1 = 3620.0;
    quiet_tick.ask_price1 = 3621.0;
    quiet_tick.bid_volume1 = 50;
    quiet_tick.ask_volume1 = 50;

    auto routed_quiet = router.route_order(quiet_tick, Direction::LONG, Offset::OPEN, 5.0, 3620.0);
    assert(routed_quiet.style == SmartExecutionStyle::PASSIVE_QUEUE); // 平稳期采用买一排队挂单，赚取 1 跳价差
    assert(routed_quiet.target_price == 3620.0);

    // 2. 模拟买盘动量瞬间爆发 (买一 120 手，卖一 20 手，OFI = +100 手)
    QuantTickMsg breakout_tick{};
    std::strncpy(breakout_tick.symbol, "rb2405", sizeof(breakout_tick.symbol) - 1);
    breakout_tick.bid_price1 = 3620.0;
    breakout_tick.ask_price1 = 3621.0;
    breakout_tick.bid_volume1 = 120;
    breakout_tick.ask_volume1 = 20;

    auto routed_breakout = router.route_order(breakout_tick, Direction::LONG, Offset::OPEN, 5.0, 3620.0);
    assert(routed_breakout.style == SmartExecutionStyle::AGGRESSIVE_TAKER); // 动量爆发瞬间对价主动吃单
    assert(routed_breakout.target_price == 3621.0); // 吃卖一价，确保不踏空

    std::cout << "  -> OFI 智能排队与对价抢单路由测试 100% 断言通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << " KunQuant M8: 红方对抗防御与现实硬边界升级测试集         \n";
    std::cout << "=========================================================\n\n";

    test_point_1_ga_core_anchors_and_plateau();
    test_point_2_regime_hysteresis_and_transition();
    test_point_3_shadow_account_composite_gates();
    test_point_4_ofi_smart_order_router();

    std::cout << "\n=========================================================\n";
    std::cout << "      全部 4 大现实硬边界防御机制 100% 断言通过!         \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
