#pragma once

#include "kun/core/types.hpp"
#include "message_bus.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <cmath>
#include <chrono>

namespace kun {

/**
 * @brief 单数据源行情观测包 (Source Observation)
 */
struct SourceTickObservation {
    std::string source_name; // "sina", "eastmoney", "ctp_sim"
    QuantTickMsg tick;
    uint64_t arrival_time_us{0};
    double weight{1.0};
    bool is_outlier{false};
};

/**
 * @brief 多源行情融合诊断遥测数据 (用于前端与可观测性看板)
 */
#pragma pack(push, 1)
struct FusionTelemetryMsg {
    char     symbol[16];
    uint64_t timestamp_us;
    double   fused_last_price;
    double   fused_bid1;
    double   fused_ask1;
    double   confidence;        // 置信度评分 (0.0 ~ 1.0)
    uint32_t active_sources;    // 当前活跃数据源数
    uint32_t outliers_rejected; // 累计过滤的异常刺针帧数
};
#pragma pack(pop)

/**
 * @brief 多源行情融合节点 (MarketFusionNode)
 * 深度复用自动驾驶传感器融合 (Sensor Fusion) 架构：
 * 1. 时间戳与到达时间对齐
 * 2. 滑动窗口中值与统计离群值过滤 (Outlier / Bad Tick Rejection)
 * 3. 动态置信度加权融合真值生成 (True Market Best Bid/Ask)
 * 4. 广播至全局真值管道 market/tick/{symbol}
 */
class MarketFusionNode {
public:
    explicit MarketFusionNode(MessageBus* bus, double outlier_pct_threshold = 0.03);
    ~MarketFusionNode();

    void start();
    void stop();

    // 接收来自特定源的原始行情观察值
    void on_source_tick(const std::string& source_name, const QuantTickMsg& tick);

    // 获取最新融合真值
    bool get_fused_tick(const std::string& symbol, QuantTickMsg& out_tick) const;
    FusionTelemetryMsg get_telemetry(const std::string& symbol) const;

    // 配置源权重 (如 Sina=0.4, Eastmoney=0.4, CTP=0.2)
    void set_source_weight(const std::string& source_name, double weight);

private:
    void fuse_and_publish(const std::string& symbol);

    static void on_source_bus_msg(const Message* msg, void* user_data);

    MessageBus* bus_;
    double outlier_pct_threshold_{0.03};
    bool running_{false};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> source_weights_;
    
    // symbol -> (source_name -> latest observation)
    std::unordered_map<std::string, std::unordered_map<std::string, SourceTickObservation>> observations_;
    // symbol -> rolling median price history (用于中值滤波基准)
    std::unordered_map<std::string, std::deque<double>> price_windows_;
    // symbol -> latest fused tick
    std::unordered_map<std::string, QuantTickMsg> fused_ticks_;
    // symbol -> telemetry stats
    std::unordered_map<std::string, FusionTelemetryMsg> telemetries_;
};

} // namespace kun
