#pragma once

#include "event.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Binary event log format:
//   [event_type : uint8_t]
//   [payload_size : uint32_t]
//   [payload bytes ...]
//
// Timestamp is stored as int64_t microseconds since epoch.
// Strings are stored as [uint16_t length][bytes...].

namespace event_serial {

// --- Low-level helpers ---

inline void write_u8(std::ostream& out, uint8_t v)  { out.write(reinterpret_cast<const char*>(&v), 1); }
inline void write_u16(std::ostream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
inline void write_u32(std::ostream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
inline void write_u64(std::ostream& out, uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_i64(std::ostream& out, int64_t v)  { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_f64(std::ostream& out, double v)   { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_i32(std::ostream& out, int32_t v)  { out.write(reinterpret_cast<const char*>(&v), 4); }

inline void write_str(std::ostream& out, const std::string& s)
{
    auto len = static_cast<uint16_t>(s.size());
    write_u16(out, len);
    out.write(s.data(), len);
}

inline void write_ts(std::ostream& out, std::chrono::system_clock::time_point tp)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    write_i64(out, us);
}

// --- Readers (from raw buffer) ---

class BufReader {
    const uint8_t* p_;
    const uint8_t* end_;
public:
    BufReader(const uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}

    void ensure(std::size_t n) const
    {
        if (p_ + n > end_)
            throw std::runtime_error("event_log: unexpected end of payload");
    }

    uint8_t  read_u8()  { ensure(1); uint8_t  v; std::memcpy(&v, p_, 1); p_ += 1; return v; }
    uint16_t read_u16() { ensure(2); uint16_t v; std::memcpy(&v, p_, 2); p_ += 2; return v; }
    uint32_t read_u32() { ensure(4); uint32_t v; std::memcpy(&v, p_, 4); p_ += 4; return v; }
    uint64_t read_u64() { ensure(8); uint64_t v; std::memcpy(&v, p_, 8); p_ += 8; return v; }
    int64_t  read_i64() { ensure(8); int64_t  v; std::memcpy(&v, p_, 8); p_ += 8; return v; }
    int32_t  read_i32() { ensure(4); int32_t  v; std::memcpy(&v, p_, 4); p_ += 4; return v; }
    double   read_f64() { ensure(8); double   v; std::memcpy(&v, p_, 8); p_ += 8; return v; }

    std::string read_str()
    {
        auto len = read_u16();
        ensure(len);
        std::string s(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return s;
    }

    std::chrono::system_clock::time_point read_ts()
    {
        auto us = read_i64();
        return std::chrono::system_clock::time_point(std::chrono::microseconds(us));
    }
};

// --- Serialise individual event types into a buffer ---

inline std::vector<uint8_t> serialise(const market_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(128);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_i64 = [&](int64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_f64(e.get_open());
    append_f64(e.get_high());
    append_f64(e.get_low());
    append_f64(e.get_close());
    append_i64(e.get_volume());
    return buf;
}

inline std::vector<uint8_t> serialise(const signal_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(64);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    uint8_t sig = static_cast<uint8_t>(e.get_signal());
    append(&sig, 1);
    append_f64(e.get_strength());
    return buf;
}

inline std::vector<uint8_t> serialise(const order_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(128);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    uint8_t ot = static_cast<uint8_t>(e.get_order_type());
    append(&ot, 1);
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    append_f64(e.get_quantity());
    append_f64(e.get_price());
    uint8_t tif = static_cast<uint8_t>(e.get_tif());
    append(&tif, 1);
    append_f64(e.get_stop_price());
    append_u64(e.get_order_id());
    append_ts(e.get_earliest_eligible_ts());
    return buf;
}

inline std::vector<uint8_t> serialise(const fill_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(96);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u64(e.get_order_id());
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    append_f64(e.get_filled_quantity());
    append_f64(e.get_fill_price());
    append_f64(e.get_commission());
    return buf;
}

inline std::vector<uint8_t> serialise(const tick_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(64);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_i64 = [&](int64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_f64(e.get_price());
    append_i64(e.get_quantity());
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    return buf;
}

inline std::vector<uint8_t> serialise(const l2_snapshot_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(256);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_i64 = [&](int64_t v) { append(&v, 8); };
    auto append_u32 = [&](uint32_t v) { append(&v, 4); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u32(static_cast<uint32_t>(e.get_bids().size()));
    for (const auto& lvl : e.get_bids()) {
        append_f64(lvl.price);
        append_i64(lvl.quantity);
    }
    append_u32(static_cast<uint32_t>(e.get_asks().size()));
    for (const auto& lvl : e.get_asks()) {
        append_f64(lvl.price);
        append_i64(lvl.quantity);
    }
    return buf;
}

inline std::vector<uint8_t> serialise(const l2_update_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(64);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_i64 = [&](int64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    append_f64(e.get_price());
    append_i64(e.get_new_quantity());
    return buf;
}

// --- Deserialise from buffer ---

inline event_pointer deserialise(event_type type, const uint8_t* data, std::size_t size)
{
    BufReader r(data, size);

    switch (type) {
    case event_type::market: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double open = r.read_f64();
        double high = r.read_f64();
        double low = r.read_f64();
        double close = r.read_f64();
        int64_t volume = r.read_i64();
        return std::make_shared<market_event>(ts, symbol, open, high, low, close, volume);
    }
    case event_type::signal: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto sig = static_cast<signal_type>(r.read_u8());
        double strength = r.read_f64();
        return std::make_shared<signal_event>(ts, symbol, sig, strength);
    }
    case event_type::order: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto ot = static_cast<order_type>(r.read_u8());
        auto sd = static_cast<order_side>(r.read_u8());
        double qty = r.read_f64();
        double price = r.read_f64();
        auto tif = static_cast<time_in_force>(r.read_u8());
        double stop_price = r.read_f64();
        uint64_t oid = r.read_u64();
        auto elig_ts = r.read_ts();
        auto ev = std::make_shared<order_event>(ts, symbol, ot, sd, qty, price, tif, stop_price);
        ev->set_order_id(oid);
        ev->set_earliest_eligible_ts(elig_ts);
        return ev;
    }
    case event_type::fill: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto sd = static_cast<order_side>(r.read_u8());
        double qty = r.read_f64();
        double price = r.read_f64();
        double commission = r.read_f64();
        return std::make_shared<fill_event>(ts, symbol, oid, sd, qty, price, commission);
    }
    case event_type::tick: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double price = r.read_f64();
        int64_t qty = r.read_i64();
        auto sd = static_cast<tick_side>(r.read_u8());
        return std::make_shared<tick_event>(ts, symbol, price, qty, sd);
    }
    case event_type::l2_snapshot: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint32_t n_bids = r.read_u32();
        std::vector<l2_level> bids(n_bids);
        for (uint32_t i = 0; i < n_bids; ++i) {
            bids[i].price = r.read_f64();
            bids[i].quantity = r.read_i64();
        }
        uint32_t n_asks = r.read_u32();
        std::vector<l2_level> asks(n_asks);
        for (uint32_t i = 0; i < n_asks; ++i) {
            asks[i].price = r.read_f64();
            asks[i].quantity = r.read_i64();
        }
        return std::make_shared<l2_snapshot_event>(ts, symbol, std::move(bids), std::move(asks));
    }
    case event_type::l2_update: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto sd = static_cast<tick_side>(r.read_u8());
        double price = r.read_f64();
        int64_t new_qty = r.read_i64();
        return std::make_shared<l2_update_event>(ts, symbol, sd, price, new_qty);
    }
    }
    throw std::runtime_error("event_log: unknown event type " + std::to_string(static_cast<int>(type)));
}

} // namespace event_serial


// Writes events to a binary log file.
class EventLogger
{
public:
    explicit EventLogger(const std::string& path)
        : out_(path, std::ios::binary | std::ios::trunc)
    {
        if (!out_)
            throw std::runtime_error("EventLogger: cannot open " + path);
    }

    void log(const event& e)
    {
        std::vector<uint8_t> payload;

        switch (e.get_type()) {
        case event_type::market:
            payload = event_serial::serialise(static_cast<const market_event&>(e)); break;
        case event_type::signal:
            payload = event_serial::serialise(static_cast<const signal_event&>(e)); break;
        case event_type::order:
            payload = event_serial::serialise(static_cast<const order_event&>(e)); break;
        case event_type::fill:
            payload = event_serial::serialise(static_cast<const fill_event&>(e)); break;
        case event_type::tick:
            payload = event_serial::serialise(static_cast<const tick_event&>(e)); break;
        case event_type::l2_snapshot:
            payload = event_serial::serialise(static_cast<const l2_snapshot_event&>(e)); break;
        case event_type::l2_update:
            payload = event_serial::serialise(static_cast<const l2_update_event&>(e)); break;
        }

        event_serial::write_u8(out_, static_cast<uint8_t>(e.get_type()));
        event_serial::write_u32(out_, static_cast<uint32_t>(payload.size()));
        out_.write(reinterpret_cast<const char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
    }

    void flush() { out_.flush(); }

private:
    std::ofstream out_;
};


// Reads events back from a binary log file.
class EventReplayer
{
public:
    explicit EventReplayer(const std::string& path)
        : in_(path, std::ios::binary)
    {
        if (!in_)
            throw std::runtime_error("EventReplayer: cannot open " + path);
    }

    bool has_next() const
    {
        return in_.good() && in_.peek() != std::ifstream::traits_type::eof();
    }

    event_pointer next()
    {
        uint8_t type_byte = 0;
        in_.read(reinterpret_cast<char*>(&type_byte), 1);
        if (!in_) return nullptr;

        uint32_t payload_size = 0;
        in_.read(reinterpret_cast<char*>(&payload_size), 4);
        if (!in_) return nullptr;

        std::vector<uint8_t> payload(payload_size);
        in_.read(reinterpret_cast<char*>(payload.data()), payload_size);
        if (!in_) return nullptr;

        return event_serial::deserialise(
            static_cast<event_type>(type_byte), payload.data(), payload_size);
    }

private:
    mutable std::ifstream in_;
};
