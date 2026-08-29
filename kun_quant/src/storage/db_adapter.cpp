#include "kun/storage/db_adapter.hpp"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <mutex>
#include <filesystem>

namespace kun {

class SqliteDatabaseAdapter : public IDatabaseAdapter {
public:
    explicit SqliteDatabaseAdapter(DbConfig config) : config_(std::move(config)) {}

    ~SqliteDatabaseAdapter() override {
        disconnect();
    }

    bool connect() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) return true;

        std::filesystem::path p(config_.sqlite_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        int rc = sqlite3_open(config_.sqlite_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "[SqliteAdapter] 无法打开数据库: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }

        // 生产级 SQLite 调优
        sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

        init_schema();
        return true;
    }

    void disconnect() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool is_connected() const override {
        return db_ != nullptr;
    }

    bool begin_transaction() override {
        return execute("BEGIN TRANSACTION;");
    }

    bool commit() override {
        return execute("COMMIT;");
    }

    bool rollback() override {
        return execute("ROLLBACK;");
    }

    bool execute(const std::string& sql) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return false;
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            if (err_msg) {
                std::cerr << "[SqliteAdapter] SQL 执行错误: " << err_msg << " | SQL: " << sql << "\n";
                sqlite3_free(err_msg);
            }
            return false;
        }
        return true;
    }

    bool save_order(const OrderData& order) override {
        std::ostringstream ss;
        ss << "INSERT OR REPLACE INTO orders (order_id, order_ref, symbol, exchange, direction, offset, order_type, price, total_volume, traded_volume, status, update_time_us) "
           << "VALUES (" << order.order_id << ", '" << order.order_ref << "', '" << order.symbol << "', '" << order.exchange << "', "
           << static_cast<int>(order.direction) << ", " << static_cast<int>(order.offset) << ", " << static_cast<int>(order.order_type) << ", "
           << order.price << ", " << order.total_volume << ", " << order.traded_volume << ", " << static_cast<int>(order.status) << ", "
           << order.update_time_us << ");";
        return execute(ss.str());
    }

    bool save_trade(const TradeData& trade) override {
        std::ostringstream ss;
        ss << "INSERT OR REPLACE INTO trades (trade_id, order_id, symbol, exchange, strategy_name, direction, offset, price, volume, commission, trade_time_us) "
           << "VALUES (" << trade.trade_id << ", " << trade.order_id << ", '" << trade.symbol << "', '" << trade.exchange << "', '"
           << trade.strategy_name << "', " << static_cast<int>(trade.direction) << ", " << static_cast<int>(trade.offset) << ", "
           << trade.price << ", " << trade.volume << ", " << trade.commission << ", " << trade.trade_time_us << ");";
        return execute(ss.str());
    }

    bool save_position(const PositionData& pos) override {
        std::ostringstream ss;
        ss << "INSERT OR REPLACE INTO positions (symbol, direction, volume, yd_volume, today_volume, frozen_volume, open_cost, position_pnl, margin) "
           << "VALUES ('" << pos.symbol << "', " << static_cast<int>(pos.direction) << ", " << pos.volume << ", " << pos.yd_volume << ", "
           << pos.today_volume << ", " << pos.frozen << ", " << pos.open_cost << ", " << pos.floating_pnl << ", " << pos.margin << ");";
        return execute(ss.str());
    }

    bool save_account_snapshot(const AccountData& acc) override {
        std::ostringstream ss;
        ss << "INSERT INTO account_snapshots (account_id, balance, available, frozen_margin, margin, floating_pnl, realized_pnl, commission) "
           << "VALUES ('" << acc.account_id << "', " << acc.balance << ", " << acc.available << ", " << acc.frozen_margin << ", "
           << acc.margin << ", " << acc.floating_pnl << ", " << acc.realized_pnl << ", " << acc.commission << ");";
        return execute(ss.str());
    }

    std::vector<OrderData> query_orders(const std::string& account_id, int limit) override {
        (void)account_id;
        std::vector<OrderData> res;
        std::string sql = "SELECT order_id, order_ref, symbol, exchange, direction, offset, order_type, price, total_volume, traded_volume, status, update_time_us FROM orders ORDER BY order_id DESC LIMIT " + std::to_string(limit) + ";";
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return res;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                OrderData o{};
                o.order_id = sqlite3_column_int64(stmt, 0);
                o.order_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                o.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                o.exchange = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                o.direction = static_cast<Direction>(sqlite3_column_int(stmt, 4));
                o.offset = static_cast<Offset>(sqlite3_column_int(stmt, 5));
                o.order_type = static_cast<OrderType>(sqlite3_column_int(stmt, 6));
                o.price = sqlite3_column_double(stmt, 7);
                o.total_volume = sqlite3_column_double(stmt, 8);
                o.traded_volume = sqlite3_column_double(stmt, 9);
                o.status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 10));
                o.update_time_us = sqlite3_column_int64(stmt, 11);
                res.push_back(o);
            }
            sqlite3_finalize(stmt);
        }
        return res;
    }

    std::vector<TradeData> query_trades(const std::string& account_id, int limit) override {
        (void)account_id;
        std::vector<TradeData> res;
        std::string sql = "SELECT trade_id, order_id, symbol, exchange, strategy_name, direction, offset, price, volume, commission, trade_time_us FROM trades ORDER BY trade_id DESC LIMIT " + std::to_string(limit) + ";";

        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return res;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                TradeData t{};
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
                res.push_back(t);
            }
            sqlite3_finalize(stmt);
        }
        return res;
    }

    std::vector<PositionData> query_positions(const std::string& account_id) override {
        (void)account_id;
        std::vector<PositionData> res;
        std::string sql = "SELECT symbol, direction, volume, yd_volume, today_volume, frozen_volume, open_cost, position_pnl, margin FROM positions;";

        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return res;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                PositionData p{};
                p.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                p.direction = static_cast<Direction>(sqlite3_column_int(stmt, 1));
                p.volume = sqlite3_column_double(stmt, 2);
                p.yd_volume = sqlite3_column_double(stmt, 3);
                p.today_volume = sqlite3_column_double(stmt, 4);
                p.frozen = sqlite3_column_double(stmt, 5);
                p.open_cost = sqlite3_column_double(stmt, 6);
                p.floating_pnl = sqlite3_column_double(stmt, 7);
                p.margin = sqlite3_column_double(stmt, 8);
                res.push_back(p);
            }
            sqlite3_finalize(stmt);
        }
        return res;
    }

    AccountData query_latest_account(const std::string& account_id) override {
        AccountData acc{};
        acc.account_id = account_id;
        std::string sql = "SELECT balance, available, frozen_margin, margin, floating_pnl, realized_pnl, commission FROM account_snapshots WHERE account_id = '" + account_id + "' ORDER BY id DESC LIMIT 1;";

        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return acc;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                acc.balance = sqlite3_column_double(stmt, 0);
                acc.available = sqlite3_column_double(stmt, 1);
                acc.frozen_margin = sqlite3_column_double(stmt, 2);
                acc.margin = sqlite3_column_double(stmt, 3);
                acc.floating_pnl = sqlite3_column_double(stmt, 4);
                acc.realized_pnl = sqlite3_column_double(stmt, 5);
                acc.commission = sqlite3_column_double(stmt, 6);
            }
            sqlite3_finalize(stmt);
        }
        return acc;
    }

    int get_schema_version() override {
        int ver = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return 0;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                ver = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        return ver;
    }

    bool run_migration(int target_version, const std::string& migration_sql) override {
        if (!begin_transaction()) return false;
        if (!execute(migration_sql)) {
            rollback();
            return false;
        }
        std::string pragma_sql = "PRAGMA user_version = " + std::to_string(target_version) + ";";
        if (!execute(pragma_sql)) {
            rollback();
            return false;
        }
        return commit();
    }

