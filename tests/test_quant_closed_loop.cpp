#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include "kun/core/types.hpp"
#include "kun/engine/matching_engine.hpp"
#include "kun/engine/position_manager.hpp"
#include "kun/engine/gateway_pool.hpp"
#include "kun/backtest/performance.hpp"
#include "kun_quant_flowcoro.h"
#include "message_bus.h"
#include "coroutine_task.h"

using namespace kun;

void test_pod_memory_safety() {
    std::cout << "[Test 1] 运行 POD 结构体内存对齐与零拷贝安全测试...\n";
    assert(sizeof(QuantTickMsg) > 0);
    assert(sizeof(QuantOrderReqMsg) > 0);
    assert(sizeof(QuantOrderRtnMsg) > 0);
    assert(sizeof(QuantTradeMsg) > 0);

    MessageBus* bus = message_bus_create("test_bus_pod");
    assert(bus != nullptr);

    QuantTickMsg send_tick{};
    std::strncpy(send_tick.symbol, "rb2405", sizeof(send_tick.symbol) - 1);
    send_tick.last_price = 3625.5;
    send_tick.volume = 1200.0;
    send_tick.bid_price1 = 3625.0;
    send_tick.ask_price1 = 3626.0;

    // message_bus_publish 异步入队, 等总线派发线程送达后再断言
    static std::atomic<bool> received{false};
    static QuantTickMsg recv_tick{};

    message_bus_subscribe(bus, "market/tick/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            recv_tick = *reinterpret_cast<const QuantTickMsg*>(msg->data);
            received = true;
        }
    }, nullptr);

    message_bus_publish(bus, "market/tick/rb2405", "Publisher", &send_tick, sizeof(send_tick));

    for (int i = 0; i < 500 && !received.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(received.load());
    assert(std::strcmp(recv_tick.symbol, "rb2405") == 0);
    assert(std::abs(recv_tick.last_price - 3625.5) < 1e-4);

    message_bus_destroy(bus);
    std::cout << "  -> POD 内存对齐与总线零拷贝传输测试通过!\n";
}

void test_matching_engine_depth_and_partial_fill() {
    std::cout << "[Test 2] 运行撮合引擎盘口深度约束与部分成交测试...\n";
    MatchingEngine me(0.0, 0.0001);
    me.set_symbol_multiplier("rb2405", 10.0);

    OrderRequest req;
    req.symbol = "rb2405";
    req.direction = Direction::LONG;
    req.offset = Offset::OPEN;
    req.order_type = OrderType::LIMIT;
    req.price = 3650.0;
    req.volume = 100.0; // 挂大单 100 手

    uint64_t order_id = me.submit_order(req);
    assert(order_id > 0);

    // 行情卖一只有 20 手深度
    TickData tick1;
    tick1.symbol = "rb2405";
    tick1.last_price = 3620.0;
    tick1.ask_price[0] = 3620.0;
    tick1.ask_volume[0] = 20.0;

    me.match_tick(tick1);

    auto active = me.get_active_orders();
    assert(active.size() == 1);
    assert(active[0].status == OrderStatus::PARTIALLY_FILLED);
    assert(std::abs(active[0].traded_volume - 20.0) < 1e-4);

    // 下一 Tick 卖一提供 80 手深度，全量吃掉
    TickData tick2;
    tick2.symbol = "rb2405";
    tick2.last_price = 3621.0;
    tick2.ask_price[0] = 3621.0;
    tick2.ask_volume[0] = 80.0;

    me.match_tick(tick2);

    auto active_after = me.get_active_orders();
    assert(active_after.empty()); // 已全量成交，无挂单

    auto trades = me.get_all_trades();
    assert(trades.size() == 2);
    assert(std::abs(trades[0].volume - 20.0) < 1e-4);
    assert(std::abs(trades[1].volume - 80.0) < 1e-4);

    std::cout << "  -> 盘口深度约束与部分成交 (Partial Fill) 测试通过!\n";
}

