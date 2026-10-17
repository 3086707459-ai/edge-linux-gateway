#include "edge/gateway.hpp"
#include "edge/fd.hpp"
#include "edge/http.hpp"
#include "edge/model.hpp"
#include "edge/modbus.hpp"
#include "edge/serial.hpp"
#include "edge/storage.hpp"
#include "dashboard.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <termios.h>
namespace edge {
namespace {
void watch(int epoll, int fd, std::uint32_t events, int operation) {
    epoll_event ev{}; ev.events = events; ev.data.fd = fd;
    if (epoll_ctl(epoll, operation, fd, &ev) != 0) throw std::runtime_error("epoll_ctl failed");
}
std::optional<int> get_limit(const std::string& target, const std::string& path) {
    if (target == path) return 50;
    const auto prefix = path + "?limit=";
    if (target.compare(0, prefix.size(), prefix) != 0) return {};
    const auto text = target.substr(prefix.size());
    if (text.empty() || text.size() > 3 || text.find_first_not_of("0123456789") != std::string::npos) return {};
    const int n = std::stoi(text);
    if (n < 1 || n > 200) return {};
    return n;
}
}
int run_gateway(const Config& c) {
    Fd lock(::open((c.database + ".lock").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!lock || flock(lock.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("database lock failed; check directory or another gateway instance");
    Fd epoll(epoll_create1(EPOLL_CLOEXEC));
    Fd timer(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM);
    Fd signals(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!epoll || !timer || !signals) throw std::runtime_error("cannot create Linux event descriptors");
    itimerspec timing{}; timing.it_value.tv_nsec = 1000000; timing.it_interval.tv_nsec = 20000000;
    if (timerfd_settime(timer.get(), 0, &timing, nullptr) != 0) throw std::runtime_error("timerfd_settime failed");
    watch(epoll.get(), timer.get(), EPOLLIN, EPOLL_CTL_ADD);
    watch(epoll.get(), signals.get(), EPOLLIN, EPOLL_CTL_ADD);
    Storage storage(c);
    Alarm alarm(c.alarm_high, c.alarm_hysteresis, storage.initial_alarm());
    ModbusDecoder decoder(c.unit);
    Fd serial;
    std::vector<std::uint8_t> transmit;
    std::size_t transmitted = 0;
    std::optional<Sample> latest;
    const auto started = monotonic_ms();
    std::int64_t last_success = 0, next_poll = 0, next_open = 0, deadline = 0;
    std::uint64_t acquired = 0, timeouts = 0, exceptions = 0, serial_failures = 0, reconnects = 0;
    unsigned consecutive_timeouts = 0;
    bool waiting = false, ever_opened = false;
    const auto stale = [&] {
        return !latest || monotonic_ms() - last_success > std::max(c.interval_ms * 3LL, c.timeout_ms * 2LL);
    };
    const auto healthy = [&] { return !stale() && storage.last_write_ok.load() && (c.source == "simulate" || static_cast<bool>(serial)); };
    const auto accept_sample = [&](double temperature, double humidity) {
        Sample s{unix_ms(), temperature, humidity, false, 0};
        s.transition = alarm.update(temperature); s.alarm = alarm.active();
        latest = s; last_success = monotonic_ms(); ++acquired;
        storage.submit(s);
        if (s.transition) std::cerr << "level=info event=" << (s.alarm ? "alarm_enter" : "alarm_clear") << " temperature_c=" << temperature << '\n';
    };
    HttpServer http(epoll.get(), c.bind, c.port, c.api_token, [&](const std::string& path) -> Response {
        if (path == "/") return {200, "text/html", dashboard};
        if (path == "/healthz") return {healthy() ? 200 : 503, "application/json", std::string("{\"healthy\":") + (healthy() ? "true}" : "false}")};
        if (path == "/api/v1/status") {
            std::ostringstream out;
            out << "{\"source\":\"" << c.source << "\",\"healthy\":" << (healthy() ? "true" : "false")
                << ",\"connected\":" << ((c.source == "simulate" || serial) ? "true" : "false")
                << ",\"stale\":" << (stale() ? "true" : "false") << ",\"uptime_ms\":" << monotonic_ms() - started
                << ",\"acquired\":" << acquired << ",\"persisted\":" << storage.persisted.load()
                << ",\"queue_depth\":" << storage.pending() << ",\"storage_dropped\":" << storage.dropped.load()
                << ",\"storage_errors\":" << storage.errors.load() << ",\"storage_ok\":" << (storage.last_write_ok.load() ? "true" : "false")
                << ",\"timeouts\":" << timeouts << ",\"modbus_exceptions\":" << exceptions
                << ",\"crc_errors\":" << decoder.crc_errors() << ",\"malformed_frames\":" << decoder.malformed()
                << ",\"serial_failures\":" << serial_failures << ",\"serial_reconnects\":" << reconnects
                << ",\"latest\":" << (latest ? sample_json(*latest) : "null") << '}';
            return {200, "application/json", out.str()};
        }
        for (const auto* endpoint : {"/api/v1/samples", "/api/v1/alarms"}) {
            const std::string base = endpoint;
            if (path == base || path.compare(0, base.size() + 1, base + "?") == 0) {
                const auto limit = get_limit(path, base);
                if (!limit) return {400, "application/json", "{\"error\":\"limit must be 1..200; only limit is supported\"}"};
                return {200, "application/json", base == "/api/v1/samples" ? storage.history(*limit) : storage.alarms(*limit)};
            }
        }
        if (path == "/metrics") {
            std::ostringstream out;
            out << "# TYPE edge_samples_total counter\nedge_samples_total " << acquired
                << "\n# TYPE edge_persisted_total counter\nedge_persisted_total " << storage.persisted.load()
                << "\n# TYPE edge_storage_dropped_total counter\nedge_storage_dropped_total " << storage.dropped.load()
                << "\n# TYPE edge_storage_errors_total counter\nedge_storage_errors_total " << storage.errors.load()
                << "\n# TYPE edge_queue_depth gauge\nedge_queue_depth " << storage.pending()
                << "\n# TYPE edge_modbus_timeouts_total counter\nedge_modbus_timeouts_total " << timeouts
                << "\n# TYPE edge_crc_errors_total counter\nedge_crc_errors_total " << decoder.crc_errors()
                << "\n# TYPE edge_healthy gauge\nedge_healthy " << (healthy() ? 1 : 0) << '\n';
            return {200, "text/plain; version=0.0.4", out.str()};
        }
        return {404, "application/json", "{\"error\":\"not found\"}"};
    });
    const auto disconnect = [&](std::int64_t now) {
        if (serial) epoll_ctl(epoll.get(), EPOLL_CTL_DEL, serial.get(), nullptr);
        serial.reset(); decoder.reset(); transmit.clear(); transmitted = 0; waiting = false;
        next_open = now + c.reconnect_ms; consecutive_timeouts = 0;
    };
    std::cerr << "level=info event=started source=" << c.source << " http=" << c.bind << ':' << c.port << '\n';
    bool running = true;
    while (running) {
        epoll_event events[32]{};
        const int count = epoll_wait(epoll.get(), events, 32, -1);
        if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("epoll_wait failed"); }
        for (int i = 0; i < count && running; ++i) {
            const int fd = events[i].data.fd;
            const auto flags = events[i].events;
            const auto now = monotonic_ms();
            if (fd == signals.get()) {
                signalfd_siginfo info{};
                if (::read(fd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) running = false;
            } else if (fd == timer.get()) {
                std::uint64_t ticks;
                if (::read(fd, &ticks, sizeof(ticks)) < 0 && errno != EAGAIN) throw std::runtime_error("timerfd read failed");
                http.expire(now);
                if (c.source == "simulate") {
                    if (now >= next_poll) {
                        // A deterministic 20-sample cycle exercises alarm enter and clear.
                        const double phase = static_cast<double>(acquired % 20);
                        accept_sample(24.0 + (phase < 10 ? phase : 20.0 - phase) * 0.9, 50.0 + std::sin(phase) * 8.0);
                        next_poll = now + c.interval_ms;
                    }
                } else {
                    if (!serial && now >= next_open) {
                        try {
                            serial = open_serial(c);
                            watch(epoll.get(), serial.get(), EPOLLIN | EPOLLRDHUP, EPOLL_CTL_ADD);
                            if (ever_opened) ++reconnects;
                            ever_opened = true; next_poll = now;
                            std::cerr << "level=info event=serial_connected\n";
                        } catch (const std::exception& e) {
                            ++serial_failures; disconnect(now);
                            std::cerr << "level=warning event=serial_open_failed message=\"" << json_escape(e.what()) << "\"\n";
                        }
                    }
                    if (serial && waiting && now >= deadline) {
                        ++timeouts; ++consecutive_timeouts; waiting = false; transmit.clear(); transmitted = 0;
                        decoder.reset();
                        watch(epoll.get(), serial.get(), EPOLLIN | EPOLLRDHUP, EPOLL_CTL_MOD);
                        if (consecutive_timeouts >= 3) disconnect(now);
                    }
                    if (serial && !waiting && now >= next_poll) {
                        tcflush(serial.get(), TCIFLUSH); decoder.reset();
                        transmit = read_request(c.unit, c.register_start); transmitted = 0;
                        waiting = true; deadline = now + c.timeout_ms; next_poll = now + c.interval_ms;
                        watch(epoll.get(), serial.get(), EPOLLIN | EPOLLOUT | EPOLLRDHUP, EPOLL_CTL_MOD);
                    }
                }
            } else if (serial && fd == serial.get()) {
                if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) { ++serial_failures; disconnect(now); continue; }
                if ((flags & EPOLLOUT) != 0 && !transmit.empty()) {
                    const auto n = ::write(fd, transmit.data() + transmitted, transmit.size() - transmitted);
                    if (n > 0) transmitted += static_cast<std::size_t>(n);
                    else if (n < 0 && errno != EAGAIN && errno != EINTR) { ++serial_failures; disconnect(now); continue; }
                    if (transmitted == transmit.size()) {
                        transmit.clear(); transmitted = 0;
                        watch(epoll.get(), fd, EPOLLIN | EPOLLRDHUP, EPOLL_CTL_MOD);
                    }
                }
                if ((flags & EPOLLIN) != 0) {
                    std::uint8_t data[256];
                    for (int batch = 0; batch < 16; ++batch) {
                        const auto n = ::read(fd, data, sizeof(data));
                        if (n > 0) decoder.feed(data, static_cast<std::size_t>(n));
                        else if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) break;
                        else if (errno == EINTR) continue;
                        else { ++serial_failures; disconnect(now); break; }
                    }
                    while (const auto result = decoder.next()) {
                        if (!waiting || !transmit.empty()) continue;
                        waiting = false;
                        consecutive_timeouts = 0;
                        if (result->exception) { ++exceptions; continue; }
                        accept_sample(result->temperature_c, result->humidity_pct);
                    }
                }
            } else http.handle(fd, flags);
        }
    }
    storage.stop();
    std::cerr << "level=info event=stopped acquired=" << acquired << " persisted=" << storage.persisted.load()
              << " dropped=" << storage.dropped.load() << " storage_errors=" << storage.errors.load() << '\n';
    return storage.errors.load() == 0 ? 0 : 2;
}
}
