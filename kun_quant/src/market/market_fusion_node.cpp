#include "kun/market/market_fusion_node.hpp"
#include "kun/market/fusion_operator.hpp"
#include <algorithm>
#include <iostream>
#include <cstring>

namespace kun {

MarketFusionNode::MarketFusionNode(MessageBus* bus, double outlier_pct_threshold)
    : bus_(bus), outlier_pct_threshold_(outlier_pct_threshold) {
    source_weights_["sina"] = 1.0;
    source_weights_["eastmoney"] = 1.0;
    source_weights_["ctp_sim"] = 1.0;
}

MarketFusionNode::~MarketFusionNode() {
    stop();
}

void MarketFusionNode::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;

    // 订阅多源原始行情主题 (market/source/...)
    message_bus_subscribe(bus_, "market/source/sina/rb2405", on_source_bus_msg, this);
    message_bus_subscribe(bus_, "market/source/eastmoney/rb2405", on_source_bus_msg, this);
    message_bus_subscribe(bus_, "market/source/ctp_sim/rb2405", on_source_bus_msg, this);
}

void MarketFusionNode::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

void MarketFusionNode::set_source_weight(const std::string& source_name, double weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_weights_[source_name] = weight;
}

void MarketFusionNode::on_source_bus_msg(const Message* msg, void* user_data) {
    auto* self = static_cast<MarketFusionNode*>(user_data);
    if (!msg || !self || msg->data_size < sizeof(QuantTickMsg)) return;

    std::string topic(msg->topic);
    size_t first = topic.find('/');
    size_t second = topic.find('/', first + 1);
    size_t third = topic.find('/', second + 1);

    if (second != std::string::npos && third != std::string::npos) {
        std::string src_name = topic.substr(second + 1, third - second - 1);
        const auto* tick = reinterpret_cast<const QuantTickMsg*>(msg->data);
        self->on_source_tick(src_name, *tick);
    }
}

void MarketFusionNode::on_source_tick(const std::string& source_name, const QuantTickMsg& tick) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string symbol = tick.symbol;

    auto now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    auto& telem = telemetries_[symbol];
    std::strncpy(telem.symbol, symbol.c_str(), sizeof(telem.symbol) - 1);
    telem.timestamp_us = now_us;

    // 1. 离群值/异常假刺针检测 (复用有状态通用算子)
    if (fusion_ops_.find(symbol) == fusion_ops_.end()) {
        fusion_ops_.emplace(symbol, MarketFusionOperator(outlier_pct_threshold_));
    }
    auto& op = fusion_ops_.at(symbol);
    bool is_outlier = op.check_and_record_price(tick.last_price);
    if (is_outlier) {
        telem.outliers_rejected++;
    }

    // 2. 记录该数据源最新观测
    double w = source_weights_.count(source_name) ? source_weights_[source_name] : 1.0;
    observations_[symbol][source_name] = {source_name, tick, now_us, w, is_outlier};

    // 3. 执行多源加权融合计算
    fuse_and_publish(symbol);
}

void MarketFusionNode::fuse_and_publish(const std::string& symbol) {
    const auto& obs_map = observations_[symbol];
    if (obs_map.empty()) return;

    std::unordered_map<std::string, MarketObservation> standard_obs;
    for (const auto& [k, v] : obs_map) {
        standard_obs[k] = {v.source_name, v.tick, v.arrival_time_us, v.weight, v.is_outlier};
    }

    if (fusion_ops_.find(symbol) == fusion_ops_.end()) {
        fusion_ops_.emplace(symbol, MarketFusionOperator(outlier_pct_threshold_));
    }
    auto& op = fusion_ops_.at(symbol);
    QuantTickMsg fused_tick{};
    uint32_t valid_sources = 0;
    double confidence = 0.0;

    if (!op.compute_fused_tick(symbol, standard_obs, fused_tick, valid_sources, confidence)) {
        return;
    }

    fused_ticks_[symbol] = fused_tick;

    // 更新诊断遥测指标
    auto& telem = telemetries_[symbol];
    telem.fused_last_price = fused_tick.last_price;
    telem.fused_bid1 = fused_tick.bid_price1;
    telem.fused_ask1 = fused_tick.ask_price1;
    telem.active_sources = valid_sources;
    telem.confidence = confidence;

    // 4. 广播至核心行情总线 Topic (market/tick/{symbol})
    std::string topic = "market/tick/" + symbol;
    message_bus_publish(bus_, topic.c_str(), "MarketFusionNode", &fused_tick, sizeof(fused_tick));

    // 广播融合诊断遥测 (monitor/fusion/{symbol})
    std::string telem_topic = "monitor/fusion/" + symbol;
    message_bus_publish(bus_, telem_topic.c_str(), "MarketFusionNode", &telem, sizeof(telem));
}

bool MarketFusionNode::get_fused_tick(const std::string& symbol, QuantTickMsg& out_tick) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fused_ticks_.find(symbol);
    if (it != fused_ticks_.end()) {
        out_tick = it->second;
        return true;
    }
    return false;
}

FusionTelemetryMsg MarketFusionNode::get_telemetry(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = telemetries_.find(symbol);
    if (it != telemetries_.end()) {
        return it->second;
    }
    FusionTelemetryMsg empty{};
    return empty;
}

} // namespace kun
