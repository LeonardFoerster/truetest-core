#pragma once

#include "event.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <zstd.h>


namespace event_serial {


inline void write_u8(std::ostream& out, uint8_t v)  { out.write(reinterpret_cast<const char*>(&v), 1); }
inline void write_u16(std::ostream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
inline void write_u32(std::ostream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
inline void write_u64(std::ostream& out, uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_i64(std::ostream& out, int64_t v)  { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_f64(std::ostream& out, double v)   { out.write(reinterpret_cast<const char*>(&v), 8); }
inline void write_i32(std::ostream& out, int32_t v)  { out.write(reinterpret_cast<const char*>(&v), 4); }

inline uint16_t checked_string_length(std::string_view value)
{
    if (value.size() > std::numeric_limits<uint16_t>::max())
        throw std::length_error("event_log: string field exceeds 65535 bytes");
    return static_cast<uint16_t>(value.size());
}

inline uint32_t checked_u32_length(std::size_t value, std::string_view field)
{
    if (value > std::numeric_limits<uint32_t>::max())
        throw std::length_error("event_log: " + std::string(field) +
                                " exceeds uint32 format limit");
    return static_cast<uint32_t>(value);
}

inline void write_str(std::ostream& out, const std::string& s)
{
    const auto len = checked_string_length(s);
    write_u16(out, len);
    out.write(s.data(), len);
}

inline void write_ts(std::ostream& out, std::chrono::system_clock::time_point tp)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    write_i64(out, us);
}


class BufReader {
    const uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;

    template <typename T>
    T read_value()
    {
        ensure(sizeof(T));
        T value;
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

public:
    BufReader(const uint8_t* data, std::size_t size) : data_(data), size_(size)
    {
        if (data_ == nullptr && size_ != 0)
            throw std::invalid_argument("event_log: null payload data");
    }

    void ensure(std::size_t n) const
    {
        if (n > size_ - offset_)
            throw std::runtime_error("event_log: unexpected end of payload");
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return size_ - offset_;
    }

    void require_consumed() const
    {
        if (remaining() != 0)
            throw std::runtime_error("event_log: trailing payload bytes");
    }

    uint8_t  read_u8()  { return read_value<uint8_t>(); }
    uint16_t read_u16() { return read_value<uint16_t>(); }
    uint32_t read_u32() { return read_value<uint32_t>(); }
    uint64_t read_u64() { return read_value<uint64_t>(); }
    int64_t  read_i64() { return read_value<int64_t>(); }
    int32_t  read_i32() { return read_value<int32_t>(); }
    double   read_f64() { return read_value<double>(); }

    std::string read_str()
    {
        auto len = read_u16();
        ensure(len);
        if (len == 0) return {};
        std::string s(reinterpret_cast<const char*>(data_ + offset_), len);
        offset_ += len;
        return s;
    }

    std::chrono::system_clock::time_point read_ts()
    {
        auto us = read_i64();
        const auto min_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::time_point::min().time_since_epoch()).count();
        const auto max_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::time_point::max().time_since_epoch()).count();
        if (us < min_us || us > max_us)
            throw std::runtime_error("event_log: timestamp out of range");
        return std::chrono::system_clock::time_point(std::chrono::microseconds(us));
    }
};

// All persisted enums currently use contiguous zero-based values. Keep the
// wire parser fail-closed rather than materialising an invalid enum value.
template <typename Enum>
inline Enum read_enum(BufReader& reader, Enum last_value, std::string_view field)
{
    const auto value = reader.read_u8();
    if (value > static_cast<uint8_t>(last_value))
        throw std::runtime_error("event_log: invalid " + std::string(field));
    return static_cast<Enum>(value);
}


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
    auto append_str = [&](std::string_view s) {
        const uint16_t len = checked_string_length(s);
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
        const uint16_t len = checked_string_length(s);
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
        const uint16_t len = checked_string_length(s);
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
        const uint16_t len = checked_string_length(s);
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
    append_f64(e.get_remaining_qty());
    append_u64(e.get_fill_id());
    uint8_t src = static_cast<uint8_t>(e.get_source());
    append(&src, 1);
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
        const uint16_t len = checked_string_length(s);
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
        const uint16_t len = checked_string_length(s);
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };
    auto append_i64 = [&](int64_t v) { append(&v, 8); };
    auto append_u32 = [&](uint32_t v) { append(&v, 4); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u32(static_cast<uint32_t>(e.bid_count()));
    for (std::size_t i = 0; i < e.bid_count(); ++i) {
        append_f64(e.bid(i).price);
        append_i64(e.bid(i).quantity);
    }
    append_u32(static_cast<uint32_t>(e.ask_count()));
    for (std::size_t i = 0; i < e.ask_count(); ++i) {
        append_f64(e.ask(i).price);
        append_i64(e.ask(i).quantity);
    }
    return buf;
}

inline std::vector<uint8_t> serialise(const cancel_event& e)
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
        const uint16_t len = checked_string_length(s);
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u64(e.get_order_id());
    append_str(e.get_reason());
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
        const uint16_t len = checked_string_length(s);
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

inline std::vector<uint8_t> serialise(const amend_event& e)
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
        const uint16_t len = checked_string_length(s);
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };
    auto append_f64 = [&](double v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u64(e.get_order_id());
    append_f64(e.get_new_price());
    append_f64(e.get_new_quantity());
    return buf;
}

inline std::vector<uint8_t> serialise(const rejection_event& e)
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
        const uint16_t len = checked_string_length(s);
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_u64(e.get_order_id());
    append_str(e.get_reason());
    return buf;
}

inline std::vector<uint8_t> serialise(const funding_event& e)
{
    std::vector<uint8_t> buf;
    buf.reserve(80);
    auto append = [&](const void* ptr, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(ptr);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_ts = [&](std::chrono::system_clock::time_point tp) {
        int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
        append(&us, 8);
    };
    auto append_str = [&](std::string_view s) {
        const uint16_t len = checked_string_length(s);
        append(&len, 2);
        append(s.data(), len);
    };
    auto append_f64 = [&](double v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_f64(e.get_qty_change());
    append_f64(e.get_cash_delta());
    append_str(e.get_reason());
    return buf;
}


inline event_pointer deserialise(event_type type, const uint8_t* data, std::size_t size)
{
    BufReader r(data, size);
    auto finish = [&r](event_pointer ev) {
        r.require_consumed();
        return ev;
    };

    switch (type) {
    case event_type::market: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double open = r.read_f64();
        double high = r.read_f64();
        double low = r.read_f64();
        double close = r.read_f64();
        int64_t volume = r.read_i64();
        return finish(std::make_shared<market_event>(
            ts, symbol, open, high, low, close, volume));
    }
    case event_type::signal: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto sig = read_enum(r, signal_type::hold, "signal type");
        double strength = r.read_f64();
        return finish(std::make_shared<signal_event>(ts, symbol, sig, strength));
    }
    case event_type::order: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto ot = read_enum(r, order_type::stop_limit, "order type");
        auto sd = read_enum(r, order_side::sell, "order side");
        double qty = r.read_f64();
        double price = r.read_f64();
        auto tif = read_enum(r, time_in_force::day, "time in force");
        double stop_price = r.read_f64();
        uint64_t oid = r.read_u64();
        auto elig_ts = r.read_ts();
        auto ev = std::make_shared<order_event>(ts, symbol, ot, sd, qty, price, tif, stop_price);
        ev->set_order_id(oid);
        ev->set_earliest_eligible_ts(elig_ts);
        return finish(std::move(ev));
    }
    case event_type::fill: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto sd = read_enum(r, order_side::sell, "order side");
        double qty = r.read_f64();
        double price = r.read_f64();
        double commission = r.read_f64();
        double remaining = 0.0;
        uint64_t fill_id = 0;
        fill_source src = fill_source::unknown;
        // The original fill wire shape ended after commission. It was then
        // extended with remaining_qty + fill_id, and later fill_source. Only
        // those complete historic forms are valid; a partial extension must
        // not be mistaken for a legacy record.
        switch (r.remaining()) {
        case 0:
            break;
        case 16:
            remaining = r.read_f64();
            fill_id = r.read_u64();
            break;
        case 17:
            remaining = r.read_f64();
            fill_id = r.read_u64();
            src = read_enum(r, fill_source::exchange, "fill source");
            break;
        default:
            throw std::runtime_error("event_log: invalid fill extension length");
        }
        auto ev = std::make_shared<fill_event>(ts, symbol, oid, sd, qty, price,
                                               commission, remaining, fill_id);
        ev->set_source(src);
        return finish(std::move(ev));
    }
    case event_type::tick: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double price = r.read_f64();
        int64_t qty = r.read_i64();
        auto sd = read_enum(r, tick_side::unknown, "tick side");
        return finish(std::make_shared<tick_event>(ts, symbol, price, qty, sd));
    }
    case event_type::l2_snapshot: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        const std::size_t n_bids = r.read_u32();
        std::array<l2_level, kL2SnapshotMaxLevels> bids{};
        const std::size_t bid_n = std::min<std::size_t>(n_bids, kL2SnapshotMaxLevels);
        for (std::size_t i = 0; i < bid_n; ++i) {
            bids[i].price = r.read_f64();
            bids[i].quantity = r.read_i64();
        }
        for (std::size_t i = bid_n; i < n_bids; ++i) {
            (void)r.read_f64();
            (void)r.read_i64();
        }
        const std::size_t n_asks = r.read_u32();
        std::array<l2_level, kL2SnapshotMaxLevels> asks{};
        const std::size_t ask_n = std::min<std::size_t>(n_asks, kL2SnapshotMaxLevels);
        for (std::size_t i = 0; i < ask_n; ++i) {
            asks[i].price = r.read_f64();
            asks[i].quantity = r.read_i64();
        }
        for (std::size_t i = ask_n; i < n_asks; ++i) {
            (void)r.read_f64();
            (void)r.read_i64();
        }
        return finish(std::make_shared<l2_snapshot_event>(
            ts, symbol, bids.data(), bid_n, asks.data(), ask_n));
    }
    case event_type::l2_update: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto sd = read_enum(r, tick_side::unknown, "tick side");
        double price = r.read_f64();
        int64_t new_qty = r.read_i64();
        return finish(std::make_shared<l2_update_event>(
            ts, symbol, sd, price, new_qty));
    }
    case event_type::cancel: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto reason = r.read_str();
        return finish(std::make_shared<cancel_event>(ts, symbol, oid, reason));
    }
    case event_type::amend: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        double new_price = r.read_f64();
        double new_qty = r.read_f64();
        return finish(std::make_shared<amend_event>(
            ts, symbol, oid, new_price, new_qty));
    }
    case event_type::rejection: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto reason = r.read_str();
        return finish(std::make_shared<rejection_event>(ts, symbol, oid, reason));
    }
    case event_type::funding: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double qty = r.read_f64();
        double cash = r.read_f64();
        auto reason = r.read_str();
        return finish(std::make_shared<funding_event>(
            ts, symbol, qty, cash, reason));
    }
    }
    throw std::runtime_error("event_log: unknown event type " + std::to_string(static_cast<int>(type)));
}

}

