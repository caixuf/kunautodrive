#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <filesystem>
#include "kun/storage/db_adapter.hpp"
#include "kun/cluster/distributed_bridge.hpp"
#include "kun/storage/event_sourcing.hpp"
#include "message_bus.h"

using namespace kun;

void test_database_adapter_and_migration() {
    std::cout << "[Test 1] 运行数据库适配器抽象与 Schema 版本迁移测试...\n";
    DbConfig cfg;
    cfg.type = DatabaseType::SQLITE_WAL;
    cfg.sqlite_path = "data/test_enterprise.db";

    // 清理上次运行的残留 (含 WAL/SHM, 强杀进程可能留下脏日志导致断言失败)
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::filesystem::remove(std::string(cfg.sqlite_path) + suffix);
    }

    auto db = create_database_adapter(cfg);
    assert(db != nullptr);
    assert(db->connect());

    // 1. 测试基础增删改查
    OrderData o{};
    o.order_id = 88001;
    o.order_ref = "ref_88001";
    o.symbol = "rb2405";
    o.exchange = "SHFE";
    o.direction = Direction::LONG;
    o.offset = Offset::OPEN;
    o.price = 3625.0;
    o.total_volume = 10.0;
    o.traded_volume = 10.0;
    o.status = OrderStatus::FILLED;
    o.update_time_us = 1700000000;
    assert(db->save_order(o));

    auto orders = db->query_orders("acc_test", 10);
    assert(!orders.empty());
    assert(orders[0].order_id == 88001);

    // 2. 测试版本化迁移
    int v0 = db->get_schema_version();
    assert(v0 == 0);

    bool mig_ok = db->run_migration(1, "CREATE TABLE IF NOT EXISTS cluster_nodes (node_id TEXT PRIMARY KEY, role TEXT);");
    assert(mig_ok);
    assert(db->get_schema_version() == 1);

    db->disconnect();
    std::cout << "  -> 存储适配器与无损版本迁移测试通过!\n";
}

void test_active_standby_ha_and_split_brain() {
    std::cout << "[Test 2] 运行分布式高可用主备容灾与防脑裂租约测试...\n";
    MessageBus* bus = message_bus_create("test_bus_cluster_ha");

    // 启动 Leader 节点与 Standby 备用节点
    ActiveStandbyManager leader_node(bus, "node_leader_01", ClusterNodeRole::LEADER, 50, 200);
    ActiveStandbyManager standby_node(bus, "node_standby_02", ClusterNodeRole::STANDBY, 50, 200);

    leader_node.start();
    standby_node.start();

    assert(leader_node.can_execute_order());
    assert(!standby_node.can_execute_order());

    // 1. 模拟 Leader 心跳广播，Standby 保持备用
    ClusterHeartbeatMsg hb{};
    std::strncpy(hb.node_id, "node_leader_01", sizeof(hb.node_id) - 1);
    hb.role = static_cast<uint8_t>(ClusterNodeRole::LEADER);
    hb.term = 1;
    standby_node.on_heartbeat_msg(hb);
    standby_node.tick_lease_check();
    assert(standby_node.get_role() == ClusterNodeRole::STANDBY);

    // 2. 模拟 Leader 挂掉 (等待 250ms 租约过期)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    standby_node.tick_lease_check();
    // 验证 Standby 晋升为新 Leader
    assert(standby_node.get_role() == ClusterNodeRole::LEADER);
    assert(standby_node.can_execute_order());
    assert(standby_node.get_current_term() == 2);

    // 3. 模拟老 Leader 网络恢复，尝试以旧任期发单，收到新 Leader 心跳后自动降级 (防脑裂)
    ClusterHeartbeatMsg new_hb{};
    std::strncpy(new_hb.node_id, "node_standby_02", sizeof(new_hb.node_id) - 1);
    new_hb.role = static_cast<uint8_t>(ClusterNodeRole::LEADER);
    new_hb.term = 2;
    leader_node.on_heartbeat_msg(new_hb);
    assert(leader_node.get_role() == ClusterNodeRole::STANDBY);
    assert(!leader_node.can_execute_order());

    leader_node.stop();
    standby_node.stop();
    message_bus_destroy(bus);
    std::cout << "  -> 主备容灾切换与防脑裂租约防护测试 100% 通过!\n";
}

void test_event_sourcing_ledger_replay() {
    std::cout << "[Test 3] 运行金融级不可篡改事件溯源与账本重放测试...\n";
    EventSourcingReplayer replayer(1000000.0);

    // 产生 2 笔开平仓事件
    LedgerEvent e1{};
    e1.timestamp_us = 1000;
    e1.event_type = LedgerEventType::TRADE_EXECUTED;
    e1.symbol = "rb2405";
    e1.direction = Direction::LONG;
    e1.offset = Offset::OPEN;
    e1.price = 3600.0;
    e1.volume = 10.0;
    e1.fee = 10.0;
    replayer.append_event(e1);

    LedgerEvent e2{};
    e2.timestamp_us = 2000;
    e2.event_type = LedgerEventType::TRADE_EXECUTED;
    e2.symbol = "rb2405";
    e2.direction = Direction::SHORT;
    e2.offset = Offset::CLOSE;
    e2.price = 3650.0; // 盈利 50 点 * 10 手 * 10 乘数 = +5000 元
    e2.volume = 10.0;
    e2.fee = 10.0;
    replayer.append_event(e2);

    // 全量重放账本
    AccountData acc = replayer.replay_all();
    assert(acc.realized_pnl == 5000.0);
    assert(acc.commission == 20.0);
    assert(replayer.calculate_audit_checksum() != 0);

    std::cout << "  -> 事件溯源重放 0 误差重构账务! 校验和=" << replayer.calculate_audit_checksum() << "\n";
}

int main() {
    std::cout << "\n=========================================================\n";
    std::cout << " KunQuant 企业级存储、可迁移与分布式容灾单测集          \n";
    std::cout << "=========================================================\n\n";

    test_database_adapter_and_migration();
    test_active_standby_ha_and_split_brain();
    test_event_sourcing_ledger_replay();

    std::cout << "\n=========================================================\n";
    std::cout << "       全部企业级存储与分布式单测 100% 断言通过!         \n";
    std::cout << "=========================================================\n\n";
    return 0;
}
