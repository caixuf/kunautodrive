#include "kun/cluster/distributed_bridge.hpp"
#include <iostream>
#include <cstring>

namespace kun {

ActiveStandbyManager::ActiveStandbyManager(
    MessageBus* bus,
    std::string node_id,
    ClusterNodeRole initial_role,
    uint64_t heartbeat_interval_ms,
    uint64_t lease_timeout_ms
) : bus_(bus),
    node_id_(std::move(node_id)),
    role_(initial_role),
    heartbeat_interval_ms_(heartbeat_interval_ms),
    lease_timeout_ms_(lease_timeout_ms) {
    last_leader_heartbeat_time_ = std::chrono::steady_clock::now();
}

ActiveStandbyManager::~ActiveStandbyManager() {
    stop();
}

void ActiveStandbyManager::start() {
    running_.store(true);
    std::cout << "[Cluster::" << node_id_ << "] 启动高可用主备容灾管理器，初始角色: " << to_string(role_.load()) << "\n";
    if (role_.load() == ClusterNodeRole::LEADER) {
        send_heartbeat();
    }
}

void ActiveStandbyManager::stop() {
    running_.store(false);
}

void ActiveStandbyManager::send_heartbeat() {
    ClusterHeartbeatMsg msg{};
    std::strncpy(msg.node_id, node_id_.c_str(), sizeof(msg.node_id) - 1);
    msg.role = static_cast<uint8_t>(role_.load());
    msg.term = current_term_.load();
    msg.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    msg.current_equity = 1000000.0;

    message_bus_publish(bus_, "cluster/heartbeat", "ActiveStandbyManager", &msg, sizeof(msg));
}

void ActiveStandbyManager::on_heartbeat_msg(const ClusterHeartbeatMsg& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sender_id(msg.node_id);
    if (sender_id == node_id_) return; // 忽略自身心跳

    ClusterNodeRole sender_role = static_cast<ClusterNodeRole>(msg.role);

    if (sender_role == ClusterNodeRole::LEADER) {
        if (msg.term >= current_term_.load()) {
            // 防脑裂保护：如果自身也是 Leader 但对方任期更大或并列，降级为 Standby
            if (role_.load() == ClusterNodeRole::LEADER) {
                std::cerr << "[Cluster::" << node_id_ << "] [SPLIT_BRAIN_DEFENSE] 发现更高/并列任期 Leader (" 
                          << sender_id << ", Term=" << msg.term << ")，主动降级为 STANDBY!\n";
                ClusterNodeRole old_role = role_.load();
                role_.store(ClusterNodeRole::STANDBY);
                if (role_change_cb_) role_change_cb_(old_role, ClusterNodeRole::STANDBY);
            }
            current_term_.store(msg.term);
            active_leader_id_ = sender_id;
            last_leader_heartbeat_time_ = std::chrono::steady_clock::now();
        }
    }
}

void ActiveStandbyManager::promote_to_leader() {
    ClusterNodeRole old_role = role_.load();
    current_term_++;
    role_.store(ClusterNodeRole::LEADER);
    active_leader_id_ = node_id_;
    std::cout << "[Cluster::" << node_id_ << "] [FAILOVER] 心跳租约超时! 节点晋升为新 LEADER (Term=" 
              << current_term_.load() << ")\n";
    send_heartbeat();
    if (role_change_cb_) role_change_cb_(old_role, ClusterNodeRole::LEADER);
}

void ActiveStandbyManager::tick_lease_check() {
    if (!running_.load()) return;

    auto now = std::chrono::steady_clock::now();
    if (role_.load() == ClusterNodeRole::LEADER) {
        send_heartbeat();
    } else if (role_.load() == ClusterNodeRole::STANDBY) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_leader_heartbeat_time_).count();
        if (elapsed_ms > static_cast<int64_t>(lease_timeout_ms_)) {
            promote_to_leader();
        }
    }
}

bool ActiveStandbyManager::can_execute_order() const {
    return role_.load() == ClusterNodeRole::LEADER;
}

} // namespace kun