struct EventLogIndexEntry {
    int64_t  timestamp_us;
    uint64_t file_offset;
};

static constexpr uint32_t EVENT_LOG_INDEX_MAGIC = 0x58495454;
static constexpr size_t   EVENT_LOG_INDEX_INTERVAL = 1000;
static constexpr size_t   EVENT_LOG_MAX_INDEX_ENTRIES = 1'000'000;
// The leading 0xFF cannot be a legacy event_type (which is currently 0..10),
// so new files are unambiguously distinguishable from headerless logs.
static constexpr std::array<uint8_t, 4> EVENT_LOG_FILE_MAGIC{
    0xFF, static_cast<uint8_t>('T'), static_cast<uint8_t>('T'), static_cast<uint8_t>('L')};
static constexpr uint8_t EVENT_LOG_FILE_VERSION = 1;
static constexpr uint8_t EVENT_LOG_FILE_FLAG_ZSTD = 1U << 0;
static constexpr uint8_t EVENT_LOG_FILE_FLAG_FINALIZED = 1U << 1;
// Set from the first descriptor-backed write.  A live ledger carrying this
// bit is deliberately unreplayable until publication adds the companion
// PUBLISHED bit after every durable filesystem step has succeeded.
static constexpr uint8_t EVENT_LOG_FILE_FLAG_AUTHORITATIVE = 1U << 2;
static constexpr uint8_t EVENT_LOG_FILE_FLAG_AUTHORITATIVE_PUBLISHED = 1U << 3;
static constexpr uint8_t EVENT_LOG_FILE_KNOWN_FLAGS =
    EVENT_LOG_FILE_FLAG_ZSTD | EVENT_LOG_FILE_FLAG_FINALIZED
    | EVENT_LOG_FILE_FLAG_AUTHORITATIVE
    | EVENT_LOG_FILE_FLAG_AUTHORITATIVE_PUBLISHED;
static constexpr std::streamoff EVENT_LOG_FILE_PREAMBLE_BYTES = 6;

#if defined(__linux__)
namespace event_log_detail {

[[noreturn]] inline void throw_errno(std::string_view operation)
{
    const int error = errno;
    throw std::runtime_error(std::string(operation) + ": "
                             + std::strerror(error));
}

inline void split_authoritative_ledger_path(const std::string& path,
                                            std::string& directory,
                                            std::string& name)
{
    if (path.empty() || path.find('\0') != std::string::npos)
        throw std::invalid_argument("authoritative event ledger path is invalid");
    const auto slash = path.find_last_of('/');
    directory = slash == std::string::npos ? "."
              : slash == 0 ? "/" : path.substr(0, slash);
    name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty() || name == "." || name == ".."
        || name.find('/') != std::string::npos)
        throw std::invalid_argument(
            "authoritative event ledger requires a concrete file name");
}

