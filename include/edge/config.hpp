#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace edge {
struct Config {
    std::string source = "simulate";
    std::string serial_device = "/dev/ttyUSB0";
    int baud = 9600;
    char parity = 'N';
    std::uint8_t unit = 1;
    std::uint16_t register_start = 0;
    int interval_ms = 1000;
    int timeout_ms = 300;
    int reconnect_ms = 1000;
    std::string bind = "127.0.0.1";
    int port = 8080;
    std::string database = "gateway.db";
    std::size_t queue_capacity = 256;
    std::size_t max_rows = 100000;
    double alarm_high = 30.0;
    double alarm_hysteresis = 2.0;
    std::string api_token; // Environment only: never written to config or logs.
    static Config load(const std::string& path);
    void validate() const;
};
}
