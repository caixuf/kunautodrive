#pragma once

/**
 * @file kun_quant_flowcoro.h
 * @brief 鲲量化 (KunQuant) 深度复用 FlowEngine & flowcoro 协程底座
 *
 * 【设计哲学：复用不变，隔离变化】
 * - 不变 (Mechanism):
 *     1. flowcoro 确定性协程调度器 (RtExecutor / RtTask / g_node_exec)
 *     2. 零拷贝 / 内存总线 (MessageBus / BusChannel / WhenAnyBus)
 *     3. 任务模型 (CoroutineTask / node_pump)
 * - 变化 (Policy):
 *     1. 消息载荷：TickMsg, BarMsg, OrderReqMsg, TradeMsg
 *     2. 节点职责：行情接入、事前风控、策略协程 (TWAP/套利/CTA)、撮合网关
 */

#include "coroutine_task.h"
#include "message_bus.h"
#include <cstdint>
#include <cmath>
#include <iostream>
#include <cstring>
#include <vector>
#include <string>
#include "kun/core/types.hpp"
#include "kun/engine/storage_manager.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <deque>

namespace kun {

// ============================================================================
// 1. 量化协程任务定义 (基于 flowcoro 协程)
// ============================================================================

// ============================================================================
// 2. 复用 CoroutineTask 的量化策略任务模板
// ============================================================================

/**
 * @brief 异步 TWAP 智能拆单协程任务 (基于 flowcoro)
 * 彻底消除状态机，用 co_await 实现微秒级无阻塞拆单
 */
class CoroTwapExecutionTask : public CoroutineTask {
public:
    CoroTwapExecutionTask(MessageBus* bus, const char* symbol, double total_vol, int slices, uint64_t interval_us)
        : CoroutineTask(bus), total_volume_(total_vol), slices_(slices), interval_us_(interval_us) {
        std::strncpy(symbol_, symbol, sizeof(symbol_) - 1);
    }

    Task run() override {
        std::cout << "[KunQuant::TWAP] 协程启动 symbol=" << symbol_
                  << " total_vol=" << total_volume_ << " slices=" << slices_ << "\n";

        std::string tick_topic = std::string("market/tick/") + symbol_;
        std::string trade_topic = std::string("trader/trade/") + symbol_;

        BusChannel tick_ch(bus(), tick_topic.c_str(), 32);
        BusChannel trade_ch(bus(), trade_topic.c_str(), 64);

        double slice_vol = total_volume_ / slices_;

        for (int i = 0; i < slices_ && !should_stop(); ++i) {
            // 1. 协程挂起，零 CPU 占用等待最新一笔 Tick 行情
            Message tick_raw = co_await tick_ch.recv();
            const auto* tick = reinterpret_cast<const QuantTickMsg*>(tick_raw.data);

            // 2. 以盘口买一价挂限价买单
            QuantOrderReqMsg order_req{};
            std::strncpy(order_req.symbol, symbol_, sizeof(order_req.symbol) - 1);
            std::strncpy(order_req.strategy_name, "CoroTWAP", sizeof(order_req.strategy_name) - 1);
            order_req.order_req_id = ++req_seq_;
            order_req.direction = 0; // LONG
            order_req.offset = 0;    // OPEN
            order_req.order_type = 0;// LIMIT
            order_req.price = tick->bid_price1 > 0 ? tick->bid_price1 : tick->last_price;
            order_req.volume = slice_vol;

            // 发布至报单总线
            message_bus_publish(bus(), "trader/order_req", "CoroTWAP", &order_req, sizeof(order_req));

            std::cout << "[KunQuant::TWAP] 发出批次 [" << (i + 1) << "/" << slices_
                      << "] 价格=" << order_req.price << " 手数=" << order_req.volume << "\n";

            // 3. 异步休眠间隔
            co_await delay_us(interval_us_);
        }

        std::cout << "[KunQuant::TWAP] 拆单执行完毕。\n";
        co_return;
    }

private:
    char symbol_[16]{};
    double total_volume_{0.0};
    int slices_{5};
    uint64_t interval_us_{1000000}; // 1秒
    uint64_t req_seq_{0};
};

/**
 * @brief 跨品种价差套利协程任务 (基于 WhenAnyBus 多路异步选择)
 */
class CoroSpreadArbitrageTask : public CoroutineTask {
public:
    CoroSpreadArbitrageTask(MessageBus* bus, const char* leg_a, const char* leg_b, double threshold)
        : CoroutineTask(bus), threshold_(threshold) {
        std::strncpy(leg_a_, leg_a, sizeof(leg_a_) - 1);
        std::strncpy(leg_b_, leg_b, sizeof(leg_b_) - 1);
    }