// The reservation keeps a directory descriptor, but the published ledger is
// later consumed by pathname.  Requiring a directory private to the effective
// uid prevents another local account from replacing that pathname between the
// publication protocol and replay.  This deliberately rejects shared runtime
// directories such as /tmp; live callers must provision a private state dir.
inline void require_private_authoritative_directory(int directory_fd)
{
    struct stat status{};
    if (::fstat(directory_fd, &status) != 0)
        throw_errno("stat authoritative event ledger directory");
    if (!S_ISDIR(status.st_mode))
        throw std::runtime_error(
            "authoritative event ledger parent is not a directory");
    if (status.st_uid != ::geteuid()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw std::runtime_error(
            "authoritative event ledger parent must be private to the effective uid");
}

inline int open_private_authoritative_directory(const std::string& path,
                                                std::string& name)
{
    std::string directory;
    split_authoritative_ledger_path(path, directory, name);

    open_how how{};
    how.flags = static_cast<std::uint64_t>(O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    how.resolve = static_cast<std::uint64_t>(RESOLVE_NO_SYMLINKS
                                             | RESOLVE_NO_MAGICLINKS);
    const int directory_fd = static_cast<int>(::syscall(
        SYS_openat2, AT_FDCWD, directory.c_str(), &how, sizeof(how)));
    if (directory_fd < 0)
        throw_errno("open authoritative event ledger directory");
    try
    {
        require_private_authoritative_directory(directory_fd);
    }
    catch (...)
    {
        (void)::close(directory_fd);
        throw;
    }
    return directory_fd;
}

inline int open_regular_event_log_for_replay(const std::string& path)
{
    // This initial descriptor only identifies the file envelope.  It stays
    // compatible with historical replay paths; an authoritative envelope is
    // always reopened below through the stricter private openat2 path before
    // any authoritative contents are accepted.
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_fd < 0)
        throw_errno("open event log for replay");

    struct stat status{};
    if (::fstat(file_fd, &status) != 0)
    {
        const int error = errno;
        (void)::close(file_fd);
        errno = error;
        throw_errno("validate event log for replay");
    }
    if (!S_ISREG(status.st_mode))
    {
        (void)::close(file_fd);
        throw std::runtime_error("EventReplayer: source is not a regular file");
    }
    return file_fd;
}

inline int open_private_authoritative_ledger_for_replay(
    const std::string& path)
{
    std::string name;
    const int directory_fd = open_private_authoritative_directory(path, name);
    open_how how{};
    how.flags = static_cast<std::uint64_t>(
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    how.resolve = static_cast<std::uint64_t>(RESOLVE_BENEATH
                                             | RESOLVE_NO_SYMLINKS
                                             | RESOLVE_NO_MAGICLINKS);
    const int file_fd = static_cast<int>(::syscall(
        SYS_openat2, directory_fd, name.c_str(), &how, sizeof(how)));
    const int open_error = errno;
    (void)::close(directory_fd);
    if (file_fd < 0)
    {
        errno = open_error;
        throw_errno("open authoritative event ledger for replay");
    }

    struct stat status{};
    if (::fstat(file_fd, &status) != 0)
    {
        const int error = errno;
        (void)::close(file_fd);
        errno = error;
        throw_errno("validate authoritative event ledger for replay");
    }
    if (!S_ISREG(status.st_mode)
        || status.st_nlink != 1
        || status.st_uid != ::geteuid()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    {
        (void)::close(file_fd);
        throw std::runtime_error(
            "authoritative event ledger replay file is not private regular data");
    }
    return file_fd;
}

inline void require_no_authoritative_partial_sibling(const std::string& path)
{
    std::string name;
    const int directory_fd = open_private_authoritative_directory(path, name);
    const std::string partial_name = name + ".partial";
    struct stat sibling_status{};
    const int result = ::fstatat(directory_fd, partial_name.c_str(),
                                 &sibling_status, AT_SYMLINK_NOFOLLOW);
    const int error = errno;
    (void)::close(directory_fd);
    if (result == 0)
        throw std::runtime_error(
            "EventReplayer: ledger has an unresolved partial sibling");
    if (error != ENOENT)
    {
        errno = error;
        throw_errno("inspect authoritative event ledger partial sibling");
    }
}

} // namespace event_log_detail
#endif

// A live ledger is not a convenience log.  It is reserved before provider
// open through directory/file descriptors so a symlink, existing target, or
// stale partial cannot be mistaken for the session's authoritative truth.
// The normal EventLogger compatibility constructor remains available for
// backtests and existing replay fixtures; live composition uses this move-only
// reservation and finalizes it only after the event stream itself is durable.
class AuthoritativeEventLedgerReservation
{
public:
    explicit AuthoritativeEventLedgerReservation(std::string final_path)
        : final_path_(std::move(final_path))
    {
#if defined(__linux__)
        reserve();
#else
        throw std::runtime_error(
            "authoritative event ledger requires the supported POSIX openat2 path");
#endif
    }

    ~AuthoritativeEventLedgerReservation() noexcept { close_noexcept(); }

    AuthoritativeEventLedgerReservation(
        const AuthoritativeEventLedgerReservation&) = delete;
    AuthoritativeEventLedgerReservation& operator=(
        const AuthoritativeEventLedgerReservation&) = delete;

    AuthoritativeEventLedgerReservation(
        AuthoritativeEventLedgerReservation&& other) noexcept
    {
        move_from(std::move(other));
    }

    AuthoritativeEventLedgerReservation& operator=(
        AuthoritativeEventLedgerReservation&& other) noexcept
    {
        if (this != &other)
        {
            close_noexcept();
            move_from(std::move(other));
        }
        return *this;
    }

    [[nodiscard]] int data_fd() const noexcept { return data_fd_; }
    [[nodiscard]] bool reserved() const noexcept
    {
        return data_fd_ >= 0 && directory_fd_ >= 0 && !published_;
    }
    [[nodiscard]] bool published() const noexcept { return published_; }
    [[nodiscard]] const std::string& final_path() const noexcept
    {
        return final_path_;
    }
    [[nodiscard]] const std::string& partial_path() const noexcept
    {
        return partial_path_;
    }

    void sync_data()
    {
#if defined(__linux__)
        if (!reserved())
            throw std::logic_error("authoritative event ledger is not writable");
        if (::fdatasync(data_fd_) != 0)
            throw_errno("fdatasync event ledger");
#else
        throw std::runtime_error(
            "authoritative event ledger requires the supported POSIX openat2 path");
#endif
    }

    // The caller must flush its descriptor-backed stream before this method.
    // Failure deliberately leaves the `.partial` file in place as forensic
    // evidence; it is never promoted to an authoritative replay file.
    //
    // Do not rename `partial_name_` by pathname here.  The reservation owns
    // the inode through data_fd_, but a hostile writer of the containing
    // directory could otherwise rename that pathname after reserve() and
    // before publication.  Publish a new final name from /proc/self/fd/N
    // instead, so the kernel links the inode we validated and synced rather
    // than whichever inode currently has the `.partial` spelling.
    void finalize(uint8_t published_file_flags)
    {
#if defined(__linux__)
        if (!reserved())
            throw std::logic_error(
                "authoritative event ledger is not an unpublished reservation");
        // The descriptor pins the original directory inode, but replay later
        // resolves final_path_ again.  Refuse publication if that directory
        // no longer satisfies the private-parent invariant.
        event_log_detail::require_private_authoritative_directory(directory_fd_);
        constexpr uint8_t required_published_flags =
            EVENT_LOG_FILE_FLAG_FINALIZED
            | EVENT_LOG_FILE_FLAG_AUTHORITATIVE
            | EVENT_LOG_FILE_FLAG_AUTHORITATIVE_PUBLISHED;
        if ((published_file_flags & required_published_flags)
            != required_published_flags)
            throw std::logic_error(
                "authoritative event ledger has invalid publication flags");
        if (::fdatasync(data_fd_) != 0)
            throw_errno("fdatasync event ledger");
        char descriptor_path[64]{};
        const auto descriptor_path_size = std::snprintf(
            descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d",
            data_fd_);
        if (descriptor_path_size < 0
            || static_cast<std::size_t>(descriptor_path_size)
                >= sizeof(descriptor_path))
            throw std::runtime_error("format event ledger descriptor path");
        if (::linkat(AT_FDCWD, descriptor_path, directory_fd_,
                     final_name_.c_str(), AT_SYMLINK_FOLLOW) != 0)
            throw_errno("publish event ledger");

        // First make the final name durable while retaining the forensic
        // `.partial` name.  If this sync fails, the ledger's on-disk header
        // still lacks AUTHORITATIVE_PUBLISHED and EventReplayer rejects the
        // final spelling even if a hostile directory writer removed the
        // sibling partial.
        if (::fsync(directory_fd_) != 0)
            throw_errno("fsync published event ledger directory");

        // Mark the exact descriptor inode as published while the forensic
        // sibling still exists.  A failed marker sync therefore remains
        // unreplayable regardless of any later path cleanup outcome.
        constexpr off_t flags_offset =
            static_cast<off_t>(EVENT_LOG_FILE_MAGIC.size() + 1U);
        const auto written = ::pwrite(data_fd_, &published_file_flags,
                                      sizeof(published_file_flags),
                                      flags_offset);
        if (written < 0)
            throw_errno("mark authoritative event ledger published");
        if (written != static_cast<ssize_t>(sizeof(published_file_flags)))
            throw std::runtime_error(
                "short write marking authoritative event ledger published");
        if (::fdatasync(data_fd_) != 0)
            throw_errno("fdatasync published event ledger marker");

        // The final name above refers to the validated descriptor.  The
        // reservation's partial name must disappear before replay accepts
        // it.  ENOENT is also safe: the final link and its marker are already
        // durable, and retaining a stale error would otherwise falsely claim
        // that the valid final ledger was unpublished.  Any other failure
        // leaves the partial in place (or fails closed via the marker guard).
        if (::unlinkat(directory_fd_, partial_name_.c_str(), 0) != 0
            && errno != ENOENT)
            throw_errno("remove event ledger partial");
        published_ = true;
        // fdatasync and the first directory fsync above already made the
        // authoritative name durable.  POSIX close may report a delayed
        // error after that point, but treating it as a failed publication
        // would leave callers believing a valid final ledger was invalid.
        // Retire the descriptor best-effort instead of manufacturing that
        // contradictory failure state.
        close_data_noexcept();
#else
        throw std::runtime_error(
            "authoritative event ledger requires the supported POSIX openat2 path");
#endif
    }

    // Deliberately leave the descriptor-reserved `.partial` in place without
    // publishing it.  This is used when live engine construction fails after
    // reservation but before a run can establish a coherent authoritative
    // event stream.  A partial is forensic evidence, never replay input.
    void abandon() noexcept
    {
        if (published_) return;
        close_noexcept();
    }

private:
    std::string final_path_;
    std::string partial_path_;
    std::string final_name_;
    std::string partial_name_;
    int directory_fd_ = -1;
    int data_fd_ = -1;
    bool published_ = false;

#if defined(__linux__)
    [[noreturn]] static void throw_errno(std::string_view operation)
    {
        event_log_detail::throw_errno(operation);
    }

    void reserve()
    {
        directory_fd_ = event_log_detail::open_private_authoritative_directory(
            final_path_, final_name_);
        partial_name_ = final_name_ + ".partial";
        const auto slash = final_path_.find_last_of('/');
        const std::string directory = slash == std::string::npos ? "."
            : slash == 0 ? "/" : final_path_.substr(0, slash);
        partial_path_ = (directory == "/" ? directory : directory + "/")
                      + partial_name_;

        // A session may only publish the final path it reserved before the
        // provider opens.  Starting with an existing final would defer the
        // collision until shutdown, leaving the new session without a
        // publishable authoritative audit record.
        struct stat final_status{};
        if (::fstatat(directory_fd_, final_name_.c_str(), &final_status,
                      AT_SYMLINK_NOFOLLOW) == 0)
        {
            close_noexcept();
            throw std::runtime_error(
                "authoritative event ledger final path already exists");
        }
        if (errno != ENOENT)
        {
            const int error = errno;
            close_noexcept();
            errno = error;
            throw_errno("inspect authoritative event ledger final path");
        }

        data_fd_ = ::openat(directory_fd_, partial_name_.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                             S_IRUSR | S_IWUSR);
        if (data_fd_ < 0)
        {
            const int error = errno;
            close_noexcept();
            errno = error;
            throw_errno("reserve event ledger partial");
        }

        struct stat status{};
        if (::fstat(data_fd_, &status) != 0)
        {
            const int error = errno;
            close_noexcept();
            errno = error;
            throw_errno("validate event ledger partial");
        }
        if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
        {
            close_noexcept();
            throw std::runtime_error(
                "authoritative event ledger partial is not a new regular file");
        }
        if (::fchmod(data_fd_, S_IRUSR | S_IWUSR) != 0)
        {
            const int error = errno;
            close_noexcept();
            errno = error;
            throw_errno("secure authoritative event ledger partial");
        }
    }

    void close_data_noexcept() noexcept
    {
        if (data_fd_ < 0) return;
        const int fd = data_fd_;
        data_fd_ = -1;
        (void)::close(fd);
    }
#endif

    void close_noexcept() noexcept
    {
#if defined(__linux__)
        if (data_fd_ >= 0)
        {
            (void)::close(data_fd_);
            data_fd_ = -1;
        }
        if (directory_fd_ >= 0)
        {
            (void)::close(directory_fd_);
            directory_fd_ = -1;
        }
#endif
    }

    void move_from(AuthoritativeEventLedgerReservation&& other) noexcept
    {
        final_path_ = std::move(other.final_path_);
        partial_path_ = std::move(other.partial_path_);
        final_name_ = std::move(other.final_name_);
        partial_name_ = std::move(other.partial_name_);
        directory_fd_ = std::exchange(other.directory_fd_, -1);
        data_fd_ = std::exchange(other.data_fd_, -1);
        published_ = std::exchange(other.published_, false);
    }
};

#if defined(__linux__)
// Minimal unbuffered ostream bridge for an already-reserved ledger FD.  The
// EventLogger needs tell/seek for its indexed binary format; this buffer keeps
// those operations descriptor-relative and never re-opens the path by name.
class AuthoritativeLedgerStreambuf final : public std::streambuf
{
public:
    explicit AuthoritativeLedgerStreambuf(int fd) noexcept : fd_(fd) {}

protected:
    std::streamsize xsputn(const char* source, std::streamsize size) override
    {
        if (fd_ < 0 || size < 0) return 0;
        std::streamsize written = 0;
        while (written < size)
        {
            const auto remaining = static_cast<std::size_t>(size - written);
            const auto result = ::write(fd_, source + written, remaining);
            if (result > 0)
            {
                written += static_cast<std::streamsize>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) continue;
            break;
        }
        return written;
    }

    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        const char one = traits_type::to_char_type(character);
        return xsputn(&one, 1) == 1 ? character : traits_type::eof();
    }

    int sync() override { return fd_ >= 0 ? 0 : -1; }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode which) override
    {
        if (fd_ < 0 || (which & std::ios_base::out) == 0)
            return pos_type(off_type(-1));
        int whence = SEEK_SET;
        switch (direction)
        {
        case std::ios_base::beg: whence = SEEK_SET; break;
        case std::ios_base::cur: whence = SEEK_CUR; break;
        case std::ios_base::end: whence = SEEK_END; break;
        default: return pos_type(off_type(-1));
        }
        const auto position = ::lseek(fd_, static_cast<off_t>(offset), whence);
        return position < 0 ? pos_type(off_type(-1))
                            : pos_type(static_cast<off_type>(position));
    }

    pos_type seekpos(pos_type position,
                     std::ios_base::openmode which) override
    {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg,
                       which);
    }

