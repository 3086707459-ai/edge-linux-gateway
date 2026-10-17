#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
namespace edge {
inline std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Sample {
    std::int64_t timestamp_ms = 0;
    double temperature_c = 0;
    double humidity_pct = 0;
    bool alarm = false;
    // 1 = entered alarm, -1 = recovered, 0 = no transition.
    int transition = 0;
};
class Alarm {
    double high_, hysteresis_;
    bool active_ = false;
public:
    Alarm(double high, double hysteresis, bool active = false) : high_(high), hysteresis_(hysteresis), active_(active) {}
    int update(double temperature) {
        const bool before = active_;
        if (!active_ && temperature >= high_) active_ = true;
        else if (active_ && temperature <= high_ - hysteresis_) active_ = false;
        return before == active_ ? 0 : (active_ ? 1 : -1);
    }
    bool active() const { return active_; }
};
std::string json_escape(const std::string& value);
std::string sample_json(const Sample& s, std::int64_t id = 0);
}
