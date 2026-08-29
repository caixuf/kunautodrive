#pragma once

#include "kun/core/types.hpp"
#include "message_bus.h"
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>
#include <functional>

namespace kun {

enum class ClusterNodeRole {
    LEADER = 0,   // 主节点 (唯一具有实盘发单权)
    STANDBY = 1,  // 热备节点 (同步总线状态，心跳超时自动接管)
    FOLLOWER = 2  // 纯跟从节点 (只读/算力/行情接收)
};

inline std::string to_string(ClusterNodeRole r) {
    switch (r) {
        case ClusterNodeRole::LEADER:   return "LEADER";
        case ClusterNodeRole::STANDBY:  return "STANDBY";
        case ClusterNodeRole::FOLLOWER: return "FOLLOWER";
        default: return "UNKNOWN";
    }
}

#pragma pack(push, 1)
struct ClusterHeartbeatMsg {
    char     node_id[32];
    uint8_t  role;         // ClusterNodeRole
    uint64_t term;         // 租约任期号
    uint64_t timestamp_us;
    double   current_equity;
};
#pragma pack(pop)

/**
 * @brief 分布式高可用与主备容灾管理器 (ActiveStandbyManager)
 * 
 * 核心机制：
 * 1. 租约心跳 (Lease Heartbeat)：Leader 周期性广播心跳租约
 * 2. 防脑裂 (Split-Brain Prevention)：只有持有有效任期租约的 Leader 允许发单
 * 3. 故障平滑接管 (Seamless Failover)：Standby 在租约超时 (如 500ms) 后晋升为 Leader 并恢复状态
 */
class ActiveStandbyManager {
public:
    ActiveStandbyManager(
        MessageBus* bus,
        std::string node_id,
        ClusterNodeRole initial_role = ClusterNodeRole::STANDBY,
        uint64_t heartbeat_interval_ms = 100,
        uint64_t lease_timeout_ms = 500
    );

    ~ActiveStandbyManager();

    void start();
    void stop();

    // 接收集群心跳
    void on_heartbeat_msg(const ClusterHeartbeatMsg& msg);

    // 周期性检查心跳租约 (在主循环或协程中调用)
    void tick_lease_check();

    // 发单权限守门检查 (非 Leader 严禁下发真实指令)
    bool can_execute_order() const;

    ClusterNodeRole get_role() const { return role_.load(); }
    uint64_t get_current_term() const { return current_term_.load(); }
    const std::string& get_node_id() const { return node_id_; }

    void register_role_change_callback(std::function<void(ClusterNodeRole, ClusterNodeRole)> cb) {
        role_change_cb_ = std::move(cb);
    }

private:
    void promote_to_leader();
    void send_heartbeat();

    MessageBus* bus_;
    std::string node_id_;
    std::atomic<ClusterNodeRole> role_;
    std::atomic<uint64_t> current_term_{1};
    uint64_t heartbeat_interval_ms_{100};
    uint64_t lease_timeout_ms_{500};

    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point last_leader_heartbeat_time_;
    std::string active_leader_id_;

    std::function<void(ClusterNodeRole, ClusterNodeRole)> role_change_cb_;
};

} // namespace kun
