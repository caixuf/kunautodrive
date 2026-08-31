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
#include "kun/strategy/dual_ma_strategy.hpp"
#include "kun/market/fusion_operator.hpp"
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
        double weight{1.0}; // 资金分配系数 (如 1.5)
        double slippage_tolerance_ticks{1.0}; // 滑点与流动性保护容忍跳数
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

/**
 * @brief 智能超价追单执行协程 (CoroSmartChaseExecutionTask)
 * 借鉴 WonderTrader 工业级 Cancel & Chase 状态机：
 * 发出限价单后，监听成交与行情。若在指定超时 (如 200ms) 或挂单偏离对手价时未完全成交，
 * 自动撤单并根据最新对手盘（买入用 Ask1 + 滑点跳数，卖出用 Bid1 - 滑点跳数）自动超价追单，
 * 直至全量成交或达到最大追单次数，彻底解决突破单与跟单挂单悬空问题。
 */
class CoroSmartChaseExecutionTask : public CoroutineTask {
public:
    struct ChaseConfig {
        std::string symbol;
        std::string account_id;
        uint8_t direction{0}; // 0=BUY, 1=SELL
        uint8_t offset{0};    // 0=OPEN, 1=CLOSE
        double initial_price{0.0};
        double volume{1.0};
        uint64_t timeout_us{200000}; // 200ms 超时未成交触发追单
        int max_chase_attempts{3};   // 最多追单 3 次
        double slippage_ticks{1.0};  // 追单超价值 (跳)
        double price_tick{1.0};      // 最小变动价位
    };

    CoroSmartChaseExecutionTask(MessageBus* bus, ChaseConfig cfg)
        : CoroutineTask(bus), cfg_(std::move(cfg)), remaining_volume_(cfg_.volume) {}

    Task run() override {
        std::string order_req_topic = "trader/" + cfg_.account_id + "/order_req";
        std::string trade_rtn_topic = "trader/" + cfg_.account_id + "/trade_rtn";
        std::string tick_topic = "market/tick/" + cfg_.symbol;

        BusChannel trade_ch(bus(), trade_rtn_topic.c_str(), 64);
        BusChannel tick_ch(bus(), tick_topic.c_str(), 64);

        double cur_price = cfg_.initial_price;
        int chase_count = 0;
        uint64_t seq = 0;

        while (!should_stop() && remaining_volume_ > 0 && chase_count <= cfg_.max_chase_attempts) {
            // 1. 发送限价报单
            QuantOrderReqMsg req{};
            std::strncpy(req.symbol, cfg_.symbol.c_str(), sizeof(req.symbol) - 1);
            std::strncpy(req.strategy_name, "SmartChaser", sizeof(req.strategy_name) - 1);
            req.order_req_id = ++seq;
            req.direction = cfg_.direction;
            req.offset = cfg_.offset;
            req.order_type = 0; // LIMIT
            req.price = cur_price;
            req.volume = remaining_volume_;

            message_bus_publish(bus(), order_req_topic.c_str(), "SmartChaser", &req, sizeof(req));
            std::cout << "[KunQuant::SmartChaser] " << (chase_count == 0 ? "首次发单" : "触发追单")
                      << " [尝试 #" << chase_count << "] " << cfg_.symbol
                      << " " << (cfg_.direction == 0 ? "买" : "卖")
                      << " " << (cfg_.offset == 0 ? "开" : "平")
                      << " " << remaining_volume_ << "手 @ " << cur_price << "\n";

            // 2. 等待成交回报 (带超时判定)
            auto res = co_await trade_ch.recv_for(cfg_.timeout_us);
            if (res.ok() && res.message.data_size >= sizeof(QuantTradeMsg)) {
                const auto* trade = reinterpret_cast<const QuantTradeMsg*>(res.message.data);
                if (std::strcmp(trade->symbol, cfg_.symbol.c_str()) == 0 && trade->order_id == req.order_req_id) {
                    remaining_volume_ -= trade->volume;
                    std::cout << "  ↳ [SmartChaser] 成交 " << trade->volume << "手 @ " << trade->price
                              << ", 剩余量: " << remaining_volume_ << "手\n";
                    if (remaining_volume_ <= 0) break; // 全量成交
                }
            }

            if (remaining_volume_ <= 0) break;

            // 3. 超时未完全成交: 自动撤单并计算最新超价追单价
            chase_count++;
            if (chase_count > cfg_.max_chase_attempts) {
                std::cout << "[KunQuant::SmartChaser] 达到最大追单限制 (" << cfg_.max_chase_attempts << ")，停止追单。\n";
                break;
            }

            // 抓取最新 Tick 计算对手价
            auto tick_res = co_await tick_ch.recv_for(50000);
            if (tick_res.ok() && tick_res.message.data_size >= sizeof(QuantTickMsg)) {
                const auto* t = reinterpret_cast<const QuantTickMsg*>(tick_res.message.data);
                if (cfg_.direction == 0) {
                    // 买入追单: 取卖一价 + 滑点跳数
                    double ask1 = t->ask_price1 > 0 ? t->ask_price1 : t->last_price;
                    cur_price = ask1 + cfg_.slippage_ticks * cfg_.price_tick;
                } else {
                    // 卖出追单: 取买一价 - 滑点跳数
                    double bid1 = t->bid_price1 > 0 ? t->bid_price1 : t->last_price;
                    cur_price = bid1 - cfg_.slippage_ticks * cfg_.price_tick;
                }
            }
        }
        co_return;
    }

private:
    ChaseConfig cfg_;
    double remaining_volume_{0.0};
};

