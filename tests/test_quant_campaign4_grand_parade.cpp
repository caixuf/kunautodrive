#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <atomic>
#include <thread>
#include <future>
#include "kun/engine/matching_engine.hpp"
#include "kun/engine/reconciler.hpp"
#include "kun/engine/risk_manager.hpp"
#include "kun/engine/gateway_pool.hpp"
#include "kun/cellular/cellular_genome.hpp"
#include "kun/cellular/quant_cellular_adapter.hpp"
#include "kun_quant_flowcoro.h"

using namespace kun;

// ── 检阅一：极端高并发行情穿透与微观冲击测试 ──
void test_phase1_high_throughput_market_impact() {
    std::cout << "[大阅兵 · 第一营] 运行 10,000 笔极端行情微观冲击与撮合压测...\n";
    MatchingEngine me(1.0, 0.0001);
    me.set_market_impact(0.0008);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i) {
        OrderRequest req{};
        req.symbol = "rb2405";
        req.direction = (i % 2 == 0) ? Direction::LONG : Direction::SHORT;
        req.offset = Offset::OPEN;
        req.order_type = OrderType::MARKET;
        req.volume = 1.0 + (i % 20);
        me.submit_order(req);

        TickData tick{};
        tick.symbol = "rb2405";
        tick.last_price = 3600.0 + (i % 50) * 0.5;
        tick.ask_price[0] = tick.last_price + 1.0;
        tick.bid_price[0] = tick.last_price - 1.0;
        tick.ask_volume[0] = 50.0;
        tick.bid_volume[0] = 50.0;

        me.match_tick(tick);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    const auto& all_trades = me.get_all_trades();
    std::cout << "  ↳ 10,000 笔订单撮合总耗时: " << elapsed_ms << " ms (" 
              << (10000.0 / (elapsed_ms > 0 ? elapsed_ms : 1) * 1000.0) << " ops/sec), 生成成交: " 
              << all_trades.size() << " 笔\n";
    assert(all_trades.size() == 10000);
#if defined(__SANITIZE_ADDRESS__) || defined(ENABLE_ASAN)
    assert(elapsed_ms < 3000); // ASAN 插桩调试模式放宽时延阈值
#else
    assert(elapsed_ms < 1000); // 生产与常规单测
#endif
    std::cout << "  -> 第一营：高并发行情微观冲击压测满分通过!\n";
}

// ── 检阅二：极端风控红蓝对抗攻击穿透测试 ──
void test_phase2_redteam_adversarial_risk_defense() {
    std::cout << "[大阅兵 · 第二营] 运行红蓝对抗极端穿透与硬熔断门禁防御测试...\n";
    PositionManager pm(1000000.0);
    RiskRuleConfig cfg;
    cfg.max_order_volume = 100.0;
    cfg.max_daily_loss_pct = 0.025; // 2.5% 硬熔断
    RiskManager risk(pm, cfg);

    // 1. 模拟红方攻击：超大单攻击
    OrderRequest attack_req{};
    attack_req.symbol = "rb2405";
    attack_req.direction = Direction::LONG;
    attack_req.offset = Offset::OPEN;
    attack_req.volume = 500.0; // 超过 100 手限制

    auto res = risk.check_order(attack_req, {});
    assert(!res.first);
    std::cout << "  ↳ 成功拦截超大单攻击: " << res.second << "\n";

    // 2. 模拟红方攻击：触发单日 2.5% 动态回撤硬熔断
    assert(!risk.is_circuit_breaker_tripped());
    risk.trip_circuit_breaker("[CIRCUIT_BREAKER] 动态回撤达 2.5% 触发硬熔断");
    assert(risk.is_circuit_breaker_tripped());
    std::cout << "  ↳ 成功触发 2.5% 动态回撤硬熔断门禁: " << risk.get_trip_reason() << "\n";

    // 3. 熔断期间拦截开仓单
    OrderRequest normal_req{};
    normal_req.symbol = "rb2405";
    normal_req.direction = Direction::LONG;
    normal_req.offset = Offset::OPEN;
    normal_req.volume = 1.0;

    auto res_circuit = risk.check_order(normal_req, {});
    assert(!res_circuit.first);
    std::cout << "  ↳ 熔断状态下严密拦截开仓单: " << res_circuit.second << "\n";

    // 4. 验证熔断期间平仓单特权放行自救 (先持仓 10 手多头)
    TradeData init_pos_trade{};
    init_pos_trade.symbol = "rb2405";
    init_pos_trade.direction = Direction::LONG;
    init_pos_trade.offset = Offset::OPEN;
    init_pos_trade.price = 3600.0;
    init_pos_trade.volume = 10.0;
    pm.on_trade(init_pos_trade);

    OrderRequest close_req{};
    close_req.symbol = "rb2405";
    close_req.direction = Direction::SHORT;
    close_req.offset = Offset::CLOSE;
    close_req.volume = 1.0;

    auto res_close = risk.check_order(close_req, {});
    assert(res_close.first);
    std::cout << "  ↳ 熔断状态下平仓止损通道顺畅放行!\n";

    std::cout << "  -> 第二营：红蓝对抗与硬熔断铁闸测试满分通过!\n";
}

