#pragma once
#include "edge/config.hpp"
#include "edge/model.hpp"
#include "edge/queue.hpp"
#include <atomic>
#include <thread>
struct sqlite3;
namespace edge {
class Storage {
    sqlite3* writer_ = nullptr;
    sqlite3* reader_ = nullptr;
    BoundedQueue<Sample> queue_;
    std::thread worker_;
    std::size_t max_rows_;
    bool initial_alarm_ = false;
    void run() noexcept;
public:
    std::atomic<std::uint64_t> persisted{0}, errors{0}, dropped{0};
    std::atomic<bool> last_write_ok{true};
    explicit Storage(const Config& c);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    bool submit(Sample sample);
    void stop();
    bool initial_alarm() const { return initial_alarm_; }
    std::size_t pending() const { return queue_.size(); }
    std::string history(int limit);
    std::string alarms(int limit);
};
}