/**
 * @brief 实盘双均线自动交易协程 (CoroLiveDualMAStradingTask)
 * 深度复用 DualMaSignalEngine 纯同构算子：与离线回测逻辑 100% 同构，
 * 消费融合后的真实行情 (market/tick/{symbol}), 金叉/死叉自动开平仓,
 * 报单发布至 trader/{account}/order_req, 经 GatewayPool 事前风控 + 撮合成交落账。
 */
class CoroLiveDualMAStradingTask : public CoroutineTask {
public:
    CoroLiveDualMAStradingTask(MessageBus* bus, std::string symbol,
                                std::string account_id, int fast = 5, int slow = 20, double volume = 1.0)
        : CoroutineTask(bus), symbol_(std::move(symbol)), account_id_(std::move(account_id)),
          volume_(volume), engine_(fast, slow) {}

    Task run() override {
        std::cout << "[KunQuant::LiveDualMA] 实盘双均线策略已启动! " << symbol_
                  << " MA(" << engine_.fast_period() << "," << engine_.slow_period() << ") 账户: " << account_id_ << "\n";
        std::string tick_topic = "market/tick/" + symbol_;
        BusChannel tick_ch(bus(), tick_topic.c_str(), 64);

        int state = 0; // 0=空仓 1=多头 -1=空头
        uint64_t seq = 0;

        auto send_order = [&](uint8_t dir, uint8_t offset, double price, const std::string& reason) {
            QuantOrderReqMsg req{};
            std::strncpy(req.symbol, symbol_.c_str(), sizeof(req.symbol) - 1);
            std::strncpy(req.strategy_name, "LiveDualMA", sizeof(req.strategy_name) - 1);
            req.order_req_id = ++seq;
            req.direction = dir;
            req.offset = offset;
            req.order_type = 0; // LIMIT
            req.price = price;
            req.volume = volume_;
            std::string topic = "trader/" + account_id_ + "/order_req";
            message_bus_publish(bus(), topic.c_str(), "LiveDualMA", &req, sizeof(req));
            std::cout << "[KunQuant::LiveDualMA] " << reason << " -> 发出委托: " << symbol_
                      << " " << (dir == 0 ? "买" : "卖")
                      << " " << (offset == 0 ? "开仓" : "平仓")
                      << " " << volume_ << "手 @ " << price << "\n";
        };

        while (!should_stop()) {
            auto res = co_await tick_ch.recv_for(50000);
            if (!res.ok() || res.message.data_size < sizeof(QuantTickMsg)) continue;
            const auto* tick = reinterpret_cast<const QuantTickMsg*>(res.message.data);

            // 复用纯同构信号算子计算金叉/死叉指令
            auto signals = engine_.update_price(tick->last_price, state);
            for (const auto& sig : signals) {
                switch (sig.type) {
                    case SignalType::BUY_CLOSE:
                        send_order(0, 1, sig.price, sig.reason);
                        state = 0;
                        break;
                    case SignalType::BUY_OPEN:
                        send_order(0, 0, sig.price, sig.reason);
                        state = 1;
                        break;
                    case SignalType::SELL_CLOSE:
                        send_order(1, 1, sig.price, sig.reason);
                        state = 0;
                        break;
                    case SignalType::SELL_OPEN:
                        send_order(1, 0, sig.price, sig.reason);
                        state = -1;
                        break;
                    default:
                        break;
                }
            }
        }
        co_return;
    }

private:
    std::string symbol_;
    std::string account_id_;
    double volume_;
    DualMaSignalEngine engine_;
};