private:
    int fd_ = -1;
};
#endif


class EventLogger
{
public:
    explicit EventLogger(const std::string& path,
                         bool compress = true,
                         std::uint64_t max_bytes = 0,
                         int max_files = 5)
        : path_(path)
        , file_out_(path, std::ios::binary | std::ios::trunc)
        , out_(&file_out_)
        , compress_(compress)
        , cctx_(nullptr, &ZSTD_freeCCtx)
        , event_count_(0)
        , max_bytes_(max_bytes)
        , max_files_(max_files)
    {
        initialize();
    }

    explicit EventLogger(std::shared_ptr<AuthoritativeEventLedgerReservation> reservation,
                         bool compress = true)
        : path_(reservation ? reservation->final_path() : std::string{})
        , reservation_(std::move(reservation))
        , compress_(compress)
        , cctx_(nullptr, &ZSTD_freeCCtx)
        , event_count_(0)
    {
        if (!reservation_ || !reservation_->reserved())
            throw std::runtime_error(
                "EventLogger: missing authoritative ledger reservation");
#if defined(__linux__)
        ledger_streambuf_ = std::make_unique<AuthoritativeLedgerStreambuf>(
            reservation_->data_fd());
        ledger_out_ = std::make_unique<std::ostream>(ledger_streambuf_.get());
        out_ = ledger_out_.get();
        initialize();
#else
        throw std::runtime_error(
            "EventLogger: authoritative ledger requires supported POSIX path");
#endif
    }

