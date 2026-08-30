#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <memory>

struct sqlite3;

namespace kun {

/**
 * @brief 高性能 SQLite (WAL 模式) 量化账务与流水持久化管理器
 * 
 * 特性：
 * 1. 开启 SQLite WAL (Write-Ahead Logging) 模式与 NORMAL 同步，微秒级异步落盘
 * 2. 严格记录 4 张核心账表：orders (订单表), trades (成交流水), positions (持仓快照), account_flow (资金流水)
 * 3. 守护进程异常强杀或断电重启后，可无损恢复持仓状态与活跃挂单
 */
class StorageManager {
public:
    explicit StorageManager(std::string db_path = "data/kun_quant.db");
    ~StorageManager();

    bool init();
    void close();

    // 写入操作
    bool save_order(const OrderData& order, const std::string& account_id);
    bool save_trade(const TradeData& trade, const std::string& account_id);
    bool save_position(const PositionData& pos, const std::string& account_id);
    bool save_account(const AccountData& acc, const std::string& account_id);

    // 行情批量写入 (M3 行情侧): 单事务批量插入, 避免逐 tick 事务拖慢总线
    bool save_ticks_batch(const std::vector<TickData>& ticks);
    bool save_bars_batch(const std::vector<BarData>& bars);

    // 状态恢复与查询
    std::vector<OrderData> load_active_orders(const std::string& account_id);
    std::vector<TradeData> load_trades(const std::string& account_id, int limit = 1000);
    std::vector<TradeData> load_all_trades(int limit = 100000); // 跨账户全量成交流水 (绩效分析用)
    std::vector<PositionData> load_positions(const std::string& account_id);
    bool load_latest_account(const std::string& account_id, AccountData& out_acc);

    const std::string& get_db_path() const { return db_path_; }

private:
    bool create_tables();
    bool execute_sql(const std::string& sql);

    std::string db_path_;
    sqlite3* db_{nullptr};
    mutable std::mutex mutex_;
};

} // namespace kun
