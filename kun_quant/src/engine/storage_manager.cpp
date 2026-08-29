#include "kun/engine/storage_manager.hpp"
#include <sqlite3.h>
#include <iostream>
#include <filesystem>
#include <sstream>

namespace kun {

StorageManager::StorageManager(std::string db_path) : db_path_(std::move(db_path)) {}

StorageManager::~StorageManager() {
    close();
}

bool StorageManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return true;

    try {
        std::filesystem::path p(db_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {}

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[StorageManager] Failed to open SQLite DB: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    // 启用 WAL 高性能模式与内存对齐
    execute_sql("PRAGMA journal_mode = WAL;");
    execute_sql("PRAGMA synchronous = NORMAL;");
    execute_sql("PRAGMA temp_store = MEMORY;");

    return create_tables();
}

void StorageManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool StorageManager::execute_sql(const std::string& sql) {
    if (!db_) return false;
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            std::cerr << "[StorageManager] SQL Error: " << err_msg << " (SQL: " << sql << ")\n";
            sqlite3_free(err_msg);
        }
        return false;
    }
    return true;
}

bool StorageManager::create_tables() {
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS orders (
            order_id INTEGER PRIMARY KEY,
            account_id TEXT NOT NULL,
            symbol TEXT NOT NULL,
            exchange TEXT,
            strategy_name TEXT,
            direction INTEGER,
            offset INTEGER,
            status INTEGER,
            price REAL,
            total_volume REAL,
            traded_volume REAL,
            order_ref TEXT,
            update_time_us INTEGER
        );

        CREATE TABLE IF NOT EXISTS trades (
            trade_id INTEGER PRIMARY KEY,
            order_id INTEGER,
            account_id TEXT NOT NULL,
            symbol TEXT NOT NULL,
            exchange TEXT,
            strategy_name TEXT,
            direction INTEGER,
            offset INTEGER,
            price REAL,
            volume REAL,
            commission REAL,
            trade_time_us INTEGER
        );

        CREATE TABLE IF NOT EXISTS positions (
            account_id TEXT NOT NULL,
            symbol TEXT NOT NULL,
            direction INTEGER NOT NULL,
            volume REAL,
            today_volume REAL,
            yd_volume REAL,
            avg_price REAL,
            open_cost REAL,
            margin REAL,
            realized_pnl REAL,
            floating_pnl REAL,
            frozen REAL,
            update_time_us INTEGER,
            PRIMARY KEY (account_id, symbol, direction)
        );

        CREATE TABLE IF NOT EXISTS account_flow (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            account_id TEXT NOT NULL,
            balance REAL,
            available REAL,
            margin REAL,
            frozen_margin REAL,
            commission REAL,
            realized_pnl REAL,
            floating_pnl REAL,
            timestamp_us INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_orders_acc ON orders(account_id, status);
        CREATE INDEX IF NOT EXISTS idx_trades_acc ON trades(account_id, symbol);
        CREATE INDEX IF NOT EXISTS idx_positions_acc ON positions(account_id);

        CREATE TABLE IF NOT EXISTS ticks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            symbol TEXT NOT NULL,
            exchange TEXT,
            last_price REAL,
            bid_price1 REAL,
            ask_price1 REAL,
            bid_volume1 REAL,
            ask_volume1 REAL,
            volume REAL,
            open_interest REAL,
            ts INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS bars (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            symbol TEXT NOT NULL,
            exchange TEXT,
            interval TEXT NOT NULL,
            open REAL,
            high REAL,
            low REAL,
            close REAL,
            volume REAL,
            open_interest REAL,
            ts INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_ticks_sym_ts ON ticks(symbol, ts);
        CREATE INDEX IF NOT EXISTS idx_bars_sym_iv_ts ON bars(symbol, interval, ts);
    )";

    return execute_sql(schema);
}

bool StorageManager::save_order(const OrderData& order, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT OR REPLACE INTO orders (
            order_id, account_id, symbol, exchange, strategy_name, direction, offset, status,
            price, total_volume, traded_volume, order_ref, update_time_us
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, order.order_id);
    sqlite3_bind_text(stmt, 2, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, order.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, order.exchange.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, order.strategy_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, static_cast<int>(order.direction));
    sqlite3_bind_int(stmt, 7, static_cast<int>(order.offset));
    sqlite3_bind_int(stmt, 8, static_cast<int>(order.status));
    sqlite3_bind_double(stmt, 9, order.price);
    sqlite3_bind_double(stmt, 10, order.total_volume);
    sqlite3_bind_double(stmt, 11, order.traded_volume);
    sqlite3_bind_text(stmt, 12, order.order_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 13, order.update_time_us);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool StorageManager::save_trade(const TradeData& trade, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT OR REPLACE INTO trades (
            trade_id, order_id, account_id, symbol, exchange, strategy_name, direction, offset,
            price, volume, commission, trade_time_us
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, trade.trade_id);
    sqlite3_bind_int64(stmt, 2, trade.order_id);
    sqlite3_bind_text(stmt, 3, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, trade.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, trade.exchange.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, trade.strategy_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, static_cast<int>(trade.direction));
    sqlite3_bind_int(stmt, 8, static_cast<int>(trade.offset));
    sqlite3_bind_double(stmt, 9, trade.price);
    sqlite3_bind_double(stmt, 10, trade.volume);
    sqlite3_bind_double(stmt, 11, trade.commission);
    sqlite3_bind_int64(stmt, 12, trade.trade_time_us);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool StorageManager::save_position(const PositionData& pos, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT OR REPLACE INTO positions (
            account_id, symbol, direction, volume, today_volume, yd_volume, avg_price,
            open_cost, margin, realized_pnl, floating_pnl, frozen, update_time_us
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pos.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, static_cast<int>(pos.direction));
    sqlite3_bind_double(stmt, 4, pos.volume);
    sqlite3_bind_double(stmt, 5, pos.today_volume);
    sqlite3_bind_double(stmt, 6, pos.yd_volume);
    sqlite3_bind_double(stmt, 7, pos.avg_price);
    sqlite3_bind_double(stmt, 8, pos.open_cost);
    sqlite3_bind_double(stmt, 9, pos.margin);
    sqlite3_bind_double(stmt, 10, pos.realized_pnl);
    sqlite3_bind_double(stmt, 11, pos.floating_pnl);
    sqlite3_bind_double(stmt, 12, pos.frozen);
    sqlite3_bind_int64(stmt, 13, 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool StorageManager::save_account(const AccountData& acc, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT INTO account_flow (
            account_id, balance, available, margin, frozen_margin, commission, realized_pnl, floating_pnl, timestamp_us
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, acc.balance);
    sqlite3_bind_double(stmt, 3, acc.available);
    sqlite3_bind_double(stmt, 4, acc.margin);
    sqlite3_bind_double(stmt, 5, acc.frozen_margin);
    sqlite3_bind_double(stmt, 6, acc.commission);
    sqlite3_bind_double(stmt, 7, acc.realized_pnl);
    sqlite3_bind_double(stmt, 8, acc.floating_pnl);
    sqlite3_bind_int64(stmt, 9, 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool StorageManager::save_ticks_batch(const std::vector<TickData>& ticks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || ticks.empty()) return false;

    const char* sql = R"(
        INSERT INTO ticks (
            symbol, exchange, last_price, bid_price1, ask_price1,
            bid_volume1, ask_volume1, volume, open_interest, ts
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    execute_sql("BEGIN TRANSACTION;");
    for (const auto& t : ticks) {
        sqlite3_bind_text(stmt, 1, t.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, t.exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, t.last_price);
        sqlite3_bind_double(stmt, 4, t.bid_price[0]);
        sqlite3_bind_double(stmt, 5, t.ask_price[0]);
        sqlite3_bind_double(stmt, 6, t.bid_volume[0]);
        sqlite3_bind_double(stmt, 7, t.ask_volume[0]);
        sqlite3_bind_double(stmt, 8, t.volume);
        sqlite3_bind_double(stmt, 9, t.open_interest);
        sqlite3_bind_int64(stmt, 10, t.timestamp_us);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    const bool ok = execute_sql("COMMIT;");
    sqlite3_finalize(stmt);
    return ok;
}

bool StorageManager::save_bars_batch(const std::vector<BarData>& bars) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || bars.empty()) return false;

    const char* sql = R"(
        INSERT INTO bars (
            symbol, exchange, interval, open, high, low, close, volume, open_interest, ts
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    execute_sql("BEGIN TRANSACTION;");
    for (const auto& b : bars) {
        sqlite3_bind_text(stmt, 1, b.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, b.exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, b.interval.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, b.open_price);
        sqlite3_bind_double(stmt, 5, b.high_price);
        sqlite3_bind_double(stmt, 6, b.low_price);
        sqlite3_bind_double(stmt, 7, b.close_price);
        sqlite3_bind_double(stmt, 8, b.volume);
        sqlite3_bind_double(stmt, 9, b.open_interest);
        sqlite3_bind_int64(stmt, 10, b.timestamp_us);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    const bool ok = execute_sql("COMMIT;");
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<OrderData> StorageManager::load_active_orders(const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderData> results;
    if (!db_) return results;

    const char* sql = "SELECT order_id, symbol, exchange, strategy_name, direction, offset, status, price, total_volume, traded_volume, order_ref, update_time_us FROM orders WHERE account_id = ? AND status IN (0, 1);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OrderData o;
        o.order_id = sqlite3_column_int64(stmt, 0);
        o.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        o.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        o.strategy_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        o.direction = static_cast<Direction>(sqlite3_column_int(stmt, 4));
        o.offset = static_cast<Offset>(sqlite3_column_int(stmt, 5));
        o.status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 6));
        o.price = sqlite3_column_double(stmt, 7);
        o.total_volume = sqlite3_column_double(stmt, 8);
        o.traded_volume = sqlite3_column_double(stmt, 9);
        o.order_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        o.update_time_us = sqlite3_column_int64(stmt, 11);
        results.push_back(o);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<TradeData> StorageManager::load_trades(const std::string& account_id, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TradeData> results;
    if (!db_) return results;

    const char* sql = "SELECT trade_id, order_id, symbol, exchange, strategy_name, direction, offset, price, volume, commission, trade_time_us FROM trades WHERE account_id = ? ORDER BY trade_time_us DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TradeData t;
        t.trade_id = sqlite3_column_int64(stmt, 0);
        t.order_id = sqlite3_column_int64(stmt, 1);
        t.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        t.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        t.strategy_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        t.direction = static_cast<Direction>(sqlite3_column_int(stmt, 5));
        t.offset = static_cast<Offset>(sqlite3_column_int(stmt, 6));
        t.price = sqlite3_column_double(stmt, 7);
        t.volume = sqlite3_column_double(stmt, 8);
        t.commission = sqlite3_column_double(stmt, 9);
        t.trade_time_us = sqlite3_column_int64(stmt, 10);
        results.push_back(t);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<PositionData> StorageManager::load_positions(const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PositionData> results;
    if (!db_) return results;

    const char* sql = "SELECT symbol, direction, volume, today_volume, yd_volume, avg_price, open_cost, margin, realized_pnl, floating_pnl, frozen FROM positions WHERE account_id = ? AND volume > 0;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PositionData p;
        p.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        p.direction = static_cast<Direction>(sqlite3_column_int(stmt, 1));
        p.volume = sqlite3_column_double(stmt, 2);
        p.today_volume = sqlite3_column_double(stmt, 3);
        p.yd_volume = sqlite3_column_double(stmt, 4);
        p.avg_price = sqlite3_column_double(stmt, 5);
        p.open_cost = sqlite3_column_double(stmt, 6);
        p.margin = sqlite3_column_double(stmt, 7);
        p.realized_pnl = sqlite3_column_double(stmt, 8);
        p.floating_pnl = sqlite3_column_double(stmt, 9);
        p.frozen = sqlite3_column_double(stmt, 10);
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool StorageManager::load_latest_account(const std::string& account_id, AccountData& out_acc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT balance, available, margin, frozen_margin, commission, realized_pnl, floating_pnl FROM account_flow WHERE account_id = ? ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_acc.account_id = account_id;
        out_acc.balance = sqlite3_column_double(stmt, 0);
        out_acc.available = sqlite3_column_double(stmt, 1);
        out_acc.margin = sqlite3_column_double(stmt, 2);
        out_acc.frozen_margin = sqlite3_column_double(stmt, 3);
        out_acc.commission = sqlite3_column_double(stmt, 4);
        out_acc.realized_pnl = sqlite3_column_double(stmt, 5);
        out_acc.floating_pnl = sqlite3_column_double(stmt, 6);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

} // namespace kun
