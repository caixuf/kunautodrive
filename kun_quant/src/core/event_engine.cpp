#include "kun/core/event_engine.hpp"
#include "kun/core/logger.hpp"

namespace kun {

EventEngine::EventEngine() = default;

EventEngine::~EventEngine() {
    stop();
}

void EventEngine::register_handler(EventType type, EventHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handlers_[type].push_back(std::move(handler));
}

void EventEngine::put(Event event) {
    if (!active_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        event_queue_.push(std::move(event));
    }
    queue_cv_.notify_one();
}

void EventEngine::put(EventType type, std::any data) {
    put(Event{type, std::move(data)});
}

void EventEngine::start() {
    if (active_.load()) return;
    active_.store(true);
    worker_thread_ = std::thread(&EventEngine::run, this);
    KUN_LOG_INFO("EventEngine", "EventEngine worker thread started.");
}

void EventEngine::stop() {
    if (!active_.load()) return;
    active_.store(false);
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    KUN_LOG_INFO("EventEngine", "EventEngine worker thread stopped.");
}

void EventEngine::run() {
    while (active_.load()) {
        Event event;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !event_queue_.empty() || !active_.load();
            });

            if (!active_.load() && event_queue_.empty()) {
                break;
            }

            if (event_queue_.empty()) {
                continue;
            }

            event = std::move(event_queue_.front());
            event_queue_.pop();
        }

        // 分发事件
        std::vector<EventHandler> targets;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            auto it = handlers_.find(event.type);
            if (it != handlers_.end()) {
                targets = it->second;
            }
        }

        for (const auto& handler : targets) {
            try {
                handler(event);
            } catch (const std::exception& e) {
                KUN_LOG_ERROR("EventEngine", std::string("Exception in event handler: ") + e.what());
            } catch (...) {
                KUN_LOG_ERROR("EventEngine", "Unknown exception in event handler.");
            }
        }
    }
}

} // namespace kun
