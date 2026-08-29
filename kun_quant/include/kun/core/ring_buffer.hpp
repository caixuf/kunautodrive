#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace kun {

// 64 字节硬件缓存行对齐，避免多核心伪共享 (False Sharing)
#ifndef HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE
constexpr size_t CACHE_LINE_SIZE = 64;
#else
constexpr size_t CACHE_LINE_SIZE = HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE;
#endif

/**
 * @brief 高性能无锁单生产者单消费者 (SPSC) 环形缓冲区 (Lock-Free Ring Buffer)
 * @tparam T 存储的数据类型
 * @tparam Capacity 队列容量 (必须是 2 的整数次幂，便于位掩码加速取模)
 */
template <typename T, size_t Capacity = 65536>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    SPSCRingBuffer()
        : buffer_(static_cast<T*>(::operator new[](sizeof(T) * Capacity))) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    ~SPSCRingBuffer() {
        T dummy;
        while (pop(dummy)) {}
        ::operator delete[](buffer_);
    }

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /**
     * @brief 生产者入队 (Move 版本)
     */
    bool push(T&& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        // 缓冲区已满
        if ((current_tail - current_head) >= Capacity) {
            return false;
        }

        new (&buffer_[current_tail & MASK]) T(std::move(item));
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 生产者入队 (Copy 版本)
     */
    bool push(const T& item) {
        T copy = item;
        return push(std::move(copy));
    }

    /**
     * @brief 消费者出队
     */
    bool pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        // 缓冲区为空
        if (current_head == current_tail) {
            return false;
        }

        item = std::move(buffer_[current_head & MASK]);
        buffer_[current_head & MASK].~T();
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 当前队列中的元素数量
     */
    size_t size() const noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        return (current_tail >= current_head) ? (current_tail - current_head) : 0;
    }

    /**
     * @brief 检查队列是否为空
     */
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取最大容量
     */
    constexpr size_t capacity() const noexcept {
        return Capacity;
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    T* const buffer_;

    // 生产者的写指针 (放在独立 cache line)
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};

    // 消费者的读指针 (放在独立 cache line)
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
};

} // namespace kun