    Task run() override {
        std::cout << "[KunQuant::Arbitrage] 套利协程启动: " << leg_a_ << " vs " << leg_b_ << "\n";

        std::string topic_a = std::string("market/tick/") + leg_a_;
        std::string topic_b = std::string("market/tick/") + leg_b_;

        while (!should_stop()) {
            // 复用 KunAutoDrive 的 WhenAnyBus：两个品种谁先来行情就唤醒谁，彻底告别轮询！
            Message msg = co_await when_any_bus(
                bus(), {topic_a.c_str(), topic_b.c_str()}
            );

            const auto* tick = reinterpret_cast<const QuantTickMsg*>(msg.data);
            if (std::strcmp(msg.topic, topic_a.c_str()) == 0) {
                last_price_a_ = tick->last_price;
            } else {
                last_price_b_ = tick->last_price;
            }

            if (last_price_a_ > 0.0 && last_price_b_ > 0.0) {
                double spread = last_price_a_ - last_price_b_;
                if (spread > threshold_) {
                    std::cout << "[KunQuant::Arbitrage] 触发正向套利信号! Spread=" << spread << " > " << threshold_ << "\n";
                }
            }
        }
        co_return;
    }

private:
    char leg_a_[16]{};
    char leg_b_[16]{};
    double threshold_{50.0};
    double last_price_a_{0.0};
    double last_price_b_{0.0};
};

/**
 * @brief 多账户主从跟单协程任务 (基于 flowcoro 协程与 MessageBus 命名空间)
 * 监听主账户成交回报，毫秒级按资金权重自动向从账户管道分发同向委托
 */
class CoroFollowTradingTask : public CoroutineTask {
public:
    struct SlaveConfig {
        char account_id[32];
        double weight; // 资金分配系数 (如 1.5)
    };

    CoroFollowTradingTask(MessageBus* bus, const char* master_account_id, std::vector<SlaveConfig> slaves)
        : CoroutineTask(bus), slaves_(std::move(slaves)) {
        std::strncpy(master_id_, master_account_id, sizeof(master_id_) - 1);
    }

    Task run() override {
        std::cout << "[KunQuant::FollowTrading] 多账户跟单协程已就绪! 监听主账户: " << master_id_
                  << " (跟单从账户数: " << slaves_.size() << ")\n";

        std::string master_trade_topic = std::string("trader/") + master_id_ + "/trade_rtn";
        BusChannel trade_ch(bus(), master_trade_topic.c_str(), 64);

        while (!should_stop()) {
            // 零 CPU 挂起等待主账户成交; 50ms 超时保证停机时协程可协作退出
            // (裸 recv() 在停机后永远等不到消息, executor 兜底销毁存在时序脆弱性)
            auto res = co_await trade_ch.recv_for(50000);
            if (!res.ok()) continue; // 超时空转, 回到循环头检查 should_stop
            const auto* master_trade = reinterpret_cast<const QuantTradeMsg*>(res.message.data);

            std::cout << "[KunQuant::FollowTrading] 主账户 [" << master_id_ << "] 发生真实成交: "
                      << master_trade->symbol << " 方向=" << (int)master_trade->direction
                      << " 开平=" << (int)master_trade->offset
                      << " 价格=" << master_trade->price
                      << " 手数=" << master_trade->volume << "\n";

            // 毫秒级广播给所有从账户
            for (const auto& slave : slaves_) {
                double target_vol = std::round(master_trade->volume * slave.weight);
                if (target_vol < 1.0 && master_trade->volume >= 1.0) target_vol = 1.0;

                QuantOrderReqMsg slave_req{};
                std::strncpy(slave_req.symbol, master_trade->symbol, sizeof(slave_req.symbol) - 1);
                std::strncpy(slave_req.strategy_name, "FollowCopier", sizeof(slave_req.strategy_name) - 1);
                slave_req.order_req_id = ++seq_;
                slave_req.direction = master_trade->direction;
                slave_req.offset = master_trade->offset;
                slave_req.order_type = 0; // LIMIT
                slave_req.price = master_trade->price;
                slave_req.volume = target_vol;

                std::string slave_order_topic = std::string("trader/") + slave.account_id + "/order_req";
                message_bus_publish(bus(), slave_order_topic.c_str(), "FollowCopier", &slave_req, sizeof(slave_req));

                std::cout << "  ↳ [从账户跟单] " << slave.account_id 
                          << " (权重 " << slave.weight << ") -> 发出委托: " 
                          << slave_req.symbol << " " << slave_req.volume << "手 @ " << slave_req.price << "\n";
            }
        }
        co_return;
    }

private:
    char master_id_[32]{};
    std::vector<SlaveConfig> slaves_;
    uint64_t seq_{0};
};

} // namespace kun

