#include "edge/modbus.hpp"
#include <algorithm>
namespace edge {
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc >> 1U) ^ ((crc & 1U) ? 0xa001U : 0U));
    }
    return crc;
}
std::vector<std::uint8_t> read_request(std::uint8_t unit, std::uint16_t start) {
    std::vector<std::uint8_t> out{unit, 0x04, static_cast<std::uint8_t>(start >> 8U), static_cast<std::uint8_t>(start & 0xffU), 0, 2};
    const auto crc = crc16(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xffU));
    out.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return out;
}
void ModbusDecoder::feed(const std::uint8_t* data, std::size_t size) {
    // Bounded even under continuous garbage; keep a response-sized suffix.
    if (size > 256) { data += size - 256; size = 256; ++malformed_; }
    if (bytes_.size() + size > 512) { bytes_.clear(); ++malformed_; }
    bytes_.insert(bytes_.end(), data, data + size);
}
std::optional<Reading> ModbusDecoder::next() {
    while (bytes_.size() >= 2) {
        if (bytes_[0] != unit_ || (bytes_[1] != 0x04 && bytes_[1] != 0x84)) {
            bytes_.erase(bytes_.begin()); ++malformed_; continue;
        }
        if (bytes_.size() < 3) return {};
        const bool exception = bytes_[1] == 0x84;
        if (!exception && bytes_[2] != 4) { bytes_.erase(bytes_.begin()); ++malformed_; continue; }
        const std::size_t length = exception ? 5U : 9U;
        if (bytes_.size() < length) return {};
        const auto expected = crc16(bytes_.data(), length - 2);
        const auto actual = static_cast<std::uint16_t>(bytes_[length - 2] | (static_cast<unsigned>(bytes_[length - 1]) << 8U));
        if (expected != actual) { bytes_.erase(bytes_.begin()); ++crc_errors_; continue; }
        if (exception) {
            const auto code = bytes_[2];
            bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(length));
            if (code == 0) { ++malformed_; continue; }
            return Reading{0, 0, code};
        }
        const int raw_temp = static_cast<int>((static_cast<unsigned>(bytes_[3]) << 8U) | bytes_[4]);
        const int signed_temp = raw_temp >= 32768 ? raw_temp - 65536 : raw_temp;
        const double temperature = static_cast<double>(signed_temp) / 10.0;
        const double humidity = static_cast<double>((static_cast<unsigned>(bytes_[5]) << 8U) | bytes_[6]) / 10.0;
        bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(length));
        if (temperature < -40 || temperature > 125 || humidity > 100) { ++malformed_; continue; }
        return Reading{temperature, humidity, 0};
    }
    return {};
}
}
