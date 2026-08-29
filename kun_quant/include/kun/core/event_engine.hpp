#pragma once

#include "types.hpp"
#include <any>
#include <functional>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>

namespace kun {

struct Event {
    EventType type;
    std::any data;
};

using EventHandler = std::function<void(const Event&)>;

/**
 * @brief 核心事件分发引擎 (EventEngine)
 * 采用生产者-消费者模型，解耦行情接入、策略运算和订单回报
 */
class EventEngine {
public:
    EventEngine();
    ~EventEngine();

    EventEngine(const EventEngine&) = delete;
    EventEngine& operator=(const EventEngine&) = delete;

    // 注册特定事件类型的处理器
    void register_handler(EventType type, EventHandler handler);

    // 发送事件入队
    void put(Event event);
    void put(EventType type, std::any data);

    // 启动/停止事件循环处理线程
    void start();
    void stop();

    bool is_active() const { return active_.load(); }

private:
    void run();

    std::atomic<bool> active_{false};
    std::thread worker_thread_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<Event> event_queue_;

    std::mutex handler_mutex_;
    std::unordered_map<EventType, std::vector<EventHandler>> handlers_;
};

} // namespace kun
