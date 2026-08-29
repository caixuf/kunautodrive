#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include "kun/core/types.hpp"
#include "kun/gateway/ctp_gateway.hpp"
#include "message_bus.h"

using namespace kun;

void test_ctp_gateway_lifecycle() {
    std::cout << "[Test 1] 运行 CTP 实盘网关连接、认证与登录生命周期测试...\n";
    MessageBus* bus = message_bus_create("test_bus_ctp_lifecycle");
    assert(bus != nullptr);

    CtpAccountConfig cfg{
        "9999", "089231", "Pass123", "client_kun_v1", "AUTH_CODE_888",
        "tcp://180.168.146.187:10203", "tcp://180.168.146.187:10213"
    };

    CtpGateway gw(bus, "acc_ctp_01", cfg);
    assert(!gw.is_connected());

    bool ok = gw.connect();
    assert(ok);
    assert(gw.is_connected());

    gw.disconnect();
    assert(!gw.is_connected());

    message_bus_destroy(bus);
    std::cout << "  -> CTP 网关标准连接与断开生命周期测试通过!\n";
}

void test_ctp_gateway_rate_limiter() {
    std::cout << "[Test 2] 运行 CTP 柜台 1 秒最多 1 次查询限速保护测试...\n";
    MessageBus* bus = message_bus_create("test_bus_ctp_ratelimit");
    CtpAccountConfig cfg{"9999", "089231", "Pass123", "app1", "auth1", "tcp://front", "tcp://front_md"};
    CtpGateway gw(bus, "acc_ctp_01", cfg);
    gw.connect();

    // 第一次查询应成功
    bool q1 = gw.query_account_safe();
    assert(q1);

    // 毫秒级立刻发第二次查询，必须被 RateLimiter 拦截拒发
    bool q2 = gw.query_positions_safe();
    assert(!q2);

    // 等待 1.05 秒后再次查询，应恢复正常放行
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    bool q3 = gw.query_positions_safe();
    assert(q3);

    gw.disconnect();
    message_bus_destroy(bus);
    std::cout << "  -> CTP 1秒1次合规限速保护测试 100% 通过! 有效防止高频查询封号。\n";
}

void test_ctp_trade_deduplication() {
    std::cout << "[Test 3] 运行 CTP 断线重连重复成交回报去重测试...\n";
    MessageBus* bus = message_bus_create("test_bus_ctp_dedup");
    CtpAccountConfig cfg{"9999", "089231", "Pass123", "app1", "auth1", "tcp://front", "tcp://front_md"};
    CtpGateway gw(bus, "acc_ctp_01", cfg);
    gw.connect();

    static int bus_recv_trades = 0;
    message_bus_subscribe(bus, "trader/acc_ctp_01/trade_rtn", [](const Message* /*msg*/, void* /*ud*/) {
        bus_recv_trades++;
    }, nullptr);

    QuantTradeMsg t1{};
    t1.trade_id = 998801;
    t1.order_id = 1001;
    std::strncpy(t1.symbol, "rb2405", sizeof(t1.symbol) - 1);
    t1.price = 3620.0;
    t1.volume = 5.0;

    // 1. 发送第 1 笔真实成交
    gw.on_ctp_trade_raw(t1);
    assert(bus_recv_trades == 1);

    // 2. 重复推送完全相同的成交帧 (模拟 CTP 断线重连重传)
    gw.on_ctp_trade_raw(t1);
    // 验证总线没有重复接收，去重计数增加
    assert(bus_recv_trades == 1);
    assert(gw.get_duplicate_trades_filtered() == 1);

    // 3. 推送第 2 笔新成交
    QuantTradeMsg t2 = t1;
    t2.trade_id = 998802;
    gw.on_ctp_trade_raw(t2);
    assert(bus_recv_trades == 2);
    assert(gw.get_duplicate_trades_filtered() == 1);

    gw.disconnect();
    message_bus_destroy(bus);
    std::cout << "  -> CTP 重复成交去重保护测试通过! 成功拦截重传脏数据。\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "  KunQuant M7: 生产级 CTP 实盘网关、流控与去重单测集     \n";
    std::cout << "=========================================================\n\n";

    test_ctp_gateway_lifecycle();
    test_ctp_gateway_rate_limiter();
    test_ctp_trade_deduplication();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 M7 CTP 实盘网关单测 100% 断言通过!           \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
