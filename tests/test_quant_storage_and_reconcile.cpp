#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <filesystem>
#include <sqlite3.h>
#include "kun/core/types.hpp"
#include "kun/engine/storage_manager.hpp"
#include "kun/engine/reconciler.hpp"
#include "kun/core/config_loader.hpp"

using namespace kun;

void test_sqlite_storage_and_recovery() {
    std::cout << "[Test 1] 运行 SQLite (WAL 模式) 订单/成交流水与持仓快照持久化测试...\n";
    std::string test_db = "data/test_storage.db";
    std::filesystem::remove(test_db);

    {
        StorageManager sm(test_db);
        bool ok = sm.init();
        assert(ok);

        // 1. 写入订单
        OrderData o1{};
        o1.order_id = 1001;
        o1.symbol = "rb2405";
        o1.exchange = "SHFE";
        o1.strategy_name = "DualMA";
        o1.direction = Direction::LONG;
        o1.offset = Offset::OPEN;
        o1.status = OrderStatus::PARTIALLY_FILLED;
        o1.price = 3620.0;
        o1.total_volume = 10.0;
        o1.traded_volume = 4.0;
        o1.order_ref = "REF_1001";
        assert(sm.save_order(o1, "acc_01"));

        // 2. 写入成交
        TradeData t1{};
        t1.trade_id = 5001;
        t1.order_id = 1001;
        t1.symbol = "rb2405";
        t1.exchange = "SHFE";
        t1.strategy_name = "DualMA";
        t1.direction = Direction::LONG;
        t1.offset = Offset::OPEN;
        t1.price = 3620.0;
        t1.volume = 4.0;
        t1.commission = 1.45;
        t1.trade_time_us = 1700000000;
        assert(sm.save_trade(t1, "acc_01"));

        // 3. 写入持仓快照
        PositionData p1{};
        p1.symbol = "rb2405";
        p1.direction = Direction::LONG;
        p1.volume = 4.0;
        p1.today_volume = 4.0;
        p1.yd_volume = 0.0;
        p1.avg_price = 3620.0;
        p1.margin = 1448.0;
        p1.realized_pnl = 0.0;
        p1.floating_pnl = 120.0;
        p1.frozen = 0.0;
        assert(sm.save_position(p1, "acc_01"));

        // 4. 写入资金流水
        AccountData acc{};
        acc.account_id = "acc_01";
        acc.balance = 1000120.0;
        acc.available = 998672.0;
        acc.margin = 1448.0;
        acc.commission = 1.45;
        acc.realized_pnl = 0.0;
        acc.floating_pnl = 120.0;
        assert(sm.save_account(acc, "acc_01"));

        sm.close();
    }

    // 模拟进程重启恢复 (Reopen DB)
    {
        StorageManager sm(test_db);
        bool ok = sm.init();
        assert(ok);

        auto active_orders = sm.load_active_orders("acc_01");
        assert(active_orders.size() == 1);
        assert(active_orders[0].order_id == 1001);
        assert(active_orders[0].status == OrderStatus::PARTIALLY_FILLED);

        auto trades = sm.load_trades("acc_01");
        assert(trades.size() == 1);
        assert(trades[0].trade_id == 5001);
        assert(std::abs(trades[0].volume - 4.0) < 1e-4);

        auto positions = sm.load_positions("acc_01");
        assert(positions.size() == 1);
        assert(positions[0].symbol == "rb2405");
        assert(std::abs(positions[0].volume - 4.0) < 1e-4);

        AccountData rec_acc{};
        assert(sm.load_latest_account("acc_01", rec_acc));
        assert(std::abs(rec_acc.balance - 1000120.0) < 1e-4);

        sm.close();
    }

    std::filesystem::remove(test_db);
    std::cout << "  -> SQLite 订单/成交/持仓/资金持久化与断电恢复测试通过!\n";
}

void test_settlement_reconciler_matched() {
    std::cout << "[Test 2] 运行柜台结算单 0 误差自动对账测试...\n";
    std::vector<TradeData> local_trades;
    TradeData t1{};
    t1.trade_id = 101;
    t1.symbol = "rb2405";
    t1.direction = Direction::LONG;
    t1.offset = Offset::OPEN;
    t1.price = 3600.0;
    t1.volume = 10.0;
    t1.commission = 3.60;
    local_trades.push_back(t1);

    std::vector<PositionData> local_positions;
    PositionData p1{};
    p1.symbol = "rb2405";
    p1.direction = Direction::LONG;
    p1.volume = 10.0;
    p1.avg_price = 3600.0;
    p1.margin = 3600.0;
    local_positions.push_back(p1);

    std::vector<SettlementTradeRecord> counter_trades = {
        {"101", "rb2405", Direction::LONG, Offset::OPEN, 3600.0, 10.0, 3.60}
    };
    std::vector<SettlementPositionRecord> counter_positions = {
        {"rb2405", Direction::LONG, 10.0, 3600.0, 3600.0}
    };

    auto report = SettlementReconciler::reconcile(
        "acc_01", "2026-08-30",
        local_trades, local_positions,
        counter_trades, counter_positions,
        0.0, 3.60
    );

    assert(report.is_matched);
    assert(report.discrepancies.empty());
    std::cout << "  -> 柜台结算单自动对账 100% 一致校验通过!\n";
}