#include "kun/strategy/adaptive_evolution_engine.hpp"

namespace kun {

/**
 * @brief 在线模型自适应进化协程任务 (基于 flowcoro 协程)
 * 实时监听全市场 Tick/Bar，持续在线模拟多组参数种群，优胜劣汰自适应进化
 */
class CoroAdaptiveEvolutionTask : public CoroutineTask {
public:
    CoroAdaptiveEvolutionTask(MessageBus* bus, const char* watch_symbol, int population_size = 20)
        : CoroutineTask(bus), symbol_(watch_symbol), engine_(population_size) {}

    Task run() override {
        std::cout << "[KunQuant::AI] 在线策略自适应进化引擎已启动! 标的: " << symbol_ << "\n";
        std::string tick_topic = std::string("market/tick/") + symbol_;
        BusChannel tick_ch(bus(), tick_topic.c_str(), 64);

        int tick_counter = 0;
        while (!should_stop()) {
            Message msg = co_await tick_ch.recv();
            if (msg.data_size >= sizeof(QuantTickMsg)) {
                const auto* tick = reinterpret_cast<const QuantTickMsg*>(msg.data);
                engine_.on_market_tick(tick->last_price);
            }
            tick_counter++;

            // 每收到 30 帧行情，推动一代参数种群进化
            if (tick_counter % 30 == 0) {
                engine_.evolve_next_generation();
                const auto& best = engine_.get_best_chromosome();
                std::cout << "[KunQuant::AI] 策略进化至第 " << engine_.get_generation() << " 代! "
                          << "最优参数: MA(" << best.fast_window << ", " << best.slow_window << ") "
                          << "止损=" << best.stop_loss_atr << "xATR "
                          << "适应度得分=" << (int)best.fitness_score << "\n";
            }
        }
        co_return;
    }

    const AdaptiveEvolutionEngine& get_engine() const { return engine_; }

private:
    std::string symbol_;
    AdaptiveEvolutionEngine engine_;
};

/**
 * @brief 多源行情融合协程任务 (基于 flowcoro::CoroutineTask 深度复用)
 * 纯粹由 flowcoro 协程驱动，零锁、零线程自旋，异步对齐多源行情并进行异常刺针过滤
 */
class CoroMarketFusionTask : public CoroutineTask {
public:
    CoroMarketFusionTask(MessageBus* bus, std::string symbol, double outlier_pct_threshold = 0.03)
        : CoroutineTask(bus), symbol_(std::move(symbol)), outlier_pct_threshold_(outlier_pct_threshold) {}

    Task run() override {
        std::cout << "[KunQuant::FusionTask] 多源行情融合协程任务已启动! 标的: " << symbol_ << "\n";

        std::string sina_topic = "market/source/sina/" + symbol_;
        std::string sim_topic = "market/source/ctp_sim/" + symbol_;

        BusChannel ch_sina(bus(), sina_topic.c_str(), 64);
        BusChannel ch_sim(bus(), sim_topic.c_str(), 64);

        while (!should_stop()) {
            // 使用 when_any_bus 零锁异步等待多源任意行情到达
            Message msg = co_await when_any_bus(bus(), {sina_topic.c_str(), sim_topic.c_str()});
            if (msg.data_size < sizeof(QuantTickMsg)) continue;

            const auto* tick = reinterpret_cast<const QuantTickMsg*>(msg.data);
            std::string topic_str(msg.topic);
            std::string src = (topic_str.find("sina") != std::string::npos) ? "sina" : "ctp_sim";

            process_source_tick(src, *tick);
        }
        co_return;
    }

    void process_source_tick(const std::string& src_name, const QuantTickMsg& tick) {
        uint64_t now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );

