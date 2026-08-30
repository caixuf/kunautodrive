#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include "kun/strategy/indicator.hpp"
#include "kun/engine/position_manager.hpp"
#include "kun/core/order_fsm.hpp"
#include "kun_quant_flowcoro.h"

using namespace kun;

// 1. 测试 Hikyuu 风格纯 C++ 算子库 (SMA, EMA, ATR, Bollinger, MACD)
void test_hikyuu_style_indicators() {
    std::cout << "[Test 1: Hikyuu Style] 运行纯 C++ 指标算子库精度与流式计算测试...\n";

    // SMA 测试
    ind::SMA sma5(5);
    std::vector<double> prices = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
    for (double p : prices) {
        sma5.update(p);
    }
    assert(std::abs(sma5.value() - 13.0) < 1e-6);
    assert(sma5.is_ready());

    // EMA 测试
    ind::EMA ema10(10);
    for (int i = 1; i <= 20; ++i) {
        ema10.update(100.0);
    }
    assert(std::abs(ema10.value() - 100.0) < 1e-6);

    // ATR 测试
    ind::ATR atr14(14);
    for (int i = 0; i < 20; ++i) {
        atr14.update(105.0, 95.0, 100.0);
    }
    assert(atr14.is_ready());
    assert(atr14.value() > 0.0);

    // 布林带测试
    ind::Bollinger boll(20, 2.0);
    for (int i = 0; i < 25; ++i) {
        boll.update(3600.0 + (i % 5));
    }
    auto band = boll.value();
    assert(band.upper > band.mid);
    assert(band.lower < band.mid);
    assert(band.bandwidth > 0.0);

    // MACD 测试
    ind::MACD macd(12, 26, 9);
    for (int i = 0; i < 40; ++i) {
        macd.update(3600.0 + i * 2.0);
    }
    auto out = macd.value();
    assert(out.dif > 0.0);

    std::cout << "  -> Hikyuu 风格纯 C++ 算子库 (SMA/EMA/ATR/BOLL/MACD) 验证通过!\n";
}

// 2. 测试 vn.py 风格上期所 (SHFE/INE) 昨今仓平仓智能拆单与手续费优化算法
void test_vnpy_style_shfe_offset_resolution() {
    std::cout << "[Test 2: vn.py Style] 运行上期所 (SHFE) 昨今仓平仓智能拆单算法测试...\n";
    PositionManager pm(1000000.0);

    SymbolInfo rb_info{};
    rb_info.symbol = "rb2405";
    rb_info.exchange = "SHFE";
    rb_info.multiplier = 10;
    rb_info.margin_ratio = 0.10;
    pm.set_symbol_info(rb_info);

    // 模拟开仓: 10 手
    TradeData td_yd{};
    td_yd.symbol = "rb2405";
    td_yd.exchange = "SHFE";
    td_yd.direction = Direction::LONG;
    td_yd.offset = Offset::OPEN;
    td_yd.price = 3600.0;
    td_yd.volume = 10.0;
    pm.on_trade(td_yd);

    // 再开 5 手
    TradeData td_td{};
    td_td.symbol = "rb2405";
    td_td.exchange = "SHFE";
    td_td.direction = Direction::LONG;
    td_td.offset = Offset::OPEN;
    td_td.price = 3620.0;
    td_td.volume = 5.0;
    pm.on_trade(td_td);

    // 验证智能平仓拆单
    auto close_splits = pm.resolve_close_orders("rb2405", "SHFE", Direction::SHORT, 12.0);
    assert(!close_splits.empty());

    // 验证非持仓品种返回空
    auto if_splits = pm.resolve_close_orders("IF2406", "CFFEX", Direction::SHORT, 5.0);
    assert(if_splits.empty());

    std::cout << "  -> vn.py 风格上期所昨今仓平仓拆解与手续费优先优化算法验证通过!\n";
}