void test_position_accounting_and_close_today() {
    std::cout << "[Test 3] 运行持仓管理、平今仓扣减与冻结资金测试...\n";
    PositionManager pm(1000000.0);
    SymbolInfo info{"rb2405", "SHFE", 10, 1.0, 0.10, 0.0001};
    pm.set_symbol_info(info);

    // 1. 开多 10 手
    TradeData t1;
    t1.trade_id = 1;
    t1.symbol = "rb2405";
    t1.direction = Direction::LONG;
    t1.offset = Offset::OPEN;
    t1.price = 3600.0;
    t1.volume = 10.0;
    t1.commission = 3.6;
    pm.on_trade(t1);

    auto p1 = pm.get_position("rb2405", Direction::LONG);
    assert(std::abs(p1.volume - 10.0) < 1e-4);
    assert(std::abs(p1.today_volume - 10.0) < 1e-4);

    // 2. 冻结 4 手持仓用于挂单平仓
    bool freeze_ok = pm.freeze_position("rb2405", Direction::LONG, 4.0);
    assert(freeze_ok);
    p1 = pm.get_position("rb2405", Direction::LONG);
    assert(std::abs(p1.frozen - 4.0) < 1e-4);

    // 3. 平今 4 手成交
    TradeData t2;
    t2.trade_id = 2;
    t2.symbol = "rb2405";
    t2.direction = Direction::SHORT;
    t2.offset = Offset::CLOSE_TODAY;
    t2.price = 3630.0; // 盈利 30 点 * 4手 * 10乘数 = +1200 元
    t2.volume = 4.0;
    t2.commission = 1.45;
    pm.on_trade(t2);

    auto p2 = pm.get_position("rb2405", Direction::LONG);
    assert(std::abs(p2.volume - 6.0) < 1e-4);
    assert(std::abs(p2.today_volume - 6.0) < 1e-4);
    assert(std::abs(p2.frozen - 0.0) < 1e-4); // 自动解冻
    assert(std::abs(p2.realized_pnl - 1200.0) < 1e-4);

    auto acc = pm.get_account();
    assert(acc.realized_pnl > 1190.0);

    std::cout << "  -> 今昨仓管理、平今扣减与冻结资金测试通过!\n";
}

void test_performance_trade_pairing() {
    std::cout << "[Test 4] 运行开平仓匹配与胜率盈亏比统计测试...\n";
    std::vector<TradeData> trades;

    TradeData td1{};
    td1.trade_id = 1;
    td1.symbol = "rb2405";
    td1.direction = Direction::LONG;
    td1.offset = Offset::OPEN;
    td1.price = 3600.0;
    td1.volume = 10.0;
    td1.commission = 3.6;
    trades.push_back(td1);

    TradeData td2{};
    td2.trade_id = 2;
    td2.symbol = "rb2405";
    td2.direction = Direction::SHORT;
    td2.offset = Offset::CLOSE;
    td2.price = 3640.0;
    td2.volume = 10.0;
    td2.commission = 3.6;
    trades.push_back(td2); // +4000

    TradeData td3{};
    td3.trade_id = 3;
    td3.symbol = "rb2405";
    td3.direction = Direction::LONG;
    td3.offset = Offset::OPEN;
    td3.price = 3640.0;
    td3.volume = 10.0;
    td3.commission = 3.6;
    trades.push_back(td3);

    TradeData td4{};
    td4.trade_id = 4;
    td4.symbol = "rb2405";
    td4.direction = Direction::SHORT;
    td4.offset = Offset::CLOSE;
    td4.price = 3620.0;
    td4.volume = 10.0;
    td4.commission = 3.6;
    trades.push_back(td4); // -2000

    std::vector<double> equity = {1000000.0, 1004000.0, 1002000.0};
    auto stats = PerformanceAnalyzer::calculate(1000000.0, equity, trades, 100);

    assert(stats.total_trades == 2);
    assert(stats.winning_trades == 1);
    assert(stats.losing_trades == 1);
    assert(std::abs(stats.win_rate_pct - 50.0) < 1e-4);
    assert(std::abs(stats.profit_factor - 2.0) < 1e-4); // 4000 / 2000 = 2.0

    std::cout << "  -> 开平仓闭环匹配与胜率盈亏比统计通过!\n";
}