        // 离群假刺针校验 (Outlier Rejection)
        bool is_outlier = false;
        if (price_window_.size() >= 3) {
            std::vector<double> sorted(price_window_.begin(), price_window_.end());
            std::sort(sorted.begin(), sorted.end());
            double median = sorted[sorted.size() / 2];
            double dev = std::abs(tick.last_price - median) / median;
            if (dev > outlier_pct_threshold_) {
                is_outlier = true;
                outliers_rejected_++;
                std::cout << "[KunQuant::FusionTask] 协程过滤假刺针! 源=" << src_name 
                          << " 异常价=" << tick.last_price << " 基准=" << median << "\n";
            }
        }

        if (!is_outlier) {
            price_window_.push_back(tick.last_price);
            if (price_window_.size() > 30) price_window_.pop_front();
        }

        obs_[src_name] = {src_name, tick, now_us, 1.0, is_outlier};
        fuse_and_publish();
    }

    uint32_t get_outliers_rejected() const { return outliers_rejected_; }

private:
    struct Obs {
        std::string src;
        QuantTickMsg tick;
        uint64_t time_us;
        double weight;
        bool is_outlier;
    };

    void fuse_and_publish() {
        double sum_p = 0.0;
        double sum_w = 0.0;
        double best_bid = 0.0;
        double best_ask = 9999999.0;
        double total_vol = 0.0;
        double total_oi = 0.0;
        uint32_t valid = 0;

        for (const auto& [name, ob] : obs_) {
            if (ob.is_outlier) continue;
            sum_p += ob.tick.last_price * ob.weight;
            sum_w += ob.weight;
            total_vol = std::max(total_vol, ob.tick.volume);
            total_oi = std::max(total_oi, ob.tick.open_interest);
            if (ob.tick.bid_price1 > best_bid) best_bid = ob.tick.bid_price1;
            if (ob.tick.ask_price1 > 0.0 && ob.tick.ask_price1 < best_ask) best_ask = ob.tick.ask_price1;
            valid++;
        }

        if (sum_w <= 0.0 || valid == 0) return;

        double fused_price = sum_p / sum_w;
        if (best_ask >= 9999990.0) best_ask = fused_price + 1.0;
        if (best_bid <= 0.0) best_bid = fused_price - 1.0;

        QuantTickMsg true_tick{};
        std::strncpy(true_tick.symbol, symbol_.c_str(), sizeof(true_tick.symbol) - 1);
        std::strncpy(true_tick.exchange, "SHFE", sizeof(true_tick.exchange) - 1);
        true_tick.last_price = fused_price;
        true_tick.bid_price1 = best_bid;
        true_tick.ask_price1 = best_ask;
        true_tick.bid_volume1 = 100.0;
        true_tick.ask_volume1 = 100.0;
        true_tick.volume = total_vol;
        true_tick.open_interest = total_oi;

        std::string topic = "market/tick/" + symbol_;
        message_bus_publish(bus(), topic.c_str(), "CoroMarketFusionTask", &true_tick, sizeof(true_tick));
    }

    std::string symbol_;
    double outlier_pct_threshold_{0.03};
    uint32_t outliers_rejected_{0};
    std::unordered_map<std::string, Obs> obs_;
    std::deque<double> price_window_;
};

/**
 * @brief 单合约行情落盘协程任务 (CoroSingleTickRecorderTask, 方案 A: 单合约单协程隔离)
 * 独立监听单个合约的真值流 market/tick/{symbol}, 批量异步写入 SQLite ticks 表。
 * 各合约通道物理隔离互不阻塞，单合约停摆不影响其他合约，recv_for(50000) 确保优雅停机秒级响应。
 */
class CoroSingleTickRecorderTask : public CoroutineTask {
public:
    CoroSingleTickRecorderTask(MessageBus* bus, std::string symbol,
                               StorageManager* storage, size_t batch_size = 500)
        : CoroutineTask(bus), symbol_(std::move(symbol)),
          storage_(storage), batch_size_(batch_size) {
        topic_ = "market/tick/" + symbol_;
    }

