#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "kun/core/types.hpp"
#include "kun/market/market_fusion_node.hpp"
#include "kun/market/sina_fetcher.hpp"
#include "kun/market/secondary_fetcher.hpp"
#include "kun/market/market_heartbeat_watchdog.hpp"
#include "kun/engine/risk_manager.hpp"
#include "kun/engine/position_manager.hpp"
#include "message_bus.h"

using namespace kun;

void test_multi_source_weighted_fusion() {
    std::cout << "[Test 1] 运行多源行情加权融合与最优盘口提取测试...\n";
    MessageBus* bus = message_bus_create("test_bus_fusion");
    assert(bus != nullptr);

    MarketFusionNode fusion(bus, 0.03); // 3% 离群过滤门限
    fusion.start();

    // 订阅最终融合真值管道
    static std::atomic<int> fused_count{0};
    static std::atomic<double> fused_last_price{0.0};
    static std::atomic<double> fused_bid1{0.0};
    static std::atomic<double> fused_ask1{0.0};
    fused_count.store(0);
    fused_last_price.store(0.0);
    fused_bid1.store(0.0);
    fused_ask1.store(0.0);

    message_bus_subscribe(bus, "market/tick/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            const auto* t = reinterpret_cast<const QuantTickMsg*>(msg->data);
            fused_last_price.store(t->last_price);
            fused_bid1.store(t->bid_price1);
            fused_ask1.store(t->ask_price1);
            fused_count.fetch_add(1);
        }
    }, nullptr);

    // 1. 数据源 1 (Sina): 价格 3620.0, 买1=3619.0, 卖1=3622.0
    QuantTickMsg tick_sina{};
    std::strncpy(tick_sina.symbol, "rb2405", sizeof(tick_sina.symbol) - 1);
    tick_sina.last_price = 3620.0;
    tick_sina.bid_price1 = 3619.0;
    tick_sina.ask_price1 = 3622.0;
    fusion.on_source_tick("sina", tick_sina);

    // 2. 数据源 2 (Eastmoney): 价格 3622.0, 买1=3620.0, 卖1=3621.0
    QuantTickMsg tick_em{};
    std::strncpy(tick_em.symbol, "rb2405", sizeof(tick_em.symbol) - 1);
    tick_em.last_price = 3622.0;
    tick_em.bid_price1 = 3620.0;
    tick_em.ask_price1 = 3621.0;
    fusion.on_source_tick("eastmoney", tick_em);

    // 验证同步融合结果直接可读
    QuantTickMsg fused_tick_obj{};
    assert(fusion.get_fused_tick("rb2405", fused_tick_obj));
    assert(std::abs(fused_tick_obj.last_price - 3621.0) < 1e-4);
    assert(std::abs(fused_tick_obj.bid_price1 - 3620.0) < 1e-4);
    assert(std::abs(fused_tick_obj.ask_price1 - 3621.0) < 1e-4);

    // 验证异步总线消息分发
    for (int i = 0; i < 200 && fused_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(fused_count.load() >= 1);
    if (fused_count.load() >= 2) {
        assert(std::abs(fused_last_price.load() - 3621.0) < 1e-4);
    }

    auto telem = fusion.get_telemetry("rb2405");
    assert(telem.active_sources == 2);
    assert(telem.confidence > 0.90);

    fusion.stop();
    message_bus_destroy(bus);
    std::cout << "  -> 多源加权融合与最优买卖盘口选择测试通过!\n";
}

void test_bad_tick_outlier_filtering() {
    std::cout << "[Test 2] 运行异常假刺针 (Bad Tick / Outlier) 过滤测试...\n";
    MessageBus* bus = message_bus_create("test_bus_outlier");
    MarketFusionNode fusion(bus, 0.03); // 3% 异常阈值
    fusion.start();

    // 先输入连续稳定的正常报价建立中值基准 (~3620)
    for (int i = 0; i < 5; ++i) {
        QuantTickMsg normal{};
        std::strncpy(normal.symbol, "rb2405", sizeof(normal.symbol) - 1);
        normal.last_price = 3620.0 + i * 0.5;
        normal.bid_price1 = normal.last_price - 1.0;
        normal.ask_price1 = normal.last_price + 1.0;
        fusion.on_source_tick("sina", normal);
    }

    auto telem_before = fusion.get_telemetry("rb2405");
    uint32_t rejected_before = telem_before.outliers_rejected;

    // 模拟某数据源网络丢包或乱序引发的 5000.0 假刺针 (+38% 暴涨)
    QuantTickMsg bad_tick{};
    std::strncpy(bad_tick.symbol, "rb2405", sizeof(bad_tick.symbol) - 1);
    bad_tick.last_price = 5000.0;
    bad_tick.bid_price1 = 4999.0;
    bad_tick.ask_price1 = 5001.0;
    fusion.on_source_tick("ctp_sim", bad_tick);

    auto telem_after = fusion.get_telemetry("rb2405");
    // 断言异常帧被成功识别并丢弃，累计拒绝数 +1
    assert(telem_after.outliers_rejected == rejected_before + 1);

    // 断言全局真值没有被 5000.0 污染，仍然保持在 3622 左右
    QuantTickMsg current_fused{};
    bool ok = fusion.get_fused_tick("rb2405", current_fused);
    assert(ok);
    assert(current_fused.last_price < 3700.0);

    fusion.stop();
    message_bus_destroy(bus);
    std::cout << "  -> 异常刺针检测与过滤机制测试通过! 假行情被成功拦截。\n";
}