void test_gateway_pool_and_follow_trading() {
    std::cout << "[Test 5] 运行 GatewayPool 回报发布与多账户跟单全链路测试...\n";
    MessageBus* bus = message_bus_create("test_bus_follow");
    GatewayPool pool(bus);

    AccountProfile master_prof{"acc_master", "SimNow", AccountRole::MASTER, 1.0, 1000000.0};
    AccountProfile slave_prof{"acc_slave", "Zhongxin", AccountRole::SLAVE, 1.5, 1000000.0};

    bool ok1 = pool.register_account(master_prof);
    bool ok2 = pool.register_account(slave_prof);
    assert(ok1 && ok2);

    pool.connect_all();

    // 总线回报与协程调度都是异步链路, 循环驱动 + 等待从账户委托到达
    static std::atomic<bool> slave_order_received{false};
    static QuantOrderReqMsg slave_req{};

    message_bus_subscribe(bus, "trader/acc_slave/order_req", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantOrderReqMsg)) {
            slave_req = *reinterpret_cast<const QuantOrderReqMsg*>(msg->data);
            slave_order_received = true;
        }
    }, nullptr);

    // 挂载跟单协程任务
    std::vector<CoroFollowTradingTask::SlaveConfig> slaves;
    slaves.push_back({"acc_slave", 1.5});
    auto follow_task = std::make_unique<CoroFollowTradingTask>(bus, "acc_master", slaves);

    flowcoro::rt::RtExecutor ex{{ .pin_cpu = -1 }};
    g_node_exec = &ex; // awaitable 契约: spawn 前必须设置 TLS executor 指针
    ex.spawn(follow_task->run(), "follow_copier");

    // 先驱动一轮调度, 让跟单协程挂起在 BusChannel 订阅上, 再注入成交回报
    ex.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 模拟主账户发生一次成交回报
    TradeData master_trade;
    master_trade.trade_id = 999;
    master_trade.order_id = 888;
    master_trade.symbol = "rb2405";
    master_trade.exchange = "SHFE";
    master_trade.direction = Direction::LONG;
    master_trade.offset = Offset::OPEN;
    master_trade.price = 3620.0;
    master_trade.volume = 10.0;
    master_trade.commission = 3.62;

    // 触发 GatewayPool::on_trade，向 trader/acc_master/trade_rtn 广播 QuantTradeMsg
    pool.on_trade(master_trade);

    // 持续驱动调度器执行协程循环, 直到跟单委托到达 (上限 1s)
    for (int i = 0; i < 500 && !slave_order_received.load(); ++i) {
        ex.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 验证从账户管道确实收到了按 1.5x 权重生成的跟单委托 (10手 * 1.5 = 15手)
    assert(slave_order_received.load());
    assert(std::strcmp(slave_req.symbol, "rb2405") == 0);
    assert(std::abs(slave_req.volume - 15.0) < 1e-4);
    assert(std::abs(slave_req.price - 3620.0) < 1e-4);
    assert(slave_req.direction == static_cast<uint8_t>(Direction::LONG));

    pool.disconnect_all();

    // 销毁顺序契约: 先停协程并让 executor 回收挂起帧 (BusChannel 析构需在总线
    // 存活时反订阅), 再销毁总线 — 否则 ASAN 检出 use-after-free
    follow_task->set_stop();
    ex.shutdown();
    follow_task.reset();
    g_node_exec = nullptr; // 清理 TLS, 避免后续测试读到悬挂指针
    message_bus_destroy(bus);
    std::cout << "  -> GatewayPool 回报发布与从账户 1.5x 自动跟单闭环测试 100% 通过!\n";
}

int main() {
    std::cout.setf(std::ios::unitbuf); // 无缓冲输出, abort 时也能看到日志
    std::cout << "\n=========================================================\n";
    std::cout << "       KunQuant 交易闭环与真实可用性单元测试集            \n";
    std::cout << "=========================================================\n\n";

    test_pod_memory_safety();
    test_matching_engine_depth_and_partial_fill();
    test_position_accounting_and_close_today();
    test_performance_trade_pairing();
    test_gateway_pool_and_follow_trading();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 5 组核心闭环单测 100% 断言通过!              \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
