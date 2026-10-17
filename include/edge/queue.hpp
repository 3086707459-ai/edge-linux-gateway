#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
namespace edge {
// Multi-producer/single-consumer; close wakes waiters and drains accepted work.
template<class T> class BoundedQueue {
    std::size_t capacity_;
    std::deque<T> items_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool closed_ = false;
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity is zero");
    }
    bool push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || items_.size() == capacity_) return false;
        items_.push_back(std::move(value));
        ready_.notify_one();
        return true;
    }
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        value = std::move(items_.front());
        items_.pop_front();
        return true;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }
};
}
