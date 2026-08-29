#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include "kun/core/types.hpp"
#include "kun/market/market_fusion_node.hpp"
#include "kun/market/sina_fetcher.hpp"
#include "message_bus.h"

using namespace kun;

void test_multi_source_weighted_fusion() {
    std::cout << "[Test 1] 运行多源行情加权融合与最优盘口提取测试...\n";
    MessageBus* bus = message_bus_create("test_bus_fusion");
    assert(bus != nullptr);

    MarketFusionNode fusion(bus, 0.03); // 3% 离群过滤门限
    fusion.start();

    // 订阅最终融合真值管道
    static bool fused_recv = false;
    static QuantTickMsg true_tick{};

    message_bus_subscribe(bus, "market/tick/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            true_tick = *reinterpret_cast<const QuantTickMsg*>(msg->data);
            fused_recv = true;
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

    assert(fused_recv);
    // 验证均价融合: (3620 + 3622) / 2 = 3621.0
    assert(std::abs(true_tick.last_price - 3621.0) < 1e-4);
    // 验证最优盘口: 最高买价=3620.0, 最低卖价=3621.0
    assert(std::abs(true_tick.bid_price1 - 3620.0) < 1e-4);
    assert(std::abs(true_tick.ask_price1 - 3621.0) < 1e-4);

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
    
    static bool received_sina = false;
    static QuantTickMsg sina_tick{};

    message_bus_subscribe(bus, "market/source/sina/rb2405", [](const Message* msg, void* /*ud*/) {
        if (msg && msg->data_size >= sizeof(QuantTickMsg)) {
            sina_tick = *reinterpret_cast<const QuantTickMsg*>(msg->data);
            received_sina = true;
        }
    }, nullptr);

    SinaMarketFetcher fetcher(bus, {"rb2405", "cu2405", "ag2405"});
    bool ok = fetcher.fetch_once();
    assert(ok);

    if (received_sina) {
        std::cout << "  [实时行情] 成功抓取真实螺纹钢行情: 价格=" << sina_tick.last_price
                  << " 买1=" << sina_tick.bid_price1 << " 卖1=" << sina_tick.ask_price1
                  << " 成交量=" << sina_tick.volume << " 持仓量=" << sina_tick.open_interest << "\n";
        assert(sina_tick.last_price > 1000.0 && sina_tick.last_price < 10000.0);
    }

    message_bus_destroy(bus);
    std::cout << "  -> 新浪真实行情抓取与 POD 解析测试通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "       KunQuant 多源数据采集与行情融合单测集              \n";
    std::cout << "=========================================================\n\n";

    test_multi_source_weighted_fusion();
    test_bad_tick_outlier_filtering();
    test_live_sina_fetcher();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部行情采集与融合单测 100% 断言通过!             \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
