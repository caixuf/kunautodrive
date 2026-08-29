#include "kun_quant_flowcoro.h"
#include "kun/backtest/backtest_engine.hpp"
#include "kun/strategy/dual_ma_strategy.hpp"
#include "kun/strategy/bollinger_strategy.hpp"
#include "kun/core/logger.hpp"
#include "kun/engine/multi_account_router.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "         鲲量化 (KunQuant) 引擎启动演示               \n";
    std::cout << "  基于 FlowEngine (MessageBus) + flowcoro C++20 协程  \n";
    std::cout << "=======================================================\n\n";

    // ─────────────────────────────────────────────────────────────
    // Part 1: 回测与策略引擎演示 (高性能向量与事件回测)
    // ─────────────────────────────────────────────────────────────
    std::cout << "[Part 1] 运行双均线与布林突破策略回测...\n";

    kun::BacktestConfig config;
    config.initial_capital = 1000000.0;
    config.slippage_points = 1.0;
    config.commission_ratio = 0.0001;
    config.symbol_info = {"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001};

    kun::BacktestEngine bt_engine(config);

    // 构造 1000 根模拟 Bar 数据 (模拟螺纹钢走势)
    std::vector<kun::BarData> mock_bars;
    double price = 3600.0;
    for (int i = 0; i < 1000; ++i) {
        double delta = (i % 40 < 20) ? 2.5 : -2.3; // 构造周期趋势
        price += delta;
        kun::BarData bar;
        bar.symbol = "rb2405";
        bar.exchange = "SHFE";
        bar.open_price = price - 1.0;
        bar.high_price = price + 3.0;
        bar.low_price = price - 3.0;
        bar.close_price = price;
        bar.volume = 1500.0;
        bar.datetime_str = "2026-08-29 09:" + std::to_string(i / 60) + ":" + std::to_string(i % 60);
        mock_bars.push_back(bar);
    }
    bt_engine.set_history_bars(mock_bars);

    auto strategy = std::make_shared<kun::DualMaStrategy>("DualMA_Trend", "rb2405", 5, 20, 2.0);
    bt_engine.set_strategy(strategy);

    kun::PerformanceStats stats = bt_engine.run();
    kun::PerformanceAnalyzer::print_report(stats);
    kun::PerformanceAnalyzer::print_ascii_equity_chart(bt_engine.get_equity_history(), 60, 8);

    // ─────────────────────────────────────────────────────────────
    // Part 2: flowcoro 协程拆单与总线异步分发演示
    // ─────────────────────────────────────────────────────────────
    std::cout << "\n[Part 2] 启动 flowcoro 实时 TWAP 协程拆单与消息总线...\n";

    MessageBus* bus = message_bus_create("kun_quant_bus");

    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex;

    auto twap_task = std::make_unique<kun::CoroTwapExecutionTask>(bus, "rb2405", 50.0, 5, 200000); // 50手, 5批, 每批200ms
    ex.spawn(twap_task->run(), "twap_executor");

    // 模拟行情生产者线程 (发布 10 帧 Tick 行情)
    std::thread quote_thread([bus] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < 8; ++i) {
            kun::QuantTickMsg tick{};
            std::strncpy(tick.symbol, "rb2405", sizeof(tick.symbol) - 1);
            tick.last_price = 3620.0 + i * 1.5;
            tick.bid_price1 = tick.last_price - 1.0;
            tick.ask_price1 = tick.last_price + 1.0;
            tick.bid_volume1 = 50.0;
            tick.ask_volume1 = 50.0;

            message_bus_publish(bus, "market/tick/rb2405", "MockCTP", &tick, sizeof(tick));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // 运行 1.5 秒协程 tick 循环
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() < 1500) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (quote_thread.joinable()) {
        quote_thread.join();
    }

    // ─────────────────────────────────────────────────────────────
    // Part 3: 多账户资金拆分与主从跟单协程演示
    // ─────────────────────────────────────────────────────────────
    std::cout << "\n[Part 3] 启动多账户资金拆分与主从跟单协程...\n";

    // 1. 资金权重自动拆单测试
    kun::MultiAccountRouter router;
    router.add_account({"acc_master_simnow", "SimNow期货仿真", kun::AccountRole::MASTER, 1.0, 1000000.0});
    router.add_account({"acc_fund_a", "产品A基金专户", kun::AccountRole::INDEPENDENT, 0.6, 6000000.0});
    router.add_account({"acc_fund_b", "产品B基金专户", kun::AccountRole::INDEPENDENT, 0.4, 4000000.0});

    kun::OrderRequest big_order{};
    big_order.symbol = "rb2405";
    big_order.direction = kun::Direction::LONG;
    big_order.offset = kun::Offset::OPEN;
    big_order.price = 3625.0;
    big_order.volume = 100.0;
    big_order.order_ref = "BigOrder_Alpha";

    std::cout << "  > 策略下发大单: " << big_order.symbol << " " << big_order.volume << "手 @ " << big_order.price << "\n";
    auto split_results = router.split_order_by_weight(big_order);
    for (const auto& [acc_id, sub_req] : split_results) {
        std::cout << "    ↳ [路由器按资金比例拆单] 账户: " << acc_id 
                  << " -> 分配手数: " << sub_req.volume << "手 (Ref: " << sub_req.order_ref << ")\n";
    }

    // 2. 主从跟单协程测试
    auto follow_task = std::make_unique<kun::CoroFollowTradingTask>(
        bus, "acc_master_simnow",
        std::vector<kun::CoroFollowTradingTask::SlaveConfig>{
            {"acc_slave_zhongxin", 1.5},
            {"acc_slave_yongan", 0.8}
        }
    );
    ex.spawn(follow_task->run(), "follow_copier");

    // 模拟主账户发生 1 笔 10 手成交
    kun::QuantTradeMsg master_trade{};
    std::strncpy(master_trade.symbol, "rb2405", sizeof(master_trade.symbol) - 1);
    master_trade.direction = 0; // BUY
    master_trade.offset = 0;    // OPEN
    master_trade.price = 3625.0;
    master_trade.volume = 10.0;
    master_trade.trade_id = 88001;

    message_bus_publish(bus, "trader/acc_master_simnow/trade_rtn", "CTP_GW", &master_trade, sizeof(master_trade));

    // 推进协程 tick
    for (int i = 0; i < 5; ++i) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ex.shutdown();
    g_node_exec = nullptr;
    message_bus_destroy(bus);

    std::cout << "\n[KunQuant] 全流程演示圆满完成！\n";
    return 0;
}
