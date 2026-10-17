#include "edge/config.hpp"
#include <arpa/inet.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace edge {
namespace {
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
long integer(const std::string& s) {
    std::size_t used = 0;
    const long n = std::stol(s, &used);
    if (used != s.size() || n < 0 || n > std::numeric_limits<int>::max())
        throw std::invalid_argument("invalid nonnegative integer");
    return n;
}
double number(const std::string& s) {
    std::size_t used = 0;
    const double n = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(n)) throw std::invalid_argument("invalid number");
    return n;
}
void require(bool ok, const char* why) { if (!ok) throw std::invalid_argument(why); }
}
Config Config::load(const std::string& path) {
    Config c;
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read configuration: " + path);
    std::string line;
    std::set<std::string> seen;
    std::size_t row = 0;
    while (std::getline(file, line)) {
        ++row;
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) throw std::runtime_error("config missing '=' at line " + std::to_string(row));
        const auto key = trim(line.substr(0, pos));
        const auto value = trim(line.substr(pos + 1));
        if (value.empty() || !seen.insert(key).second) throw std::runtime_error("empty/duplicate config key: " + key);
        try {
            if (key == "source") c.source = value;
            else if (key == "serial_device") c.serial_device = value;
            else if (key == "baud") c.baud = static_cast<int>(integer(value));
            else if (key == "parity") {
                require(value.size() == 1, "parity must be N/E/O"); c.parity = value.front();
            }
            else if (key == "unit") { const auto n = integer(value); require(n >= 1 && n <= 247, "unit 1..247"); c.unit = static_cast<std::uint8_t>(n); }
            else if (key == "register_start") { const auto n = integer(value); require(n <= 65534, "register_start 0..65534"); c.register_start = static_cast<std::uint16_t>(n); }
            else if (key == "interval_ms") c.interval_ms = static_cast<int>(integer(value));
            else if (key == "timeout_ms") c.timeout_ms = static_cast<int>(integer(value));
            else if (key == "reconnect_ms") c.reconnect_ms = static_cast<int>(integer(value));
            else if (key == "bind") c.bind = value;
            else if (key == "port") c.port = static_cast<int>(integer(value));
            else if (key == "database") c.database = value;
            else if (key == "queue_capacity") c.queue_capacity = static_cast<std::size_t>(integer(value));
            else if (key == "max_rows") c.max_rows = static_cast<std::size_t>(integer(value));
            else if (key == "alarm_high") c.alarm_high = number(value);
            else if (key == "alarm_hysteresis") c.alarm_hysteresis = number(value);
            else throw std::invalid_argument("unknown key");
        } catch (const std::exception& e) {
            throw std::runtime_error("config line " + std::to_string(row) + " (" + key + "): " + e.what());
        }
    }
    if (file.bad()) throw std::runtime_error("configuration read failed");
    if (const char* token = std::getenv("EDGE_API_TOKEN")) c.api_token = token;
    c.validate();
    return c;
}
void Config::validate() const {
    require(source == "simulate" || source == "modbus", "source must be simulate/modbus");
    require(baud == 9600 || baud == 19200 || baud == 38400 || baud == 57600 || baud == 115200, "unsupported baud");
    require(parity == 'N' || parity == 'E' || parity == 'O', "parity must be N/E/O");
    require(unit >= 1 && unit <= 247 && register_start <= 65534, "invalid Modbus address");
    require(interval_ms >= 50 && interval_ms <= 3600000, "interval_ms 50..3600000");
    require(timeout_ms >= 20 && timeout_ms <= 10000, "timeout_ms 20..10000");
    require(reconnect_ms >= 100 && reconnect_ms <= 60000, "reconnect_ms 100..60000");
    require(port >= 1 && port <= 65535, "port 1..65535");
    require(!database.empty() && database != ":memory:", "database must be a persistent file");
    require(queue_capacity >= 1 && queue_capacity <= 4096, "queue_capacity 1..4096");
    require(max_rows >= 1 && max_rows <= 10000000, "max_rows 1..10000000");
    require(std::isfinite(alarm_high) && alarm_high >= -40 && alarm_high <= 125, "alarm_high -40..125");
    require(std::isfinite(alarm_hysteresis) && alarm_hysteresis > 0 && alarm_hysteresis <= 50, "alarm_hysteresis (0,50]");
    in_addr addr{};
    require(inet_pton(AF_INET, bind.c_str(), &addr) == 1, "bind must be an IPv4 literal");
    if ((ntohl(addr.s_addr) >> 24U) != 127U) require(api_token.size() >= 16, "non-loopback bind requires EDGE_API_TOKEN with at least 16 characters");
    require(api_token.size() <= 256, "EDGE_API_TOKEN too long");
    for (const unsigned char ch : api_token) require(ch >= 33 && ch <= 126, "EDGE_API_TOKEN must be printable ASCII without spaces");
}
}
