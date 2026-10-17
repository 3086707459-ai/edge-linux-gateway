#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace edge {
std::uint16_t crc16(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> read_request(std::uint8_t unit, std::uint16_t start);
struct Reading { double temperature_c; double humidity_pct; std::uint8_t exception = 0; };
class ModbusDecoder {
    std::uint8_t unit_;
    std::vector<std::uint8_t> bytes_;
    std::uint64_t crc_errors_ = 0, malformed_ = 0;
public:
    explicit ModbusDecoder(std::uint8_t unit) : unit_(unit) {}
    void feed(const std::uint8_t* data, std::size_t size);
    std::optional<Reading> next();
    void reset() { bytes_.clear(); }
    std::uint64_t crc_errors() const { return crc_errors_; }
    std::uint64_t malformed() const { return malformed_; }
};
}