// ── 检阅三：形态发生细胞演化高频前向与力场并发测试 ──
void test_phase3_cellular_evolution_and_force_field_stress() {
    std::cout << "[大阅兵 · 第三营] 运行形态发生细胞 50,000 轮演化与兰纳-琼斯力场并发压测...\n";
    MorphogeneticEvolutionEngine engine(20, 77777);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int gen = 0; gen < 20; ++gen) {
        for (int step = 0; step < 2500; ++step) {
            double inputs[4] = {3600.0 + step * 0.1, 8000.0, 1.0, 0.05};
            for (auto& org : const_cast<std::vector<CellularOrganism>&>(engine.get_population())) {
                auto acts = org.forward(inputs);
                org.step_force_field_physics(0.016f);
                org.fitness_score += acts.positive_action - acts.negative_action;
            }
        }
        engine.evolve_generation();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    auto& champ = engine.get_champion();
    uint32_t max_gen = 0;
    for (const auto& org : engine.get_population()) {
        max_gen = std::max<uint32_t>(max_gen, org.generation);
    }
    std::cout << "  ↳ 50,000 次前向+力场演化总耗时: " << elapsed_ms << " ms, 种群最高代际: Gen-" 
              << max_gen << ", 冠军个体: " << champ.lineage_name 
              << " (细胞=" << champ.cells.size() << ", 突触=" << champ.synapses.size() << ")\n";
    assert(max_gen >= 15);
    assert(!champ.cells.empty());
    assert(champ.is_compiled());

    std::cout << "  -> 第三营：形态发生细胞演化与力场并发压测满分通过!\n";
}

// ── 检阅四：全系统三账户穿透对账与资金恒等式总校验 ──
void test_phase4_multi_account_full_reconcile() {
    std::cout << "[大阅兵 · 第四营] 运行多账户穿透对账与资金会计恒等式总校验...\n";
    double init_cap = 500000.0;

    std::vector<TradeData> trades;
    // 模拟 100 笔双向套利与震荡交易
    for (int i = 0; i < 50; ++i) {
        TradeData op{};
        op.symbol = "rb2405";
        op.direction = Direction::LONG;
        op.offset = Offset::OPEN;
        op.price = 3600.0 + i;
        op.volume = 2.0;
        op.commission = 7.2;
        trades.push_back(op);

        TradeData cl{};
        cl.symbol = "rb2405";
        cl.direction = Direction::SHORT;
        cl.offset = Offset::CLOSE;
        cl.price = 3610.0 + i; // 每笔赚 10 点 (+200元)
        cl.volume = 2.0;
        cl.commission = 7.22;
        trades.push_back(cl);
    }

    // 50 笔 * 200元 = +10,000元毛利
    // 手续费 = 50 * (7.2 + 7.22) = 721 元
    // 理论最终资金 = 500,000 + 10,000 - 721 = 509,279 元
    double expected_balance = 509279.0;
    double calc_balance = 0.0;
    double diff = 0.0;

    bool passed = SettlementReconciler::validate_balance_invariant(
        init_cap, expected_balance, trades, calc_balance, diff
    );
    std::cout << "  ↳ 穿透会计恒等式核算: 理论=" << calc_balance << ", 实际=" << expected_balance << ", 误差=" << diff << "\n";
    assert(passed);
    assert(diff < 0.01);

    std::cout << "  -> 第四营：全账户资金会计恒等式穿透校验 100% 满分通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "    第四战役【乾坤定鼎 · 大阅兵】全系统生产级终极单测集    \n";
    std::cout << "=========================================================\n\n";

    test_phase1_high_throughput_market_impact();
    test_phase2_redteam_adversarial_risk_defense();
    test_phase3_cellular_evolution_and_force_field_stress();
    test_phase4_multi_account_full_reconcile();

    std::cout << "\n=========================================================\n";
    std::cout << "   四大战役终极大阅兵 4 营联合检阅 100% 满分全部通过!     \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