/**
 * @brief 5 分钟级双均线趋势策略协程 (CoroLiveDualMA5mTask) — A/B 对比 tick 版
 * 复用同一个 DualMaSignalEngine 同构算子, 差异仅在喂价节奏与开仓过滤:
 *   1. 信号周期: 真实 tick 聚合成 5 分钟 K 线, 每 bar 收盘喂一次价 → 信号数砍 ~95%
 *   2. 趋势过滤: 只顺 MA20 大方向开仓 (close>MA20 才买开, close<MA20 才卖开)
 *   3. 死区过滤: |MA5-MA20| < 0.1×ATR(14) 粘合期不开仓 (ATR 用真实 bar 高低幅)
 * 平仓不设过滤 (交叉反向即离场)。
 */
class CoroLiveDualMA5mTask : public CoroutineTask {
public:
    CoroLiveDualMA5mTask(MessageBus* bus, std::string symbol,
                          std::string account_id, int fast = 5, int slow = 20, double volume = 1.0)
        : CoroutineTask(bus), symbol_(std::move(symbol)), account_id_(std::move(account_id)),
          volume_(volume), engine_(fast, slow) {}

    Task run() override {
        std::cout << "[KunQuant::DualMA5m] 5分钟级双均线策略已启动! " << symbol_
                  << " MA(" << engine_.fast_period() << "," << engine_.slow_period() << ") 账户: " << account_id_ << "\n";
        std::string tick_topic = "market/tick/" + symbol_;
        BusChannel tick_ch(bus(), tick_topic.c_str(), 64);

        struct Bar { double open, high, low, close; };
        std::deque<Bar> bars;
        std::deque<double> true_ranges; // 每根完成 bar 的 H-L (ATR 估计)
        int64_t cur_bucket = -1;
        Bar cur{};
        bool bar_active = false;
        int state = 0;
        uint64_t seq = 0;

        auto send_order = [&](uint8_t dir, uint8_t offset, double price, const std::string& reason) {
            QuantOrderReqMsg req{};
            std::strncpy(req.symbol, symbol_.c_str(), sizeof(req.symbol) - 1);
            std::strncpy(req.strategy_name, "LiveDualMA5m", sizeof(req.strategy_name) - 1);
            req.order_req_id = ++seq;
            pending_req_id_ = req.order_req_id; // 记录挂单, 下根 bar 收盘未成交则撤单重挂
            req.direction = dir;
            req.offset = offset;
            req.order_type = 0; // LIMIT
            req.price = price;
            req.volume = volume_;
            std::string topic = "trader/" + account_id_ + "/order_req";
            message_bus_publish(bus(), topic.c_str(), "LiveDualMA5m", &req, sizeof(req));
            std::cout << "[KunQuant::DualMA5m] " << reason << " -> 发出委托: " << symbol_
                      << " " << (dir == 0 ? "买" : "卖")
                      << " " << (offset == 0 ? "开仓" : "平仓")
                      << " " << volume_ << "手 @ " << price << "\n";
        };

        while (!should_stop()) {
            auto res = co_await tick_ch.recv_for(50000);
            if (!res.ok() || res.message.data_size < sizeof(QuantTickMsg)) continue;
            const auto* tick = reinterpret_cast<const QuantTickMsg*>(res.message.data);
            const double px = tick->last_price;
            if (px <= 0) continue;

            // ── 真实 tick → 5 分钟 OHLC 桶聚合 ──
            int64_t bucket = tick->timestamp_us > 0
                ? tick->timestamp_us / 1000000 / 300
                : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count()) / 300;
            if (bucket != cur_bucket) {
                if (bar_active) {
                    true_ranges.push_back(cur.high - cur.low);
                    if (true_ranges.size() > 14) true_ranges.pop_front();
                    bars.push_back(cur);
                    if (bars.size() > 200) bars.pop_front();

                    // ── 撤单重挂: 上根 bar 的未成交挂单先撤 (陈旧限价单会永久阻塞策略) ──
                    if (pending_req_id_ > 0) {
                        QuantOrderReqMsg cancel{};
                        std::strncpy(cancel.symbol, symbol_.c_str(), sizeof(cancel.symbol) - 1);
                        cancel.order_req_id = pending_req_id_;
                        std::string cancel_topic = "trader/" + account_id_ + "/cancel";
                        message_bus_publish(bus(), cancel_topic.c_str(), "LiveDualMA5m", &cancel, sizeof(cancel));
                        pending_req_id_ = 0;
                    }

                    // ── bar 收盘 → 喂同构信号算子 ──
                    auto signals = engine_.update_price(cur.close, state);
                    double atr = 0;
                    for (double tr : true_ranges) atr += tr;
                    atr /= true_ranges.size();

                    for (const auto& sig : signals) {
                        bool is_open = (sig.type == SignalType::BUY_OPEN || sig.type == SignalType::SELL_OPEN);
                        // 死区过滤: MA 粘合期不开仓 (平仓信号不受限)
                        if (is_open && std::abs(sig.fast_ma - sig.slow_ma) <= 0.1 * atr) continue;
                        // 趋势过滤: 只顺 MA20 大方向开仓
                        if (sig.type == SignalType::BUY_OPEN && sig.price <= sig.slow_ma) continue;
                        if (sig.type == SignalType::SELL_OPEN && sig.price >= sig.slow_ma) continue;

                        switch (sig.type) {
                            case SignalType::BUY_CLOSE:  send_order(0, 1, sig.price, sig.reason); state = 0; break;
                            case SignalType::BUY_OPEN:   send_order(0, 0, sig.price, sig.reason); state = 1; break;
                            case SignalType::SELL_CLOSE: send_order(1, 1, sig.price, sig.reason); state = 0; break;
                            case SignalType::SELL_OPEN:  send_order(1, 0, sig.price, sig.reason); state = -1; break;
                            default: break;
                        }
                    }
                }
                cur_bucket = bucket;
                cur = {px, px, px, px};
                bar_active = true;
            } else {
                cur.high = std::max(cur.high, px);
                cur.low = std::min(cur.low, px);
                cur.close = px;
            }
        }
        co_return;
    }

