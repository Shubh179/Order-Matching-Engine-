#ifndef MUTEX_QUEUE_H
#define MUTEX_QUEUE_H

#include <queue>
#include <mutex>

// ============================================================================
// MutexQueue: A thread-safe queue using std::queue + std::mutex.
//
// Purpose:
//   Serves as the baseline comparison for the lock-free SPSC queue
//   in the Part 4 benchmark. By measuring identical workloads through
//   both queues, we can quantify the latency and throughput improvement
//   that lock-free design provides.
//
// API:
//   Matches SPSCQueue exactly (push/pop returning bool) so the benchmark
//   can swap implementations via template parameter.
//
// Why no condition_variable?
//   - To match the SPSC queue's non-blocking API (return false if empty)
//   - The benchmark consumer spin-yields on pop, same as production
//   - Using condition_variable would measure CV wakeup latency instead
//     of raw queue contention latency — an unfair comparison
//
// Performance characteristics:
//   - push: lock + push + unlock → O(1) amortized but with mutex overhead
//   - pop:  lock + front/pop + unlock → O(1) amortized but with mutex overhead
//   - Each lock/unlock pair costs ~25-50ns on modern Linux (futex-based)
//   - Under contention (producer + consumer racing), mutex operations
//     can spike to 1-10μs due to context switching
//
// Thread safety: FULL (any number of producers/consumers)
// ============================================================================
template <typename T>
class MutexQueue {
private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;

    // No capacity limit — std::queue grows dynamically.
    // This is intentional: the mutex queue should not be penalized
    // for ring-buffer-full backpressure that doesn't apply to it.

public:
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(item);
        return true;  // Always succeeds (unbounded)
    }

    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) {
            return false;
        }
        item = queue_.front();
        queue_.pop();
        return true;
    }
};

#endif // MUTEX_QUEUE_H
