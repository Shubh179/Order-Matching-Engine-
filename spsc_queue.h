#ifndef SPSC_QUEUE_H
#define SPSC_QUEUE_H

#include <atomic>
#include <array>

template <typename T, size_t Capacity = 1024>
class SPSCQueue {
private:
    std::array<T, Capacity> buffer;
    std::atomic<size_t> head{0}; // Read index
    std::atomic<size_t> tail{0}; // Write index

public:
    bool push(const T& item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t current_head = head.load(std::memory_order_acquire);

        // Ring buffer check: queue is full if tail has caught up to head
        if (((current_tail + 1) % Capacity) == current_head) {
            return false; // Full
        }

        buffer[current_tail] = item;
        tail.store((current_tail + 1) % Capacity, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        size_t current_tail = tail.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return false; // Empty
        }

        item = buffer[current_head];
        head.store((current_head + 1) % Capacity, std::memory_order_release);
        return true;
    }
};

#endif // SPSC_QUEUE_H