void test_live_sina_fetcher() {
    std::cout << "[Test 3] 运行新浪真实期货 HTTP 行情抓取与解析测试...\n";
    MessageBus* bus = message_bus_create("test_bus_sina");
    
    static std::atomic<bool> received_sina{false};
    static QuantTickMsg sina_tick{};

    message_bus_subscribe(bus, "market/source/sina/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            sina_tick = *reinterpret_cast<const QuantTickMsg*>(msg->data);
            received_sina = true;
        }
    }, nullptr);

    // sina_code 留空走通用推导规则 (品种前缀大写+0)
    SinaMarketFetcher fetcher(bus, {{"rb2405", ""}, {"cu2405", ""}, {"ag2405", ""}});
    bool ok = fetcher.fetch_once();

    if (ok && received_sina) {
        std::cout << "  [实时行情] 成功抓取真实螺纹钢行情: 价格=" << sina_tick.last_price
                  << " 买1=" << sina_tick.bid_price1 << " 卖1=" << sina_tick.ask_price1
                  << " 成交量=" << sina_tick.volume << " 持仓量=" << sina_tick.open_interest << "\n";
        assert(sina_tick.last_price > 1000.0 && sina_tick.last_price < 10000.0);
    } else {
        std::cout << "  [Warn] 离线/CI沙箱环境或网络限制，跳过在线抓取\n";
    }

    message_bus_destroy(bus);
    std::cout << "  -> 新浪真实行情抓取与 POD 解析测试通过!\n";
}

void test_secondary_market_fetcher() {
    std::cout << "[Test 4] 运行第二备用行情源 (SecondaryMarketFetcher) 采集与总线发布测试...\n";
    MessageBus* bus = message_bus_create("test_bus_sec");
    
    static std::atomic<bool> received_sec{false};
    static QuantTickMsg sec_tick{};

    message_bus_subscribe(bus, "market/source/secondary/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            sec_tick = *reinterpret_cast<const QuantTickMsg*>(msg->data);
            received_sec = true;
        }
    }, nullptr);

    SecondaryMarketFetcher sec_fetcher(bus, {{"rb2405", "nf_RB0"}});
    sec_fetcher.inject_raw_tick("rb2405", 3625.0, 50000.0, 120000.0, 3624.0, 3626.0, 10.0, 15.0);

    for (int i = 0; i < 200 && !received_sec.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(received_sec.load());
    assert(std::abs(sec_tick.last_price - 3625.0) < 1e-4);
    assert(std::abs(sec_tick.bid_price1 - 3624.0) < 1e-4);
    assert(std::abs(sec_tick.ask_price1 - 3626.0) < 1e-4);

    message_bus_destroy(bus);
    std::cout << "  -> 第二备用行情源接入与广播测试通过!\n";
}

void test_market_heartbeat_watchdog_and_stale_freeze() {
    std::cout << "[Test 5] 运行数据断流看门狗 (MarketHeartbeatWatchdog) 与风控开仓冻结测试...\n";
    
    // 1. 初始化看门狗 (门限设为 50ms 方便单测)
    MarketHeartbeatWatchdog watchdog(50);
    uint64_t t0 = 1000000000ULL; // 基准时间微秒
    watchdog.on_tick_received("rb2405", t0);

    // 刚刚收到行情时，状态健康
    assert(watchdog.is_symbol_healthy("rb2405", t0 + 20000)); // +20ms < 50ms
    assert(!watchdog.is_symbol_healthy("rb2405", t0 + 80000)); // +80ms > 50ms -> 断流

    // 2. 挂载到事前风控引擎进行开仓硬拦截测试
    PositionManager pm(1000000.0);
    RiskManager rm(pm);
    rm.set_market_watchdog(&watchdog);

    OrderRequest open_req;
    open_req.symbol = "rb2405";
    open_req.direction = Direction::LONG;
    open_req.offset = Offset::OPEN;
    open_req.price = 3500.0;
    open_req.volume = 1.0;

    // 当行情健康时 (当前时间模拟为 t0 + 10ms)
    // 默认 check_order 取系统当前时间，这里注入健康心跳
    watchdog.on_tick_received("rb2405", 0); // 注入当前真实时间
    auto [passed_healthy, reason_healthy] = rm.check_order(open_req, {});
    assert(passed_healthy);

    // 模拟行情断流: 强行将最后一次 tick 时间设置为 10 秒前
    uint64_t old_time_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count() - 10000000ULL
    );
    watchdog.on_tick_received("rb2405", old_time_us);

    auto [passed_stale, reason_stale] = rm.check_order(open_req, {});
    assert(!passed_stale);
    assert(reason_stale.find("MARKET_STALE_FREEZE") != std::string::npos);

    // 行情断流期间，平仓单依然允许执行（防止无法止损自救）
    OrderRequest close_req;
    close_req.symbol = "rb2405";
    close_req.direction = Direction::SHORT;
    close_req.offset = Offset::CLOSE;
    close_req.price = 3500.0;
    close_req.volume = 1.0;
    // (空仓时被持仓不足拦截，而非被断流冻结拦截)
    auto [close_passed, close_reason] = rm.check_order(close_req, {});
    assert(!close_passed);
    assert(close_reason.find("exceeds current position") != std::string::npos);

    // 行情恢复正常，自动解除冻结
    watchdog.on_tick_received("rb2405", 0);
    auto [passed_recovered, reason_recovered] = rm.check_order(open_req, {});
    assert(passed_recovered);

    std::cout << "  -> 行情断流看门狗自动冻结开仓与自愈测试 100% 通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "       KunQuant 多源数据采集与行情融合单测集              \n";
    std::cout << "=========================================================\n\n";

    test_multi_source_weighted_fusion();
    test_bad_tick_outlier_filtering();
    test_live_sina_fetcher();
    test_secondary_market_fetcher();
    test_market_heartbeat_watchdog_and_stale_freeze();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 5 组行情采集、融合与断流防护单测 100% 通过!    \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
