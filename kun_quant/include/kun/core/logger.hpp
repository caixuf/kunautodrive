#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>

namespace kun {

// 枚举值加 LOG_ 前缀: DEBUG 会撞 flowcoro Debug 构建注入的 -DDEBUG 宏,
// ERROR 会撞 Windows 头文件 (wingdi.h) 的 ERROR 宏 —— CI asan/mingw 双双翻车。
enum class LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) {
        level_ = level;
    }

    void log(LogLevel level, const std::string& tag, const std::string& msg) {
        if (level < level_) return;

        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t timer = std::chrono::system_clock::to_time_t(now);
        std::tm bt;
#if defined(_WIN32)
        localtime_s(&bt, &timer);
#else
        localtime_r(&timer, &bt);
#endif

        std::lock_guard<std::mutex> lock(mutex_);
        
        // ANSI Color
        const char* color_code = "\033[0m";
        const char* level_str = "INFO";
        switch (level) {
            case LogLevel::LOG_DEBUG: color_code = "\033[36m"; level_str = "DEBUG"; break;
            case LogLevel::LOG_INFO:  color_code = "\033[32m"; level_str = "INFO "; break;
            case LogLevel::LOG_WARN:  color_code = "\033[33m"; level_str = "WARN "; break;
            case LogLevel::LOG_ERROR: color_code = "\033[31m"; level_str = "ERROR"; break;
        }

        std::cout << color_code
                  << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S")
                  << "." << std::setfill('0') << std::setw(3) << now_ms.count() << "] "
                  << "[" << level_str << "] "
                  << "[" << tag << "] "
                  << msg
                  << "\033[0m\n";
    }

private:
    Logger() = default;
    LogLevel level_{LogLevel::LOG_INFO};
    std::mutex mutex_;
};

#define KUN_LOG_DEBUG(tag, msg) kun::Logger::instance().log(kun::LogLevel::LOG_DEBUG, tag, msg)
#define KUN_LOG_INFO(tag, msg)  kun::Logger::instance().log(kun::LogLevel::LOG_INFO, tag, msg)
#define KUN_LOG_WARN(tag, msg)  kun::Logger::instance().log(kun::LogLevel::LOG_WARN, tag, msg)
#define KUN_LOG_ERROR(tag, msg) kun::Logger::instance().log(kun::LogLevel::LOG_ERROR, tag, msg)

} // namespace kun
