#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cmath>

namespace kun {

enum class AccountRole : uint8_t {
    INDEPENDENT = 0, // 独立运行账户
    MASTER = 1,      // 信号主账户
    SLAVE = 2        // 自动跟单从账户
};

struct AccountProfile {
    std::string account_id;
    std::string broker_name;
    AccountRole role{AccountRole::INDEPENDENT};
    double capital_weight{1.0};   // 资金分配/跟单权重 (如 1.5 倍)
    double initial_balance{1000000.0};
    bool is_live{false};          // 强类型实盘机器标记 (默认为 false 仿真安全态)
};

enum class RouteMode : uint8_t {
    DIRECT = 0,           // 直连指定账户
    CAPITAL_WEIGHTED = 1, // 按资金权重自动拆分大单到多个账户
    MASTER_SLAVE = 2      // 主从跟单复制
};

/**
 * @brief 多账户订单路由分发引擎 (MultiAccountRouter)
 * 纯粹的领域逻辑路由组件：将抽象策略信号/订单精准分发到各账户管道
 */
class MultiAccountRouter {
public:
    MultiAccountRouter() = default;

    void add_account(const AccountProfile& profile) {
        std::lock_guard<std::mutex> lock(mutex_);
        accounts_[profile.account_id] = profile;
        if (profile.role == AccountRole::MASTER) {
            master_id_ = profile.account_id;
        }
    }

    const AccountProfile* get_account(const std::string& account_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) {
            return &(it->second);
        }
        return nullptr;
    }

    std::vector<AccountProfile> get_all_accounts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AccountProfile> list;
        for (const auto& [_, acc] : accounts_) {
            list.push_back(acc);
        }
        return list;
    }

    const std::string& get_master_account_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return master_id_;
    }

    /**
     * @brief 按资金权重将单笔报单拆分为多账户委托
     */
    std::vector<std::pair<std::string, OrderRequest>> split_order_by_weight(const OrderRequest& original_req) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, OrderRequest>> results;

        double total_weight = 0.0;
        for (const auto& [_, acc] : accounts_) {
            if (acc.role != AccountRole::SLAVE) {
                total_weight += acc.capital_weight;
            }
        }
        if (total_weight <= 0.0) total_weight = 1.0;

        for (const auto& [acc_id, acc] : accounts_) {
            if (acc.role == AccountRole::SLAVE) continue;

            double ratio = acc.capital_weight / total_weight;
            double split_vol = std::round(original_req.volume * ratio);
            if (split_vol < 1.0 && original_req.volume >= 1.0) {
                split_vol = 1.0;
            }

            OrderRequest req = original_req;
            req.volume = split_vol;
            req.order_ref = original_req.order_ref + "_" + acc_id;
            results.emplace_back(acc_id, req);
        }
        return results;
    }

private:
    mutable std::mutex mutex_;
    std::string master_id_;
    std::unordered_map<std::string, AccountProfile> accounts_;
};

} // namespace kun