    ~EventLogger() noexcept
    {
        // A descriptor-reserved live ledger must be explicitly finalized by
        // the shutdown protocol.  Publishing from stack unwinding would turn
        // a constructor failure or incomplete teardown into an apparently
        // authoritative final file.  Compatibility loggers keep their legacy
        // destructor-finalize behaviour.
        if (reservation_)
        {
            abandon_unpublished();
            return;
        }
        try {
            finalize();
        } catch (...) {
            // A destructor cannot surface a failed final trailer. Explicit
            // callers receive the stored failure and the logger stays closed.
        }
    }

    EventLogger(const EventLogger&) = delete;
    EventLogger& operator=(const EventLogger&) = delete;

    // Close a secure live ledger without publishing its final pathname.  This
    // operation is intentionally noexcept so constructor unwinding and
    // terminal safety handling cannot accidentally promote a partial file.
    void abandon_unpublished() noexcept
    {
        if (!reservation_ || state_ == logger_state::finalized
            || state_ == logger_state::abandoned)
            return;
        reservation_->abandon();
        state_ = logger_state::abandoned;
    }

    void log(const event& e)
    {
        ensure_open();

        std::vector<uint8_t> payload;

        switch (e.get_type()) {
        case event_type::market:
            payload = event_serial::serialise(require_event<market_event>(e, "market")); break;
        case event_type::signal:
            payload = event_serial::serialise(require_event<signal_event>(e, "signal")); break;
        case event_type::order:
            payload = event_serial::serialise(require_event<order_event>(e, "order")); break;
        case event_type::fill:
            payload = event_serial::serialise(require_event<fill_event>(e, "fill")); break;
        case event_type::tick:
            payload = event_serial::serialise(require_event<tick_event>(e, "tick")); break;
        case event_type::l2_snapshot:
            payload = event_serial::serialise(require_event<l2_snapshot_event>(e, "l2 snapshot")); break;
        case event_type::l2_update:
            payload = event_serial::serialise(require_event<l2_update_event>(e, "l2 update")); break;
        case event_type::cancel:
            payload = event_serial::serialise(require_event<cancel_event>(e, "cancel")); break;
        case event_type::amend:
            payload = event_serial::serialise(require_event<amend_event>(e, "amend")); break;
        case event_type::rejection:
            payload = event_serial::serialise(require_event<rejection_event>(e, "rejection")); break;
        case event_type::funding:
            payload = event_serial::serialise(require_event<funding_event>(e, "funding")); break;
        default:
            throw std::runtime_error("EventLogger: unknown event type");
        }

        const uint8_t* encoded_data = payload.data();
        std::size_t encoded_size = payload.size();
        if (compress_) {
            const size_t bound = ZSTD_compressBound(payload.size());
            compressed_buf_.resize(bound);
            const size_t compressed_size = ZSTD_compressCCtx(
                cctx_.get(), compressed_buf_.data(), bound,
                payload.data(), payload.size(), 1);
            if (ZSTD_isError(compressed_size))
                throw std::runtime_error(std::string("zstd compress: ") + ZSTD_getErrorName(compressed_size));
            encoded_data = compressed_buf_.data();
            encoded_size = compressed_size;
        }

        const auto encoded_size_u32 = event_serial::checked_u32_length(
            encoded_size, compress_ ? "compressed payload" : "payload");
        if (encoded_size >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max())) {
            throw std::length_error("event_log: payload is not stream-writable");
        }

