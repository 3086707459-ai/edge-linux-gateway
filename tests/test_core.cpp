#include "edge/config.hpp"
#include "edge/http.hpp"
#include "edge/model.hpp"
#include "edge/modbus.hpp"
#include "edge/queue.hpp"
#include "edge/storage.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
int checks = 0;
void check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
std::vector<std::uint8_t> frame(std::vector<std::uint8_t> data) {
    const auto crc = edge::crc16(data.data(), data.size());
    data.push_back(static_cast<std::uint8_t>(crc & 0xffU));
    data.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return data;
}
void test_protocol() {
    const std::string reference = "123456789";
    check(edge::crc16(reinterpret_cast<const std::uint8_t*>(reference.data()), reference.size()) == 0x4b37, "CRC reference vector");
    check(edge::read_request(1, 0) == std::vector<std::uint8_t>({1, 4, 0, 0, 0, 2, 0x71, 0xcb}), "Modbus request wire format");
    edge::ModbusDecoder d(1);
    const auto good = frame({1, 4, 4, 0xff, 0xcc, 2, 0x64}); // -5.2 C, 61.2 %
    d.feed(good.data(), 4);
    check(!d.next(), "partial response retained");
    d.feed(good.data() + 4, good.size() - 4);
    auto result = d.next();
    check(result && result->temperature_c == -5.2 && result->humidity_pct == 61.2, "signed temperature and big endian registers");
    auto bad = good; bad.back() ^= 1;
    d.feed(bad.data(), bad.size());
    d.feed(good.data(), good.size());
    check(d.next().has_value() && d.crc_errors() >= 1, "recover after bad CRC");
    auto exception = frame({1, 0x84, 2});
    d.feed(exception.data(), exception.size());
    result = d.next();
    check(result && result->exception == 2, "exception response");
    const auto range = frame({1, 4, 4, 0, 250, 0xff, 0xff});
    d.feed(range.data(), range.size());
    check(!d.next() && d.malformed() > 0, "reject out of range measurements");
    std::vector<std::uint8_t> noise(2048, 0xff);
    d.feed(noise.data(), noise.size()); d.next();
    d.feed(good.data(), good.size());
    check(d.next().has_value(), "recover after oversized noise");
    d.feed(good.data(), good.size()); d.feed(good.data(), good.size());
    check(d.next().has_value() && d.next().has_value() && !d.next(), "coalesced frames");
    d.feed(good.data(), 5); d.reset();
    check(!d.next(), "reset stale partial response");
}
void test_alarm_queue() {
    edge::Alarm alarm(30, 2);
    check(alarm.update(29) == 0 && !alarm.active(), "below alarm threshold");
    check(alarm.update(30) == 1 && alarm.active(), "enter at high threshold");
    check(alarm.update(29) == 0 && alarm.active(), "hysteresis suppresses chatter");
    check(alarm.update(28) == -1 && !alarm.active(), "clear at low threshold");
    edge::Alarm restored(30, 2, true);
    check(restored.update(29) == 0 && restored.active(), "restore persisted alarm");
    edge::BoundedQueue<int> q(1);
    check(q.push(7) && !q.push(8), "bounded nonblocking push");
    q.close();
    int value = 0;
    check(q.pop(value) && value == 7 && !q.pop(value) && !q.push(9), "drain on close");
    edge::BoundedQueue<int> wake(2);
    bool ended = false;
    std::thread consumer([&] { int n; ended = !wake.pop(n); });
    wake.close(); consumer.join();
    check(ended, "close wakes blocked consumer");
}
void test_http() {
    check(edge::parse_request("GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n").error == 0, "valid request");
    auto r = edge::parse_request("GET /api/v1/status HTTP/1.1\r\nhOsT: localhost\r\naUtHoRiZaTiOn: Bearer test\r\n\r\n");
    check(r.authorization == "Bearer test" && r.error == 0, "case insensitive headers");
    for (const auto* raw : {
        "GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\nx",
        "GET / HTTP/1.1\r\nHost: a\r\nX: one\ntwo\r\n\r\n"})
        check(edge::parse_request(raw).error == 400, "reject malformed HTTP");
    check(edge::parse_request("POST / HTTP/1.1\r\nHost: a\r\n\r\n").error == 405, "read-only HTTP");
    check(edge::json_escape("a\"\\\n") == "a\\\"\\\\\\u000a", "JSON escapes");
}
void test_config_storage(const std::string& root) {
    unsetenv("EDGE_API_TOKEN");
    const auto path = root + "/gateway.conf";
    const auto load = [&](const std::string& text) {
        std::ofstream f(path); f << text; f.close(); return edge::Config::load(path);
    };
    check(load("source=simulate\nport=8090\n").port == 8090, "config valid");
    for (const auto* text : {"port=1\nport=2", "unknown=42", "alarm_high=nan", "unit=257", "port=70000", "bind=0.0.0.0", "queue_capacity=0", "source=other", "interval_ms=-1", "parity=NN"}) {
        bool rejected = false;
        try { load(text); } catch (const std::exception&) { rejected = true; }
        check(rejected, "reject invalid configuration");
    }
    setenv("EDGE_API_TOKEN", "0123456789abcdef", 1);
    check(load("bind=0.0.0.0").api_token.size() == 16, "remote bind requires environment token");
    unsetenv("EDGE_API_TOKEN");
    edge::Config c; c.database = root + "/test.db"; c.max_rows = 3; c.queue_capacity = 32;
    {
        edge::Storage db(c);
        for (int i = 0; i < 6; ++i) check(db.submit({1000 + i, 31.0, 50.0, i < 3, 0}), "enqueue sample");
        db.stop();
        check(db.persisted.load() == 6 && db.errors.load() == 0, "transactional storage drain");
        const auto rows = db.history(200);
        check(rows.find("\"id\":6") != std::string::npos && rows.find("\"id\":4") != std::string::npos && rows.find("\"id\":3") == std::string::npos, "row retention bound");
        const auto events = db.alarms(200);
        check(events.find("\"active\":true") != std::string::npos && events.find("\"active\":false") != std::string::npos, "alarm transitions stored");
    }
    {
        edge::Storage db(c);
        check(!db.initial_alarm(), "alarm state restored after restart");
        check(db.submit({2000, 35, 50, true, 0}), "restart enqueue"); db.stop();
        check(db.history(1).find("\"id\":7") != std::string::npos, "monotonic ids across restart");
    }
    { edge::Storage db(c); check(db.initial_alarm(), "active alarm restored"); }
}
}
int main() {
    char name[] = "/tmp/edge-core-XXXXXX";
    const char* root = mkdtemp(name);
    if (!root) return 1;
    try {
        test_protocol(); test_alarm_queue(); test_http(); test_config_storage(root);
        std::filesystem::remove_all(root);
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