void test_settlement_reconciler_discrepancy_alert() {
    std::cout << "[Test 3] 运行柜台结算单差异检测与告警测试...\n";
    std::vector<TradeData> local_trades;
    TradeData t1{};
    t1.trade_id = 101;
    t1.symbol = "rb2405";
    t1.volume = 10.0;
    t1.commission = 3.60;
    local_trades.push_back(t1);

    std::vector<PositionData> local_positions;
    PositionData p1{};
    p1.symbol = "rb2405";
    p1.direction = Direction::LONG;
    p1.volume = 10.0; // 本地记 10 手
    local_positions.push_back(p1);

    // 柜台少 2 手 (只有 8 手)，且手续费多收了 5 元
    std::vector<SettlementTradeRecord> counter_trades = {
        {"101", "rb2405", Direction::LONG, Offset::OPEN, 3600.0, 8.0, 8.60}
    };
    std::vector<SettlementPositionRecord> counter_positions = {
        {"rb2405", Direction::LONG, 8.0, 3600.0, 2880.0}
    };

    auto report = SettlementReconciler::reconcile(
        "acc_01", "2026-08-30",
        local_trades, local_positions,
        counter_trades, counter_positions,
        0.0, 8.60
    );

    assert(!report.is_matched);
    assert(report.discrepancies.size() >= 2);
    std::cout << "  -> 成功捕获持仓差异与手续费偏差告警!\n";
}

void test_config_loader() {
    std::cout << "[Test 4] 运行 quant_config.json 配置文件加载与解耦测试...\n";
    QuantAppConfig cfg;
    bool ok = ConfigLoader::load_from_file("kun_quant/config/quant_config.json", cfg);
    assert(ok);
    assert(cfg.server.port == 8900);
    assert(cfg.accounts.size() >= 3);
    assert(cfg.symbols.size() >= 3);
    std::cout << "  -> 配置文件加载与解耦测试通过!\n";
}

void test_market_data_batch_storage() {
    std::cout << "[Test 5] 运行行情批量落盘 (ticks/bars 表, 单事务) 测试...\n";
    std::string test_db = "data/test_market_storage.db";
    std::filesystem::remove(test_db);
    std::filesystem::remove(test_db + "-wal");
    std::filesystem::remove(test_db + "-shm");

    StorageManager sm(test_db);
    assert(sm.init());

    // 批量写入 3000 条 tick (超过 TickRecorder 默认 500 批量阈值的 6 倍)
    std::vector<TickData> ticks;
    ticks.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        TickData t{};
        t.symbol = "au2412";
        t.exchange = "SHFE";
        t.timestamp_us = 1788000000000000LL + static_cast<int64_t>(i) * 200000; // 200ms 一帧
        t.last_price = 568.0 + i * 0.01;
        t.bid_price[0] = t.last_price - 0.02;
        t.ask_price[0] = t.last_price + 0.02;
        t.bid_volume[0] = 120.0;
        t.ask_volume[0] = 150.0;
        t.volume = 1000.0 + i;
        t.open_interest = 200000.0;
        ticks.push_back(t);
    }
    assert(sm.save_ticks_batch(ticks));

    // 批量写入 K 线
    std::vector<BarData> bars;
    bars.reserve(120);
    for (int i = 0; i < 120; ++i) {
        BarData b{};
        b.symbol = "au2412";
        b.exchange = "SHFE";
        b.interval = "1m";
        b.timestamp_us = 1788000000000000LL + static_cast<int64_t>(i) * 60000000;
        b.open_price = 568.0;
        b.high_price = 570.0;
        b.low_price = 567.0;
        b.close_price = 569.0 + i * 0.02;
        b.volume = 5000.0 + i;
        b.open_interest = 200000.0;
        bars.push_back(b);
    }
    assert(sm.save_bars_batch(bars));
    sm.close();

    // 重开校验行数与末条内容
    {
        sqlite3* db = nullptr;
        assert(sqlite3_open(test_db.c_str(), &db) == SQLITE_OK);
        sqlite3_stmt* stmt = nullptr;

        assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM ticks WHERE symbol='au2412';", -1, &stmt, nullptr) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == 3000);
        sqlite3_finalize(stmt);

        assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM bars WHERE symbol='au2412' AND interval='1m';", -1, &stmt, nullptr) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == 120);
        sqlite3_finalize(stmt);

        assert(sqlite3_prepare_v2(db, "SELECT last_price, ts FROM ticks WHERE symbol='au2412' ORDER BY ts DESC LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(std::abs(sqlite3_column_double(stmt, 0) - (568.0 + 2999 * 0.01)) < 1e-6);
        sqlite3_finalize(stmt);

        sqlite3_close(db);
    }

    std::filesystem::remove(test_db);
    std::filesystem::remove(test_db + "-wal");
    std::filesystem::remove(test_db + "-shm");
    std::cout << "  -> 3000 tick + 120 bar 批量单事务落盘与重启校验通过!\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << "  KunQuant M3: SQLite持久化、对账与配置化单元测试集      \n";
    std::cout << "=========================================================\n\n";

    test_sqlite_storage_and_recovery();
    test_settlement_reconciler_matched();
    test_settlement_reconciler_discrepancy_alert();
    test_config_loader();
    test_market_data_batch_storage();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部 M3 持久化与对账单测 100% 断言通过!           \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