        const bool sample_index =
            event_count_ % EVENT_LOG_INDEX_INTERVAL == 0 &&
            index_.size() < EVENT_LOG_MAX_INDEX_ENTRIES;
        EventLogIndexEntry index_entry{};
        if (sample_index) {
            if (index_.size() == index_.capacity()) {
                const auto next_capacity = std::min(
                    EVENT_LOG_MAX_INDEX_ENTRIES,
                    std::max<std::size_t>(1, index_.capacity() * 2));
                index_.reserve(next_capacity);
            }
            index_entry.timestamp_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    e.get_timestamp().time_since_epoch()).count();
        }

        try {
            const auto record_position = out_->tellp();
            if (record_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine record offset");
            const auto record_offset = static_cast<std::streamoff>(record_position);
            if (record_offset < 0)
                throw std::runtime_error("EventLogger: invalid record offset");
            if (sample_index)
                index_entry.file_offset = static_cast<uint64_t>(record_offset);

            event_serial::write_u8(*out_, static_cast<uint8_t>(e.get_type()));
            event_serial::write_u32(*out_, encoded_size_u32);
            if (encoded_size > 0) {
                out_->write(reinterpret_cast<const char*>(encoded_data),
                            static_cast<std::streamsize>(encoded_size));
            }
            if (!*out_)
                throw std::runtime_error("EventLogger: record write failed");

            if (sample_index)
                index_.push_back(index_entry);
            ++event_count_;

            const auto end_position = out_->tellp();
            if (end_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine log size");
            const auto log_size = static_cast<std::streamoff>(end_position);
            if (log_size < 0)
                throw std::runtime_error("EventLogger: invalid log size");
            if (max_bytes_ > 0 && static_cast<std::uint64_t>(log_size) >= max_bytes_)
                rotate();
        } catch (...) {
            poison(std::current_exception());
        }
    }

    void flush()
    {
        if (state_ == logger_state::finalized) return;
        if (state_ == logger_state::abandoned)
            throw std::logic_error("EventLogger: authoritative ledger was abandoned");
        rethrow_if_poisoned();
        try {
            out_->flush();
            if (!*out_)
                throw std::runtime_error("EventLogger: flush failed");
            if (reservation_)
                reservation_->sync_data();
        } catch (...) {
            poison(std::current_exception());
        }
    }

    void finalize()
    {
        if (state_ == logger_state::finalized) return;
        if (state_ == logger_state::abandoned)
            throw std::logic_error("EventLogger: authoritative ledger was abandoned");
        rethrow_if_poisoned();

        try {
            const auto index_position = out_->tellp();
            if (index_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine index offset");
            const auto signed_index_offset =
                static_cast<std::streamoff>(index_position);
            if (signed_index_offset < 0)
                throw std::runtime_error("EventLogger: invalid index offset");

            const auto index_count = event_serial::checked_u32_length(
                index_.size(), "index count");
            for (const auto& entry : index_) {
                event_serial::write_i64(*out_, entry.timestamp_us);
                event_serial::write_u64(*out_, entry.file_offset);
            }
            event_serial::write_u64(
                *out_, static_cast<uint64_t>(signed_index_offset));
            event_serial::write_u32(*out_, index_count);
            event_serial::write_u32(*out_, EVENT_LOG_INDEX_MAGIC);
            out_->flush();
            if (!*out_)
                throw std::runtime_error("EventLogger: finalize write failed");
            mark_file_finalized();
            if (reservation_)
                reservation_->finalize(
                    file_flags(/*finalized=*/true)
                    | EVENT_LOG_FILE_FLAG_AUTHORITATIVE_PUBLISHED);
            state_ = logger_state::finalized;
        } catch (...) {
            poison(std::current_exception());
        }
    }

private:
    enum class logger_state { open, finalized, abandoned, poisoned };

    std::string path_;
    std::shared_ptr<AuthoritativeEventLedgerReservation> reservation_;
    std::ofstream file_out_;
#if defined(__linux__)
    std::unique_ptr<AuthoritativeLedgerStreambuf> ledger_streambuf_;
#endif
    std::unique_ptr<std::ostream> ledger_out_;
    std::ostream* out_ = nullptr;
    bool compress_;
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx_;
    std::vector<uint8_t> compressed_buf_;
    std::vector<EventLogIndexEntry> index_;
    size_t event_count_;
    logger_state state_ = logger_state::open;
    std::exception_ptr failure_;
    std::uint64_t max_bytes_ = 0;
    int max_files_ = 5;

    template <typename Event>
    static const Event& require_event(const event& value, std::string_view name)
    {
        const auto* typed = dynamic_cast<const Event*>(&value);
        if (!typed) {
            throw std::runtime_error(
                "EventLogger: event tag/dynamic type mismatch for " +
                std::string(name));
        }
        return *typed;
    }

    void ensure_open() const
    {
        rethrow_if_poisoned();
        if (state_ == logger_state::finalized
            || state_ == logger_state::abandoned)
            throw std::logic_error("EventLogger: cannot log after finalize");
    }

    void rethrow_if_poisoned() const
    {
        if (state_ == logger_state::poisoned)
            std::rethrow_exception(failure_);
    }

    [[noreturn]] void poison(std::exception_ptr failure)
    {
        state_ = logger_state::poisoned;
        failure_ = failure;
        std::rethrow_exception(failure_);
    }

    void initialize()
    {
        if (!out_ || !*out_)
            throw std::runtime_error("EventLogger: cannot open " + path_);
        if (compress_) {
            cctx_.reset(ZSTD_createCCtx());
            if (!cctx_)
                throw std::runtime_error("EventLogger: failed to create zstd context");
        }
        write_file_preamble();
    }

    void write_file_preamble()
    {
        out_->write(reinterpret_cast<const char*>(EVENT_LOG_FILE_MAGIC.data()),
                    static_cast<std::streamsize>(EVENT_LOG_FILE_MAGIC.size()));
        event_serial::write_u8(*out_, EVENT_LOG_FILE_VERSION);
        event_serial::write_u8(*out_, file_flags(/*finalized=*/false));
        if (!*out_)
            throw std::runtime_error("EventLogger: preamble write failed");
    }

    [[nodiscard]] uint8_t file_flags(bool finalized) const noexcept
    {
        uint8_t flags = compress_ ? EVENT_LOG_FILE_FLAG_ZSTD : uint8_t{0};
        if (finalized)
            flags |= EVENT_LOG_FILE_FLAG_FINALIZED;
        if (reservation_)
            flags |= EVENT_LOG_FILE_FLAG_AUTHORITATIVE;
        return flags;
    }

    void mark_file_finalized()
    {
        constexpr std::streamoff flags_offset =
            static_cast<std::streamoff>(EVENT_LOG_FILE_MAGIC.size() + 1U);
        out_->seekp(flags_offset, std::ios::beg);
        if (!*out_)
            throw std::runtime_error("EventLogger: cannot update finalize marker");
        event_serial::write_u8(*out_, file_flags(/*finalized=*/true));
        out_->flush();
        if (!*out_)
            throw std::runtime_error("EventLogger: finalize marker write failed");
    }

    void rotate()
    {
        if (reservation_)
            throw std::runtime_error(
                "EventLogger: authoritative ledger cannot rotate");
        finalize();
        file_out_.close();

        auto nth = [&](int i) -> std::string {
            return path_ + "." + std::to_string(i);
        };
        if (max_files_ > 0) {
            std::string oldest = nth(max_files_);
            std::remove(oldest.c_str());
            for (int i = max_files_ - 1; i >= 1; --i)
                std::rename(nth(i).c_str(), nth(i + 1).c_str());
            std::rename(path_.c_str(), nth(1).c_str());
        } else {
            std::remove(path_.c_str());
        }

        file_out_.open(path_, std::ios::binary | std::ios::trunc);
        out_ = &file_out_;
        if (!*out_)
            throw std::runtime_error("EventLogger: reopen after rotate failed: " + path_);
        index_.clear();
        event_count_ = 0;
        write_file_preamble();
        state_ = logger_state::open;
    }
};


struct EventReplayLimits
{
    // Current events top out near 128 KiB (two uint16-sized strings plus
    // fixed fields).  One MiB leaves ample schema headroom while keeping a
    // corrupt record from driving a large allocation.
    std::size_t max_encoded_payload_bytes = 1U << 20;
    std::size_t max_decoded_payload_bytes = 1U << 20;
    std::uint32_t max_index_entries = 1'000'000;
};


class EventReplayer
{
public:
    explicit EventReplayer(const std::string& path,
                           int64_t replay_from_us = 0,
                           int64_t replay_to_us = INT64_MAX,
                           EventReplayLimits limits = {})
#if defined(__linux__)
        : in_()
#else
        : in_(path, std::ios::binary)
#endif
        , compressed_(false)
        , dctx_(nullptr, &ZSTD_freeDCtx)
        , replay_from_us_(replay_from_us)
        , replay_to_us_(replay_to_us)
        , limits_(limits)
    {
        // A secure live session writes only to a descriptor-reserved
        // `<final>.partial` until the data and parent directory have both
        // been synced and atomically published.  Treat a leftover partial as
        // forensic evidence, never as an authoritative replay input.
        if (std::string_view(path).ends_with(".partial"))
            throw std::runtime_error(
                "EventReplayer: partial ledger is not authoritative");
#if defined(__linux__)
        reopen_regular_source(path);
#endif
        if (!in_)
            throw std::runtime_error("EventReplayer: cannot open " + path);

        auto file_size = source_file_size();
        bool has_file_preamble = read_file_preamble(file_size);
#if defined(__linux__)
        // A final name accompanied by its authoritative reservation's
        // `.partial` sibling denotes a failed or interrupted publication.
        // Ordinary compatibility logs deliberately retain their historical
        // replay behavior even if an unrelated sibling happens to exist.
        if (has_file_preamble && file_authoritative_)
        {
            // The first read only identifies the format.  Re-open the live
            // ledger through its trusted parent directory before trusting
            // any of its contents, so replay cannot be redirected through a
            // symlink or a locally writable directory entry.
            reopen_authoritative_source(path);
            file_size = source_file_size();
            reset_file_preamble_state();
            has_file_preamble = read_file_preamble(file_size);
            if (!has_file_preamble || !file_authoritative_)
                throw std::runtime_error(
                    "EventReplayer: authoritative ledger changed during secure open");
            if (!file_authoritative_published_ || !file_finalized_)
                throw std::runtime_error(
                    "EventReplayer: authoritative ledger was not published");
            event_log_detail::require_no_authoritative_partial_sibling(path);
        }
#else
        if (has_file_preamble && file_authoritative_)
            throw std::runtime_error(
                "EventReplayer: authoritative ledgers require supported POSIX paths");
#endif
        data_end_ = file_size;

        constexpr std::streamoff trailer_bytes = 16;
        const bool requires_index_trailer =
            has_file_preamble && file_finalized_;
        if (requires_index_trailer && file_size < trailer_bytes) {
            throw std::runtime_error(
                "EventReplayer: finalized file is missing index trailer");
        }

        if (requires_index_trailer ||
            (!has_file_preamble && file_size >= trailer_bytes)) {
            seek_absolute(file_size - trailer_bytes);
            uint64_t index_offset = 0;
            uint32_t entry_count = 0;
            uint32_t magic = 0;
            read_exact(&index_offset, 8, "index offset");
            read_exact(&entry_count, 4, "index count");
            read_exact(&magic, 4, "index magic");

            if (magic == EVENT_LOG_INDEX_MAGIC) {
                const auto trailer_offset =
                    static_cast<uint64_t>(file_size - trailer_bytes);
                constexpr uint64_t index_entry_bytes = 16;
                const uint64_t index_bytes =
                    static_cast<uint64_t>(entry_count) * index_entry_bytes;
                if (index_bytes > trailer_offset ||
                    index_offset < static_cast<uint64_t>(data_begin_) ||
                    index_offset != trailer_offset - index_bytes) {
                    throw std::runtime_error("EventReplayer: invalid index trailer");
                }
                if (entry_count > limits_.max_index_entries)
                    throw std::runtime_error("EventReplayer: index entry limit exceeded");

                data_end_ = static_cast<std::streamoff>(index_offset);
                validate_index_and_seek(index_offset, entry_count);
            } else if (requires_index_trailer) {
                throw std::runtime_error(
                    "EventReplayer: finalized file is missing index trailer");
            } else {
                seek_absolute(data_begin_);
            }
        } else {
            seek_absolute(data_begin_);
        }

        if (!has_file_preamble)
            compressed_ = detect_legacy_compression();
        if (compressed_)
            create_decompression_context();
    }

    ~EventReplayer() = default;

    EventReplayer(const EventReplayer&) = delete;
    EventReplayer& operator=(const EventReplayer&) = delete;

    bool has_next() const
    {
        if (failed_ || !in_.good()) return false;
        const auto pos = in_.tellg();
        if (pos == std::streampos(-1)) return false;
        return static_cast<std::streamoff>(pos) < data_end_;
    }

    event_pointer next()
    {
        if (failed_)
            std::rethrow_exception(failure_);

        try {
            return next_impl();
        } catch (...) {
            // A malformed record is not a recoverable skip: continuing after
            // it could replay a suffix from an untrusted, corrupted log.
            failed_ = true;
            failure_ = std::current_exception();
            throw;
        }
    }

private:
    event_pointer next_impl()
    {
        while (true) {
            const auto record_offset = current_position();
            if (record_offset == data_end_) return nullptr;
            if (record_offset < 0 || record_offset > data_end_)
                throw std::runtime_error("EventReplayer: invalid record offset");
            if (data_end_ - record_offset < 5)
                throw std::runtime_error("EventReplayer: truncated record header");

            uint8_t type_byte = 0;
            uint32_t payload_size = 0;
            read_exact(&type_byte, 1, "record type");
            read_exact(&payload_size, 4, "payload size");

            if (type_byte > static_cast<uint8_t>(event_type::funding))
                throw std::runtime_error("EventReplayer: unknown event type");
            if (payload_size == 0)
                throw std::runtime_error("EventReplayer: empty payload");
            if (payload_size > limits_.max_encoded_payload_bytes)
                throw std::runtime_error("EventReplayer: encoded payload limit exceeded");
            if (!compressed_ && payload_size > limits_.max_decoded_payload_bytes)
                throw std::runtime_error("EventReplayer: decoded payload limit exceeded");
            if (payload_size >
                static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
                throw std::runtime_error("EventReplayer: payload is not stream-readable");

            const auto payload_offset = current_position();
            const auto remaining = data_end_ - payload_offset;
            if (remaining < 0 || static_cast<uint64_t>(remaining) < payload_size)
                throw std::runtime_error("EventReplayer: truncated payload");

            std::vector<uint8_t> raw(static_cast<std::size_t>(payload_size));
            if (!raw.empty())
                read_exact(raw.data(), static_cast<std::streamsize>(raw.size()),
                           "payload");

            event_pointer ev;
            if (compressed_) {
                const auto frame_size = ZSTD_findFrameCompressedSize(
                    raw.data(), raw.size());
                if (ZSTD_isError(frame_size) || frame_size != raw.size())
                    throw std::runtime_error(
                        "EventReplayer: record is not exactly one zstd frame");
                const unsigned long long decompressed_size =
                    ZSTD_getFrameContentSize(raw.data(), raw.size());
                if (decompressed_size == ZSTD_CONTENTSIZE_ERROR)
                    throw std::runtime_error("EventReplayer: invalid zstd frame");
                if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN)
                    throw std::runtime_error(
                        "EventReplayer: zstd frame content size is unknown");
                if (decompressed_size >
                    static_cast<unsigned long long>(
                        limits_.max_decoded_payload_bytes))
                    throw std::runtime_error(
                        "EventReplayer: decoded payload limit exceeded");
                if (decompressed_size >
                    static_cast<unsigned long long>(
                        std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "EventReplayer: decoded payload is not addressable");

                const auto decoded_size =
                    static_cast<std::size_t>(decompressed_size);
                if (decoded_size == 0)
                    throw std::runtime_error(
                        "EventReplayer: decoded payload is empty");
                std::vector<uint8_t> decompressed(decoded_size);
                size_t result = ZSTD_decompressDCtx(
                    dctx_.get(), decompressed.data(), decompressed.size(),
                    raw.data(), raw.size());
                if (ZSTD_isError(result))
                    throw std::runtime_error(std::string("zstd decompress: ") + ZSTD_getErrorName(result));
                if (result != decoded_size)
                    throw std::runtime_error(
                        "EventReplayer: zstd content size mismatch");
                ev = event_serial::deserialise(
                    static_cast<event_type>(type_byte), decompressed.data(), result);
            } else {
                ev = event_serial::deserialise(
                    static_cast<event_type>(type_byte), raw.data(), raw.size());
            }

            auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                ev->get_timestamp().time_since_epoch()).count();
            if (ts_us > replay_to_us_) return nullptr;
            if (ts_us < replay_from_us_) continue;
            return ev;
        }
    }

    mutable std::ifstream in_;
    bool compressed_;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx_;
    int64_t replay_from_us_;
    int64_t replay_to_us_;
    EventReplayLimits limits_;
    std::streamoff data_begin_ = 0;
    std::streamoff data_end_ = 0;
    bool file_finalized_ = false;
    bool file_authoritative_ = false;
    bool file_authoritative_published_ = false;
    bool failed_ = false;
    std::exception_ptr failure_;

    std::streamoff source_file_size()
    {
        in_.clear();
        in_.seekg(0, std::ios::end);
        const auto file_end = in_.tellg();
        if (file_end == std::streampos(-1))
            throw std::runtime_error(
                "EventReplayer: cannot determine file size");
        const auto file_size = static_cast<std::streamoff>(file_end);
        if (file_size < 0)
            throw std::runtime_error("EventReplayer: invalid file size");
        return file_size;
    }

    void reset_file_preamble_state() noexcept
    {
        data_begin_ = 0;
        data_end_ = 0;
        compressed_ = false;
        file_finalized_ = false;
        file_authoritative_ = false;
        file_authoritative_published_ = false;
    }

#if defined(__linux__)
    void reopen_regular_source(const std::string& path)
    {
        reopen_source_from_descriptor(
            event_log_detail::open_regular_event_log_for_replay(path),
            "regular event log");
    }

    void reopen_authoritative_source(const std::string& path)
    {
        reopen_source_from_descriptor(
            event_log_detail::open_private_authoritative_ledger_for_replay(path),
            "authoritative ledger");
    }

    void reopen_source_from_descriptor(int source_fd, std::string_view label)
    {
        char descriptor_path[64]{};
        const auto descriptor_path_size = std::snprintf(
            descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d",
            source_fd);
        if (descriptor_path_size < 0
            || static_cast<std::size_t>(descriptor_path_size)
                >= sizeof(descriptor_path))
        {
            (void)::close(source_fd);
            throw std::runtime_error(
                "EventReplayer: format " + std::string(label)
                    + " descriptor path");
        }

        in_.close();
        in_.clear();
        in_.open(descriptor_path, std::ios::binary);
        const int close_error = ::close(source_fd);
        if (!in_)
            throw std::runtime_error(
                "EventReplayer: cannot reopen " + std::string(label)
                    + " source");
        if (close_error != 0)
            throw std::runtime_error(
                "EventReplayer: cannot retire " + std::string(label)
                    + " descriptor");
    }
#endif

    std::streamoff current_position() const
    {
        const auto pos = in_.tellg();
        if (pos == std::streampos(-1))
            throw std::runtime_error("EventReplayer: cannot determine file position");
        return static_cast<std::streamoff>(pos);
    }

    void seek_absolute(std::streamoff offset)
    {
        if (offset < 0)
            throw std::runtime_error("EventReplayer: invalid seek offset");
        in_.clear();
        in_.seekg(offset, std::ios::beg);
        if (!in_)
            throw std::runtime_error("EventReplayer: seek failed");
    }

    void read_exact(void* destination,
                    std::streamsize bytes,
                    std::string_view field)
    {
        in_.read(static_cast<char*>(destination), bytes);
        if (in_.gcount() != bytes)
            throw std::runtime_error("EventReplayer: truncated " +
                                     std::string(field));
    }

    bool read_file_preamble(std::streamoff file_size)
    {
        data_begin_ = 0;
        if (file_size == 0)
            return false;

        seek_absolute(0);
        uint8_t first = 0;
        read_exact(&first, 1, "file preamble");
        if (first != EVENT_LOG_FILE_MAGIC.front()) {
            seek_absolute(0);
            return false;
        }
        if (file_size < EVENT_LOG_FILE_PREAMBLE_BYTES)
            throw std::runtime_error("EventReplayer: truncated file preamble");

        std::array<uint8_t, 4> magic{};
        magic.front() = first;
        read_exact(magic.data() + 1, 3, "file preamble magic");
        if (magic != EVENT_LOG_FILE_MAGIC)
            throw std::runtime_error("EventReplayer: invalid file preamble");

        uint8_t version = 0;
        uint8_t flags = 0;
        read_exact(&version, 1, "file preamble version");
        read_exact(&flags, 1, "file preamble flags");
        if (version != EVENT_LOG_FILE_VERSION)
            throw std::runtime_error("EventReplayer: unsupported file version");
        if ((flags & ~EVENT_LOG_FILE_KNOWN_FLAGS) != 0)
            throw std::runtime_error("EventReplayer: unsupported file flags");
        const bool finalized = (flags & EVENT_LOG_FILE_FLAG_FINALIZED) != 0;
        const bool authoritative =
            (flags & EVENT_LOG_FILE_FLAG_AUTHORITATIVE) != 0;
        const bool authoritative_published =
            (flags & EVENT_LOG_FILE_FLAG_AUTHORITATIVE_PUBLISHED) != 0;
        // Publication is the final durable transition of an authoritative
        // file.  Any other combination is torn/corrupt metadata, never a
        // compatibility log that may bypass private-path validation.
        if (authoritative_published && (!authoritative || !finalized))
            throw std::runtime_error(
                "EventReplayer: invalid authoritative publication flags");

        data_begin_ = EVENT_LOG_FILE_PREAMBLE_BYTES;
        compressed_ = (flags & EVENT_LOG_FILE_FLAG_ZSTD) != 0;
        file_finalized_ = finalized;
        file_authoritative_ = authoritative;
        file_authoritative_published_ = authoritative_published;
        return true;
    }

    bool detect_legacy_compression()
    {
        const auto start_pos = current_position();
        bool is_compressed = false;
        if (data_end_ - start_pos >= 9) {
            uint8_t type_byte = 0;
            uint32_t payload_size = 0;
            read_exact(&type_byte, 1, "record type");
            read_exact(&payload_size, 4, "payload size");
            const auto remaining = data_end_ - current_position();
            if (type_byte <= static_cast<uint8_t>(event_type::funding) &&
                payload_size >= 4 &&
                payload_size <= limits_.max_encoded_payload_bytes &&
                remaining >= 0 &&
                static_cast<uint64_t>(remaining) >= payload_size) {
                std::vector<uint8_t> payload(static_cast<std::size_t>(payload_size));
                read_exact(payload.data(), static_cast<std::streamsize>(payload.size()),
                           "legacy payload");
                constexpr std::array<uint8_t, 4> zstd_magic{
                    0x28, 0xB5, 0x2F, 0xFD};
                if (std::equal(zstd_magic.begin(), zstd_magic.end(), payload.begin())) {
                    const auto frame_size = ZSTD_findFrameCompressedSize(
                        payload.data(), payload.size());
                    const auto decoded_size = ZSTD_getFrameContentSize(
                        payload.data(), payload.size());
                    is_compressed =
                        !ZSTD_isError(frame_size) && frame_size == payload.size() &&
                        decoded_size != ZSTD_CONTENTSIZE_ERROR &&
                        decoded_size != ZSTD_CONTENTSIZE_UNKNOWN &&
                        decoded_size > 0 &&
                        decoded_size <= limits_.max_decoded_payload_bytes;
                }
            }
        }
        seek_absolute(start_pos);
        return is_compressed;
    }

    void create_decompression_context()
    {
        dctx_.reset(ZSTD_createDCtx());
        if (!dctx_)
            throw std::runtime_error(
                "EventReplayer: failed to create zstd context");
    }

    void validate_index_and_seek(uint64_t index_offset, uint32_t entry_count)
    {
        const auto index_end = static_cast<std::streamoff>(index_offset);
        bool timestamps_sorted = true;
        bool have_best_offset = false;
        int64_t previous_timestamp = 0;
        uint64_t previous_offset = 0;
        uint64_t best_offset = 0;
        EventLogIndexEntry pending{};
        uint32_t next_index = 0;
        bool have_pending = false;

        const auto read_next_index = [&] {
            if (next_index == entry_count) {
                have_pending = false;
                return;
            }

            const uint64_t entry_offset =
                index_offset + static_cast<uint64_t>(next_index) * 16U;
            seek_absolute(static_cast<std::streamoff>(entry_offset));
            read_exact(&pending.timestamp_us, 8, "index timestamp");
            read_exact(&pending.file_offset, 8, "index file offset");

            if (pending.file_offset < static_cast<uint64_t>(data_begin_) ||
                pending.file_offset >= index_offset ||
                (next_index > 0 && pending.file_offset <= previous_offset)) {
                throw std::runtime_error("EventReplayer: invalid index offset");
            }
            if (next_index > 0 && pending.timestamp_us < previous_timestamp)
                timestamps_sorted = false;
            if (replay_from_us_ > 0 &&
                pending.timestamp_us <= replay_from_us_) {
                best_offset = pending.file_offset;
                have_best_offset = true;
            }

            previous_timestamp = pending.timestamp_us;
            previous_offset = pending.file_offset;
            ++next_index;
            have_pending = true;
        };

        read_next_index();
        seek_absolute(data_begin_);
        std::streamoff record_offset = data_begin_;

        // Validate the record area and every index target in one streaming
        // pass. No record payload or index vector is materialised here.
        while (record_offset < index_end) {
            if (have_pending &&
                pending.file_offset < static_cast<uint64_t>(record_offset)) {
                throw std::runtime_error("EventReplayer: index is not on a record boundary");
            }
            if (have_pending &&
                pending.file_offset == static_cast<uint64_t>(record_offset)) {
                read_next_index();
                seek_absolute(record_offset);
            }

            if (index_end - record_offset < 5) {
                throw std::runtime_error(
                    "EventReplayer: index does not start on a record boundary");
            }

            uint8_t type_byte = 0;
            uint32_t payload_size = 0;
            read_exact(&type_byte, 1, "record type");
            read_exact(&payload_size, 4, "payload size");
            if (type_byte > static_cast<uint8_t>(event_type::funding) ||
                payload_size == 0 ||
                payload_size > limits_.max_encoded_payload_bytes) {
                throw std::runtime_error(
                    "EventReplayer: invalid record before index");
            }

            const auto payload_offset = current_position();
            const auto remaining = index_end - payload_offset;
            if (remaining < 0 ||
                static_cast<uint64_t>(remaining) < payload_size) {
                throw std::runtime_error(
                    "EventReplayer: index cuts through a record");
            }
            record_offset = payload_offset +
                            static_cast<std::streamoff>(payload_size);
            seek_absolute(record_offset);
        }

        if (record_offset != index_end || have_pending) {
            throw std::runtime_error("EventReplayer: invalid index record boundary");
        }

        if (timestamps_sorted && have_best_offset)
            seek_absolute(static_cast<std::streamoff>(best_offset));
        else
            seek_absolute(data_begin_);
    }
};
