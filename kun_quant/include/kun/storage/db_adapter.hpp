#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>

namespace kun {

enum class DatabaseType {
    SQLITE_WAL = 0,
    POSTGRESQL = 1,
    DUCKDB_PARQUET = 2
};

struct DbConfig {
    DatabaseType type{DatabaseType::SQLITE_WAL};
    std::string host{"127.0.0.1"};
    int port{5432};
    std::string database{"kun_quant"};
    std::string user{"postgres"};
    std::string password{""};
    std::string sqlite_path{"data/kun_quant.db"};
    std::string parquet_dir{"data/parquet"};
    int pool_size{4};
    int max_retries{3};
};

/**
 * @brief 企业级存储适配器抽象基类 (IDatabaseAdapter)
 * 解耦存储引擎，支持在 SQLite WAL、云端 PostgreSQL 与 DuckDB/Parquet 列存之间无损迁移
 */
class IDatabaseAdapter {
public:
    virtual ~IDatabaseAdapter() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // 事务支持
    virtual bool begin_transaction() = 0;
    virtual bool commit() = 0;
    virtual bool rollback() = 0;

    // 执行 DDL / DML
    virtual bool execute(const std::string& sql) = 0;

    // 核心交易与账务实体持久化
    virtual bool save_order(const OrderData& order) = 0;
    virtual bool save_trade(const TradeData& trade) = 0;
    virtual bool save_position(const PositionData& pos) = 0;
    virtual bool save_account_snapshot(const AccountData& acc) = 0;

    // 查询接口
    virtual std::vector<OrderData> query_orders(const std::string& account_id, int limit = 100) = 0;
    virtual std::vector<TradeData> query_trades(const std::string& account_id, int limit = 100) = 0;
    virtual std::vector<PositionData> query_positions(const std::string& account_id) = 0;
    virtual AccountData query_latest_account(const std::string& account_id) = 0;

    // 数据库模式版本迁移
    virtual int get_schema_version() = 0;
    virtual bool run_migration(int target_version, const std::string& migration_sql) = 0;
};

/**
 * @brief 工厂方法：根据配置实例化对应存储适配器
 */
std::unique_ptr<IDatabaseAdapter> create_database_adapter(const DbConfig& config);

} // namespace kun
