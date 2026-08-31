#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include "kun/engine/matching_engine.hpp"
#include "kun/engine/reconciler.hpp"
#include "kun_quant_flowcoro.h"

using namespace kun;

void test_market_impact_and_depth_slippage() {
    std::cout << "[Test 1] 运行微观市场冲击成本 (Market Impact) 与深度滑点测试...\n";
    MatchingEngine me(1.0, 0.0001); // 1.0 固定滑点
    me.set_market_impact(0.001);     // 0.1% 冲击系数

    // 1. 小单测试 (1 手, 盘口深度 100 手)
    double impact_small = me.calculate_market_impact(1.0, 100.0, 3600.0);
    // Delta_P = 0.001 * 3600 * sqrt(1/100) = 3.6 * 0.1 = 0.36
    std::cout << "  ↳ 1手小单冲击成本: " << impact_small << " 点\n";
    assert(std::abs(impact_small - 0.36) < 0.01);

    // 2. 大单测试 (100 手, 盘口深度 100 手)
    double impact_large = me.calculate_market_impact(100.0, 100.0, 3600.0);
    // Delta_P = 0.001 * 3600 * sqrt(100/100) = 3.6
    std::cout << "  ↳ 100手大单冲击成本: " << impact_large << " 点\n";
    assert(std::abs(impact_large - 3.6) < 0.01);
    assert(impact_large > impact_small * 5.0);

    // 3. 撮合执行测试
    OrderRequest req{};
    req.symbol = "rb2405";
    req.direction = Direction::LONG;
    req.offset = Offset::OPEN;
    req.order_type = OrderType::MARKET;
    req.volume = 100.0;
    me.submit_order(req);

    TickData tick{};
    tick.symbol = "rb2405";
    tick.last_price = 3600.0;
    tick.ask_price[0] = 3600.0;
    tick.ask_volume[0] = 100.0;

    me.match_tick(tick);

    const auto& trades = me.get_all_trades();
    assert(trades.size() == 1);
    // 成交价应包含: 基础ask(3600) + 固定滑点(1.0) + 动态冲击成本(3.6) = 3604.6
    std::cout << "  ↳ 实际撮合成交均价: " << trades[0].price << " (包含真实深度冲击成本!)\n";
    assert(std::abs(trades[0].price - 3604.6) < 0.1);

    std::cout << "  -> 微观市场冲击成本模型测试 100% 通过!\n";
}

void test_balance_invariant_reconciliation() {
    std::cout << "[Test 2] 运行穿透对账资金会计恒等式 (Balance Invariant) 测试...\n";
    double initial_balance = 1000000.0; // 100万初始本金

    std::vector<TradeData> trades;

    // 开多 10 手 @ 3600
    TradeData t1{};
    t1.symbol = "rb2405";
    t1.direction = Direction::LONG;
    t1.offset = Offset::OPEN;
    t1.price = 3600.0;
    t1.volume = 10.0;
    t1.commission = 36.0; // 36 元手续费
    trades.push_back(t1);

    // 平多 10 手 @ 3650 (+50点盈利, 50 * 10 * 10 = +5000元)
    TradeData t2{};
    t2.symbol = "rb2405";
    t2.direction = Direction::SHORT;
    t2.offset = Offset::CLOSE;
    t2.price = 3650.0;
    t2.volume = 10.0;
    t2.commission = 36.5;
    trades.push_back(t2);

    // 理论最终资金: 1000000 + 5000 - 36 - 36.5 = 1004927.5
    double current_balance = 1004927.5;
    double calc_balance = 0.0;
    double diff = 0.0;

    bool is_balanced = SettlementReconciler::validate_balance_invariant(
        initial_balance, current_balance, trades, calc_balance, diff
    );
    std::cout << "  ↳ 平账核算结果: " << (is_balanced ? "平账一致" : "不平账") 
              << ", 理论资金: " << calc_balance << ", 实际资金: " << current_balance << "\n";
    assert(is_balanced);
    assert(diff < 0.01);

    // 制造 10 元偷逃/不平账异常
    double hacked_balance = 1004917.5; // 虚减 10 元
    bool is_hacked_balanced = SettlementReconciler::validate_balance_invariant(
        initial_balance, hacked_balance, trades, calc_balance, diff
    );
    assert(!is_hacked_balanced);
    assert(std::abs(diff - 10.0) < 0.01);
    std::cout << "  ↳ 成功捕获 10.0 元资金不平账异常 (diff=" << diff << " RMB)\n";

    std::cout << "  -> 资金会计恒等式穿透对账测试 100% 通过!\n";
}