    Task run() override {
        BusChannel channel(bus(), topic_.c_str(), 64);
        last_flush_ = std::chrono::steady_clock::now();

        while (!should_stop()) {
            auto res = co_await channel.recv_for(50000); // 50ms 超时响应停机
            if (res.ok()) {
                const auto* t = reinterpret_cast<const QuantTickMsg*>(res.message.data);
                // 只落盘真实行情; 历史回放 (exchange=REPLAY) 不计入真实 tick 账本
                if (std::strcmp(t->exchange, "REPLAY") == 0) continue;
                TickData td;
                td.symbol = t->symbol;
                td.exchange = t->exchange;
                td.timestamp_us = (t->timestamp_us != 0)
                    ? static_cast<int64_t>(t->timestamp_us)
                    : std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
                td.last_price = t->last_price;
                td.bid_price[0] = t->bid_price1;
                td.ask_price[0] = t->ask_price1;
                td.bid_volume[0] = t->bid_volume1;
                td.ask_volume[0] = t->ask_volume1;
                td.volume = t->volume;
                td.open_interest = t->open_interest;

                buffer_.push_back(std::move(td));
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count();
            if (buffer_.size() >= batch_size_ || (!buffer_.empty() && elapsed_ms >= 1000)) {
                flush();
            }
        }
        flush(); // 停机前冲刷残余
        co_return;
    }

    uint64_t get_total_saved() const { return total_saved_; }

private:
    void flush() {
        if (buffer_.empty() || !storage_) {
            last_flush_ = std::chrono::steady_clock::now();
            return;
        }
        if (storage_->save_ticks_batch(buffer_)) {
            total_saved_ += buffer_.size();
        }
        buffer_.clear();
        last_flush_ = std::chrono::steady_clock::now();
    }

    std::string symbol_;
    std::string topic_;
    StorageManager* storage_{nullptr};
    size_t batch_size_{500};
    std::vector<TickData> buffer_;
    std::chrono::steady_clock::time_point last_flush_;
    uint64_t total_saved_{0};
};

/**
 * @brief 行情落盘协程任务 (CoroTickRecorderTask, M3 行情侧)
 * 兼容多合约监听模式与单合约转发
 */
class CoroTickRecorderTask : public CoroutineTask {
public:
    CoroTickRecorderTask(MessageBus* bus, std::vector<std::string> symbols,
                         StorageManager* storage, size_t batch_size = 500)
        : CoroutineTask(bus), symbols_(std::move(symbols)),
          storage_(storage), batch_size_(batch_size) {
        for (const auto& s : symbols_) {
            topics_.push_back("market/tick/" + s);
        }
    }

    Task run() override {
        if (symbols_.empty()) co_return;

        std::vector<std::unique_ptr<BusChannel>> channels;
        channels.reserve(topics_.size());
        for (const auto& t : topics_) {
            channels.push_back(std::make_unique<BusChannel>(bus(), t.c_str(), 64));
        }

        last_flush_ = std::chrono::steady_clock::now();
        size_t round_robin = 0;

        while (!should_stop()) {
            auto res = co_await channels[round_robin % channels.size()]->recv_for(50000); // 50ms 超时
            round_robin++;
            if (!res.ok()) {
                if (should_stop()) break;
                // 空闲时检查是否有待刷新的残余数据
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count();
                if (!buffer_.empty() && elapsed_ms >= 1000) {
                    flush();
                }
                continue;
            }

            const auto* t = reinterpret_cast<const QuantTickMsg*>(res.message.data);
            TickData td;
            td.symbol = t->symbol;
            td.exchange = t->exchange;
            td.timestamp_us = (t->timestamp_us != 0)
                ? static_cast<int64_t>(t->timestamp_us)
                : std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
            td.last_price = t->last_price;
            td.bid_price[0] = t->bid_price1;
            td.ask_price[0] = t->ask_price1;
            td.bid_volume[0] = t->bid_volume1;
            td.ask_volume[0] = t->ask_volume1;
            td.volume = t->volume;
            td.open_interest = t->open_interest;

            buffer_.push_back(std::move(td));

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count();
            if (buffer_.size() >= batch_size_ || elapsed_ms >= 1000) {
                flush();
            }
        }
        flush(); // 停机前冲刷残余缓冲
        co_return;
    }

    uint64_t get_total_saved() const { return total_saved_; }

private:
    void flush() {
        if (buffer_.empty() || !storage_) {
            if (buffer_.empty()) last_flush_ = std::chrono::steady_clock::now();
            return;
        }
        if (storage_->save_ticks_batch(buffer_)) {
            total_saved_ += buffer_.size();
        }
        buffer_.clear();
        last_flush_ = std::chrono::steady_clock::now();
    }

    std::vector<std::string> symbols_;
    std::vector<std::string> topics_;
    StorageManager* storage_{nullptr};
    size_t batch_size_{500};
    std::vector<TickData> buffer_;
    std::chrono::steady_clock::time_point last_flush_;
    uint64_t total_saved_{0};
};

} // namespace kun