private:
    void init_schema() {
        execute(R"(
            CREATE TABLE IF NOT EXISTS orders (
                order_id INTEGER PRIMARY KEY,
                order_ref TEXT,
                symbol TEXT NOT NULL,
                exchange TEXT,
                direction INTEGER,
                offset INTEGER,
                order_type INTEGER,
                price REAL,
                total_volume REAL,
                traded_volume REAL,
                status INTEGER,
                update_time_us INTEGER
            );
            CREATE TABLE IF NOT EXISTS trades (
                trade_id INTEGER PRIMARY KEY,
                order_id INTEGER,
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
                symbol TEXT NOT NULL,
                direction INTEGER NOT NULL,
                volume REAL,
                yd_volume REAL,
                today_volume REAL,
                frozen_volume REAL,
                open_cost REAL,
                position_pnl REAL,
                margin REAL,
                PRIMARY KEY(symbol, direction)
            );
            CREATE TABLE IF NOT EXISTS account_snapshots (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                account_id TEXT NOT NULL,
                balance REAL,
                available REAL,
                frozen_margin REAL,
                margin REAL,
                floating_pnl REAL,
                realized_pnl REAL,
                commission REAL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
        )");
    }

    DbConfig config_;
    sqlite3* db_{nullptr};
    std::mutex mutex_;
};

std::unique_ptr<IDatabaseAdapter> create_database_adapter(const DbConfig& config) {
    return std::make_unique<SqliteDatabaseAdapter>(config);
}

} // namespace kun
