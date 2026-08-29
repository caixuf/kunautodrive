#include "kun/market/market_fusion_node.hpp"
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
    // 支持通配前缀订阅
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

    // 解析 topic: "market/source/{source_name}/{symbol}"
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

    // 1. 离群值/异常假刺针检测 (Outlier Filtering)
    auto& p_win = price_windows_[symbol];
    bool is_outlier = false;

    if (p_win.size() >= 3) {
        std::vector<double> sorted(p_win.begin(), p_win.end());
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];

        double dev_pct = std::abs(tick.last_price - median) / median;
        if (dev_pct > outlier_pct_threshold_) {
            is_outlier = true;
            telem.outliers_rejected++;
            std::cout << "[MarketFusion] 触发异常刺针过滤! 源: " << source_name
                      << " 标的: " << symbol << " 异常价=" << tick.last_price 
                      << " 基准中值=" << median << " 偏离度=" << (dev_pct * 100.0) << "%\n";
        }
    }

    if (!is_outlier) {
        p_win.push_back(tick.last_price);
        if (p_win.size() > 30) p_win.pop_front();
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

    double sum_weighted_price = 0.0;
    double sum_weight = 0.0;
    double best_bid = 0.0;
    double best_ask = 9999999.0;
    double total_vol = 0.0;
    double total_oi = 0.0;
    uint32_t valid_sources = 0;

    for (const auto& [name, obs] : obs_map) {
        if (obs.is_outlier) continue;

        sum_weighted_price += obs.tick.last_price * obs.weight;
        sum_weight += obs.weight;
        total_vol = std::max(total_vol, obs.tick.volume);
        total_oi = std::max(total_oi, obs.tick.open_interest);

        if (obs.tick.bid_price1 > best_bid) {
            best_bid = obs.tick.bid_price1;
        }
        if (obs.tick.ask_price1 > 0.0 && obs.tick.ask_price1 < best_ask) {
            best_ask = obs.tick.ask_price1;
        }

        valid_sources++;
    }

    if (sum_weight <= 0.0 || valid_sources == 0) return;

    double fused_price = sum_weighted_price / sum_weight;
    if (best_ask >= 9999990.0) best_ask = fused_price + 1.0;
    if (best_bid <= 0.0) best_bid = fused_price - 1.0;

    // 构造全局融合真值 QuantTickMsg
    QuantTickMsg fused_tick{};
    std::strncpy(fused_tick.symbol, symbol.c_str(), sizeof(fused_tick.symbol) - 1);
    std::strncpy(fused_tick.exchange, "SHFE", sizeof(fused_tick.exchange) - 1);
    fused_tick.timestamp_us = telemetries_[symbol].timestamp_us;
    fused_tick.last_price = fused_price;
    fused_tick.volume = total_vol;
    fused_tick.open_interest = total_oi;
    fused_tick.bid_price1 = best_bid;
    fused_tick.bid_volume1 = 100.0;
    fused_tick.ask_price1 = best_ask;
    fused_tick.ask_volume1 = 100.0;

    fused_ticks_[symbol] = fused_tick;

    // 更新诊断遥测指标
    auto& telem = telemetries_[symbol];
    telem.fused_last_price = fused_price;
    telem.fused_bid1 = best_bid;
    telem.fused_ask1 = best_ask;
    telem.active_sources = valid_sources;
    telem.confidence = (valid_sources >= 2) ? 0.98 : 0.75;

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
