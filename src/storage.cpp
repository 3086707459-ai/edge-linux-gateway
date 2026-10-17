#include "edge/storage.hpp"
#include <sqlite3.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
namespace edge {
namespace {
void exec(sqlite3* db, const std::string& sql) {
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db)));
}
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite prepare: " + std::string(sqlite3_errmsg(db)));
    return Statement(stmt, sqlite3_finalize);
}
void done(sqlite3* db, sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) throw std::runtime_error("SQLite step: " + std::string(sqlite3_errmsg(db)));
}
}
std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
        else if (ch < 0x20) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(ch) << std::dec;
        else out << static_cast<char>(ch);
    }
    return out.str();
}
std::string sample_json(const Sample& s, std::int64_t id) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << "{\"id\":" << id << ",\"timestamp_ms\":" << s.timestamp_ms
        << ",\"temperature_c\":" << s.temperature_c << ",\"humidity_pct\":" << s.humidity_pct
        << ",\"alarm\":" << (s.alarm ? "true" : "false") << '}';
    return out.str();
}
Storage::Storage(const Config& c) : queue_(c.queue_capacity), max_rows_(c.max_rows) {
    try {
        if (sqlite3_open_v2(c.database.c_str(), &writer_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot open database");
        sqlite3_busy_timeout(writer_, 200);
        exec(writer_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=256;");
        auto version = prepare(writer_, "PRAGMA user_version");
        if (sqlite3_step(version.get()) != SQLITE_ROW) throw std::runtime_error("cannot read schema version");
        const int v = sqlite3_column_int(version.get(), 0);
        version.reset();
        if (v != 0 && v != 1) throw std::runtime_error("unsupported database schema version");
        exec(writer_, "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS samples("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,timestamp_ms INTEGER NOT NULL,temperature_c REAL NOT NULL,"
            "humidity_pct REAL NOT NULL,alarm INTEGER NOT NULL CHECK(alarm IN (0,1)));"
            "CREATE TABLE IF NOT EXISTS alarms(id INTEGER PRIMARY KEY AUTOINCREMENT,sample_id INTEGER NOT NULL,"
            "timestamp_ms INTEGER NOT NULL,active INTEGER NOT NULL,temperature_c REAL NOT NULL);"
            "PRAGMA user_version=1; COMMIT;");
        auto latest = prepare(writer_, "SELECT alarm FROM samples ORDER BY id DESC LIMIT 1");
        const int rc = sqlite3_step(latest.get());
        if (rc == SQLITE_ROW) initial_alarm_ = sqlite3_column_int(latest.get(), 0) != 0;
        else if (rc != SQLITE_DONE) throw std::runtime_error("cannot read latest alarm state");
        latest.reset();
        if (sqlite3_open_v2(c.database.c_str(), &reader_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot open database reader");
        sqlite3_busy_timeout(reader_, 50);
        worker_ = std::thread(&Storage::run, this);
    } catch (...) {
        if (reader_) sqlite3_close(reader_);
        if (writer_) sqlite3_close(writer_);
        throw;
    }
}
Storage::~Storage() {
    stop();
    if (reader_) sqlite3_close(reader_);
    if (writer_) sqlite3_close(writer_);
}
void Storage::stop() { queue_.close(); if (worker_.joinable()) worker_.join(); }
bool Storage::submit(Sample s) {
    if (!queue_.push(s)) { ++dropped; return false; }
    return true;
}
void Storage::run() noexcept {
    bool active = initial_alarm_;
    Sample s;
    while (queue_.pop(s)) {
        try {
            exec(writer_, "BEGIN IMMEDIATE");
            auto stmt = prepare(writer_, "INSERT INTO samples(timestamp_ms,temperature_c,humidity_pct,alarm) VALUES(?,?,?,?)");
            sqlite3_bind_int64(stmt.get(), 1, s.timestamp_ms);
            sqlite3_bind_double(stmt.get(), 2, s.temperature_c);
            sqlite3_bind_double(stmt.get(), 3, s.humidity_pct);
            sqlite3_bind_int(stmt.get(), 4, s.alarm ? 1 : 0);
            done(writer_, stmt.get());
            const auto id = sqlite3_last_insert_rowid(writer_);
            if (s.alarm != active) {
                auto event = prepare(writer_, "INSERT INTO alarms(sample_id,timestamp_ms,active,temperature_c) VALUES(?,?,?,?)");
                sqlite3_bind_int64(event.get(), 1, id);
                sqlite3_bind_int64(event.get(), 2, s.timestamp_ms);
                sqlite3_bind_int(event.get(), 3, s.alarm ? 1 : 0);
                sqlite3_bind_double(event.get(), 4, s.temperature_c);
                done(writer_, event.get());
            }
            // Independent limits on sample rows and alarm rows; monotonic ids survive restart.
            const auto cap = std::to_string(max_rows_);
            exec(writer_, "DELETE FROM samples WHERE id <= (SELECT MAX(id)-" + cap + " FROM samples);"
                "DELETE FROM alarms WHERE id <= (SELECT MAX(id)-" + cap + " FROM alarms); COMMIT;");
            active = s.alarm;
            ++persisted;
            last_write_ok = true;
        } catch (const std::exception& e) {
            sqlite3_exec(writer_, "ROLLBACK", nullptr, nullptr, nullptr);
            ++errors;
            last_write_ok = false;
            std::cerr << "level=error event=storage_failed message=\"" << json_escape(e.what()) << "\"\n";
        }
    }
    sqlite3_wal_checkpoint_v2(writer_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
}
std::string Storage::history(int limit) {
    auto stmt = prepare(reader_, "SELECT id,timestamp_ms,temperature_c,humidity_pct,alarm FROM samples ORDER BY id DESC LIMIT ?");
    sqlite3_bind_int(stmt.get(), 1, limit);
    std::string out = "[";
    int rc;
    bool first = true;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        if (!first) out += ',';
        first = false;
        Sample s{sqlite3_column_int64(stmt.get(), 1), sqlite3_column_double(stmt.get(), 2), sqlite3_column_double(stmt.get(), 3), sqlite3_column_int(stmt.get(), 4) != 0, 0};
        out += sample_json(s, sqlite3_column_int64(stmt.get(), 0));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error("history query failed");
    return out + ']';
}
std::string Storage::alarms(int limit) {
    auto stmt = prepare(reader_, "SELECT id,sample_id,timestamp_ms,active,temperature_c FROM alarms ORDER BY id DESC LIMIT ?");
    sqlite3_bind_int(stmt.get(), 1, limit);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '[';
    int rc;
    bool first = true;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << sqlite3_column_int64(stmt.get(), 0) << ",\"sample_id\":" << sqlite3_column_int64(stmt.get(), 1)
            << ",\"timestamp_ms\":" << sqlite3_column_int64(stmt.get(), 2) << ",\"active\":"
            << (sqlite3_column_int(stmt.get(), 3) ? "true" : "false") << ",\"temperature_c\":" << sqlite3_column_double(stmt.get(), 4) << '}';
    }
    if (rc != SQLITE_DONE) throw std::runtime_error("alarm query failed");
    return out.str() + ']';
}
}