void test_basis_arbitrage_coroutine_loop() {
    std::cout << "[Test 3] 运行跨期基差对冲套利协程 (Basis Arbitrage Coroutine) 测试...\n";
    MessageBus* bus = message_bus_create("Campaign3Bus");
    assert(bus != nullptr);

    static std::mutex order_mtx;
    static std::vector<QuantOrderReqMsg> received_orders;
    received_orders.clear();

    message_bus_subscribe(bus, "trader/acc_arb_01/order_req", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantOrderReqMsg)) {
            std::lock_guard<std::mutex> lk(order_mtx);
            const auto* req = reinterpret_cast<const QuantOrderReqMsg*>(msg->data);
            received_orders.push_back(*req);
        }
    }, nullptr);

    // 启动对冲套利任务: 监听 rb2405 (近月) vs rb2410 (远月), 开仓阈值=30, 平仓阈值=5
    auto arb_task = std::make_unique<CoroBasisArbitrageTask>(
        bus, "acc_arb_01", "rb2405", "rb2410", 30.0, 5.0, 2.0
    );

    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex;
    ex.spawn(arb_task->run(), "basis_arb");

    ex.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 模拟注入行情: 近月 3650, 远月 3600 (价差 50 > 30 阈值)
    QuantTickMsg tick_a{};
    std::strncpy(tick_a.symbol, "rb2405", sizeof(tick_a.symbol) - 1);
    tick_a.last_price = 3650.0;

    QuantTickMsg tick_b{};
    std::strncpy(tick_b.symbol, "rb2410", sizeof(tick_b.symbol) - 1);
    tick_b.last_price = 3600.0;

    // 循环注入两腿行情驱动跨期价差计算
    for (int iter = 0; iter < 5; ++iter) {
        message_bus_publish(bus, "market/tick/rb2405", "MockFeed", &tick_a, sizeof(tick_a));
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        message_bus_publish(bus, "market/tick/rb2410", "MockFeed", &tick_b, sizeof(tick_b));
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        {
            std::lock_guard<std::mutex> lk(order_mtx);
            if (received_orders.size() >= 2) break;
        }
    }

    std::vector<QuantOrderReqMsg> snapshot;
    {
        std::lock_guard<std::mutex> lk(order_mtx);
        snapshot = received_orders;
    }

    assert(snapshot.size() >= 2);
    std::cout << "  ↳ 成功捕获双腿并发对冲报单 (" << snapshot.size() << " 笔):\n";
    std::cout << "     腿1: " << snapshot[0].symbol << " 方向=" << (snapshot[0].direction == 1 ? "卖空" : "买多") << " 价格=" << snapshot[0].price << "\n";
    std::cout << "     腿2: " << snapshot[1].symbol << " 方向=" << (snapshot[1].direction == 0 ? "买多" : "卖空") << " 价格=" << snapshot[1].price << "\n";

    assert(snapshot[0].direction == 1 && std::strcmp(snapshot[0].symbol, "rb2405") == 0);
    assert(snapshot[1].direction == 0 && std::strcmp(snapshot[1].symbol, "rb2410") == 0);

    arb_task->set_stop();
    ex.run();
    ex.shutdown();
    arb_task.reset();
    g_node_exec = nullptr;
    message_bus_destroy(bus);

    std::cout << "  -> 跨期基差对冲套利协程测试 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "     第三战役【天工开物 · 闭环阵】真实滑点对冲单测集        \n";
    std::cout << "=========================================================\n\n";

    test_market_impact_and_depth_slippage();
    test_balance_invariant_reconciliation();
    test_basis_arbitrage_coroutine_loop();

    std::cout << "\n=========================================================\n";
    std::cout << "       第三战役全部 3 组闭环单测 100% 满分通过!            \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