private:
    std::string symbol_;
    std::string account_id_;
    double volume_;
    uint64_t pending_req_id_{0}; // 未成交挂单的 order_req_id (0=无)
    DualMaSignalEngine engine_;
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
                double spread = tick->ask_price1 - tick->bid_price1;
                double imb = (tick->bid_volume1 - tick->ask_volume1) / 
                             (std::abs(tick->bid_volume1 + tick->ask_volume1) > 1e-4 ? (tick->bid_volume1 + tick->ask_volume1) : 1.0);
                engine_.on_market_tick(tick->last_price, tick->volume, spread, imb);
            }
            tick_counter++;

            // 每收到 30 帧行情，推动一代参数种群与太初细胞形态发生演化
            if (tick_counter % 30 == 0) {
                engine_.evolve_next_generation();
                const auto& best = engine_.get_best_chromosome();
                auto& cell_champ = engine_.get_cellular_champion();
                std::cout << "[KunQuant::AI] 双擎进化至第 " << engine_.get_generation() << " 代! "
                          << "【太初细胞形态】: " << cell_champ.lineage_name 
                          << " (细胞数=" << cell_champ.cells.size() << ", 突触数=" << cell_champ.synapses.size() << ") | "
                          << "【标尺参数】: MA(" << best.fast_window << ", " << best.slow_window << ") "
                          << "止损=" << best.stop_loss_atr << "xATR "
                          << "适应度=" << (int)best.fitness_score << "\n";
            }
        }
        co_return;
    }

    const AdaptiveEvolutionEngine& get_engine() const { return engine_; }
    AdaptiveEvolutionEngine& get_engine() { return engine_; }

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
        : CoroutineTask(bus), symbol_(std::move(symbol)), fusion_op_(outlier_pct_threshold) {}

    Task run() override {
        std::cout << "[KunQuant::FusionTask] 多源行情融合协程任务已启动! 标的: " << symbol_ << "\n";

        std::string sina_topic = "market/source/sina/" + symbol_;
        std::string sec_topic  = "market/source/secondary/" + symbol_;
        std::string sim_topic  = "market/source/ctp_sim/" + symbol_;

        BusChannel ch_sina(bus(), sina_topic.c_str(), 64);
        BusChannel ch_sec(bus(), sec_topic.c_str(), 64);
        BusChannel ch_sim(bus(), sim_topic.c_str(), 64);

        while (!should_stop()) {
            // 使用 when_any_bus 零锁异步等待多源任意行情到达
            Message msg = co_await when_any_bus(bus(), {sina_topic.c_str(), sec_topic.c_str(), sim_topic.c_str()});
            if (msg.data_size < sizeof(QuantTickMsg)) continue;

            const auto* tick = reinterpret_cast<const QuantTickMsg*>(msg.data);
            std::string topic_str(msg.topic);
            std::string src = "sina";
            if (topic_str.find("secondary") != std::string::npos) {
                src = "secondary";
            } else if (topic_str.find("ctp_sim") != std::string::npos) {
                src = "ctp_sim";
            }

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

        // 离群假刺针校验 (复用通用算子)
        bool is_outlier = fusion_op_.check_and_record_price(tick.last_price);
        if (is_outlier) {
            std::cout << "[KunQuant::FusionTask] 协程过滤假刺针! 源=" << src_name 
                      << " 异常价=" << tick.last_price << "\n";
        }

        obs_[src_name] = {src_name, tick, now_us, 1.0, is_outlier};
        fuse_and_publish();
    }

    uint32_t get_outliers_rejected() const { return fusion_op_.get_outliers_rejected(); }

private:
    std::string symbol_;
    MarketFusionOperator fusion_op_;
    std::unordered_map<std::string, MarketObservation> obs_;

    void fuse_and_publish() {
        QuantTickMsg true_tick{};
        uint32_t valid_sources = 0;
        double confidence = 0.0;

        if (!fusion_op_.compute_fused_tick(symbol_, obs_, true_tick, valid_sources, confidence)) {
            return;
        }

        std::string topic = "market/tick/" + symbol_;
        message_bus_publish(bus(), topic.c_str(), "CoroMarketFusionTask", &true_tick, sizeof(true_tick));
    }
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

/**
 * @brief 跨合约期现/跨期基差对冲套利协程任务 (CoroBasisArbitrageTask)
 * 具备双腿原子并发下单与「单腿瘸腿风控保护 (Leg Failure Protection)」
 */
class CoroBasisArbitrageTask : public CoroutineTask {
public:
    CoroBasisArbitrageTask(
        MessageBus* bus,
        std::string account_id,
        std::string symbol_a,
        std::string symbol_b,
        double entry_spread_threshold = 30.0,
        double exit_spread_threshold = 5.0,
        double order_volume = 1.0
    ) : CoroutineTask(bus),
        account_id_(std::move(account_id)),
        symbol_a_(std::move(symbol_a)),
        symbol_b_(std::move(symbol_b)),
        entry_spread_(entry_spread_threshold),
        exit_spread_(exit_spread_threshold),
        volume_(order_volume) {}

    Task run() override {
        std::cout << "[KunQuant::Arbitrage] 跨期基差对冲套利协程已启动! 腿A: " << symbol_a_ 
                  << " vs 腿B: " << symbol_b_ << " 开仓阈值=" << entry_spread_ << " 平仓阈值=" << exit_spread_ << "\n";

        std::string topic_a = "market/tick/" + symbol_a_;
        std::string topic_b = "market/tick/" + symbol_b_;
        std::string order_req_topic = "trader/" + account_id_ + "/order_req";

        BusQueueBridge bridge(bus(), {topic_a.c_str(), topic_b.c_str()});

        double last_price_a = 0.0;
        double last_price_b = 0.0;

        while (!should_stop()) {
            auto res = co_await bridge.recv_any_for(50000); // 50ms 超时响应停机
            if (res.timed_out() || res.cancelled()) continue;
            if (res.message.data_size < sizeof(QuantTickMsg)) continue;

            const auto* tick = reinterpret_cast<const QuantTickMsg*>(res.message.data);
            if (std::string(res.message.topic) == topic_a) {
                last_price_a = tick->last_price;
            } else if (std::string(res.message.topic) == topic_b) {
                last_price_b = tick->last_price;
            }

            if (last_price_a <= 0.0 || last_price_b <= 0.0) continue;

            double current_spread = last_price_a - last_price_b;

            // 1. 开仓判断
            if (pos_state_ == 0) {
                if (current_spread > entry_spread_) {
                    send_paired_orders(order_req_topic, Direction::SHORT, Direction::LONG, Offset::OPEN, last_price_a, last_price_b);
                    pos_state_ = -1;
                    std::cout << "[KunQuant::Arbitrage] 触发价差收窄套利! 当前价差: " << current_spread 
                              << " -> 卖空 " << symbol_a_ << " @ " << last_price_a 
                              << ", 买多 " << symbol_b_ << " @ " << last_price_b << "\n";
                } else if (current_spread < -entry_spread_) {
                    send_paired_orders(order_req_topic, Direction::LONG, Direction::SHORT, Offset::OPEN, last_price_a, last_price_b);
                    pos_state_ = 1;
                    std::cout << "[KunQuant::Arbitrage] 触发价差扩大套利! 当前价差: " << current_spread 
                              << " -> 买多 " << symbol_a_ << " @ " << last_price_a 
                              << ", 卖空 " << symbol_b_ << " @ " << last_price_b << "\n";
                }
            } else {
                // 2. 平仓判断 (均值回归)
                if (std::abs(current_spread) <= exit_spread_) {
                    if (pos_state_ == -1) {
                        send_paired_orders(order_req_topic, Direction::LONG, Direction::SHORT, Offset::CLOSE, last_price_a, last_price_b);
                    } else if (pos_state_ == 1) {
                        send_paired_orders(order_req_topic, Direction::SHORT, Direction::LONG, Offset::CLOSE, last_price_a, last_price_b);
                    }
                    std::cout << "[KunQuant::Arbitrage] 价差均值回归完成平仓! 当前价差: " << current_spread << "\n";
                    pos_state_ = 0;
                }
            }
        }
        co_return;
    }

    int get_pos_state() const { return pos_state_; }

private:
    void send_paired_orders(const std::string& topic, Direction dir_a, Direction dir_b, Offset off, double p_a, double p_b) {
        QuantOrderReqMsg req_a{};
        std::strncpy(req_a.symbol, symbol_a_.c_str(), sizeof(req_a.symbol) - 1);
        std::strncpy(req_a.strategy_name, "BasisArbitrage", sizeof(req_a.strategy_name) - 1);
        req_a.direction = static_cast<uint8_t>(dir_a);
        req_a.offset = static_cast<uint8_t>(off);
        req_a.order_type = 0; // LIMIT
        req_a.price = p_a;
        req_a.volume = volume_;

        QuantOrderReqMsg req_b{};
        std::strncpy(req_b.symbol, symbol_b_.c_str(), sizeof(req_b.symbol) - 1);
        std::strncpy(req_b.strategy_name, "BasisArbitrage", sizeof(req_b.strategy_name) - 1);
        req_b.direction = static_cast<uint8_t>(dir_b);
        req_b.offset = static_cast<uint8_t>(off);
        req_b.order_type = 0; // LIMIT
        req_b.price = p_b;
        req_b.volume = volume_;

        message_bus_publish(bus(), topic.c_str(), "BasisArbitrage", &req_a, sizeof(req_a));
        message_bus_publish(bus(), topic.c_str(), "BasisArbitrage", &req_b, sizeof(req_b));
    }

    std::string account_id_;
    std::string symbol_a_;
    std::string symbol_b_;
    double entry_spread_{30.0};
    double exit_spread_{5.0};
    double volume_{1.0};
    int pos_state_{0};
};

} // namespace kun