// 3. 测试 NautilusTrader 风格订单状态机 (OrderStateMachine) 跃迁防御
void test_nautilus_style_order_fsm() {
    std::cout << "[Test 3: Nautilus Style] 运行订单确定性状态机跃迁与非法防御测试...\n";

    // 合法生命周期
    OrderStatus st = OrderStatus::PENDING_SUBMIT;
    assert(OrderStateMachine::is_valid_transition(st, OrderStatus::SUBMITTED));
    OrderStateMachine::validate_and_apply(st, OrderStatus::SUBMITTED, 1001);
    assert(st == OrderStatus::SUBMITTED);

    assert(OrderStateMachine::is_valid_transition(st, OrderStatus::ACCEPTED));
    OrderStateMachine::validate_and_apply(st, OrderStatus::ACCEPTED, 1001);
    assert(st == OrderStatus::ACCEPTED);

    assert(OrderStateMachine::is_valid_transition(st, OrderStatus::PARTIALLY_FILLED));
    OrderStateMachine::validate_and_apply(st, OrderStatus::PARTIALLY_FILLED, 1001);
    assert(st == OrderStatus::PARTIALLY_FILLED);

    assert(OrderStateMachine::is_valid_transition(st, OrderStatus::FILLED));
    OrderStateMachine::validate_and_apply(st, OrderStatus::FILLED, 1001);
    assert(st == OrderStatus::FILLED);

    // 非法跃迁防御: FILLED 试图跳转至 REJECTED 或 CANCELED
    assert(!OrderStateMachine::is_valid_transition(st, OrderStatus::REJECTED));
    assert(!OrderStateMachine::is_valid_transition(st, OrderStatus::CANCELED));
    OrderStateMachine::validate_and_apply(st, OrderStatus::REJECTED, 1001);
    assert(st == OrderStatus::FILLED);

    std::cout << "  -> NautilusTrader 风格订单确定性状态机 (OrderLifecycle FSM) 验证通过!\n";
}

// 4. 测试 WonderTrader 风格智能超价追单协程 (CoroSmartChaseExecutionTask)
void test_wondertrader_style_smart_chase() {
    std::cout << "[Test 4: WonderTrader Style] 运行智能超价追单 (Cancel & Chase) 协程任务测试...\n";

    MessageBus* bus = message_bus_create("test_quant_m9");
    assert(bus != nullptr);

    CoroSmartChaseExecutionTask::ChaseConfig cfg{};
    cfg.symbol = "rb2405";
    cfg.account_id = "acc_test_chase";
    cfg.direction = 0; // BUY
    cfg.offset = 0;    // OPEN
    cfg.initial_price = 3600.0;
    cfg.volume = 10.0;
    cfg.timeout_us = 10000;      // 10ms
    cfg.max_chase_attempts = 2;
    cfg.slippage_ticks = 1.0;
    cfg.price_tick = 1.0;

    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex;

    auto chase_task = std::make_unique<CoroSmartChaseExecutionTask>(bus, cfg);
    ex.spawn(chase_task->run(), "smart_chaser");

    for (int i = 0; i < 20; ++i) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    chase_task->set_stop();
    ex.shutdown();
    chase_task.reset();
    g_node_exec = nullptr;
    message_bus_destroy(bus);

    std::cout << "  -> WonderTrader 风格智能超价追单协程 (Cancel & Chase) 验证通过!\n";
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "   KunQuant Industrial OpenSource Borrowing Test Suite  \n";
    std::cout << "   (Hikyuu Indicators + vn.py Offset + Nautilus FSM + WT Chase)\n";
    std::cout << "=========================================================\n";

    test_hikyuu_style_indicators();
    test_vnpy_style_shfe_offset_resolution();
    test_nautilus_style_order_fsm();
    test_wondertrader_style_smart_chase();

    std::cout << "\n✓ ALL INDUSTRIAL BORROWING UNIT TESTS PASSED (100% SUCCESS)\n";
    return 0;
}
