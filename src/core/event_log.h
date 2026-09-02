#pragma once

#include "event.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

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
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_f64(e.get_open());
    append_f64(e.get_high());
    append_f64(e.get_low());
    append_f64(e.get_close());
    append_i64(e.get_volume());
    append_u64(e.get_quantity_scale());
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
    append_u64(e.get_opener_order_id());
    append_str(e.get_strategy_name());
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
    append_u64(e.get_opener_order_id());
    append_str(e.get_strategy_name());
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
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    append_f64(e.get_price());
    append_i64(e.get_quantity());
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    append_u64(e.get_quantity_scale());
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
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

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
    append_u64(e.get_quantity_scale());
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
    auto append_u64 = [&](uint64_t v) { append(&v, 8); };

    append_ts(e.get_timestamp());
    append_str(e.get_symbol());
    uint8_t sd = static_cast<uint8_t>(e.get_side());
    append(&sd, 1);
    append_f64(e.get_price());
    append_i64(e.get_new_quantity());
    append_u64(e.get_quantity_scale());
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
        std::uint64_t quantity_scale = 1;
        if (r.remaining() == sizeof(std::uint64_t))
            quantity_scale = r.read_u64();
        else if (r.remaining() != 0)
            throw std::runtime_error("event_log: invalid market extension length");
        return finish(std::make_shared<market_event>(
            ts, symbol, open, high, low, close, volume, quantity_scale));
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
        uint64_t opener_order_id = 0;
        std::string strategy_name;
        if (r.remaining() != 0) {
            if (r.remaining() < sizeof(std::uint64_t) + sizeof(std::uint16_t))
                throw std::runtime_error("event_log: invalid order attribution extension");
            opener_order_id = r.read_u64();
            strategy_name = r.read_str();
        }
        auto ev = std::make_shared<order_event>(ts, symbol, ot, sd, qty, price, tif, stop_price);
        ev->set_order_id(oid);
        ev->set_earliest_eligible_ts(elig_ts);
        ev->set_opener_order_id(opener_order_id);
        ev->set_strategy_name(strategy_name);
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
        uint64_t opener_order_id = 0;
        std::string strategy_name;
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
            // V2 attribution extends the complete 17-byte fill shape with
            // opener id plus a bounded length-prefixed strategy name.
            if (r.remaining() < 17U + sizeof(std::uint64_t)
                                  + sizeof(std::uint16_t))
                throw std::runtime_error("event_log: invalid fill extension length");
            remaining = r.read_f64();
            fill_id = r.read_u64();
            src = read_enum(r, fill_source::exchange, "fill source");
            opener_order_id = r.read_u64();
            strategy_name = r.read_str();
            break;
        }
        auto ev = std::make_shared<fill_event>(ts, symbol, oid, sd, qty, price,
                                               commission, remaining, fill_id,
                                               strategy_name, opener_order_id);
        ev->set_source(src);
        return finish(std::move(ev));
    }
    case event_type::tick: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        double price = r.read_f64();
        int64_t qty = r.read_i64();
        auto sd = read_enum(r, tick_side::unknown, "tick side");
        std::uint64_t quantity_scale = 1;
        if (r.remaining() == sizeof(std::uint64_t))
            quantity_scale = r.read_u64();
        else if (r.remaining() != 0)
            throw std::runtime_error("event_log: invalid tick extension length");
        return finish(std::make_shared<tick_event>(
            ts, symbol, price, qty, sd, quantity_scale));
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
        std::uint64_t quantity_scale = 1;
        if (r.remaining() == sizeof(std::uint64_t))
            quantity_scale = r.read_u64();
        else if (r.remaining() != 0)
            throw std::runtime_error("event_log: invalid L2 snapshot extension length");
        return finish(std::make_shared<l2_snapshot_event>(
            ts, symbol, bids.data(), bid_n, asks.data(), ask_n,
            quantity_scale));
    }
    case event_type::l2_update: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        auto sd = read_enum(r, tick_side::unknown, "tick side");
        double price = r.read_f64();
        int64_t new_qty = r.read_i64();
        std::uint64_t quantity_scale = 1;
        if (r.remaining() == sizeof(std::uint64_t))
            quantity_scale = r.read_u64();
        else if (r.remaining() != 0)
            throw std::runtime_error("event_log: invalid L2 update extension length");
        return finish(std::make_shared<l2_update_event>(
            ts, symbol, sd, price, new_qty, quantity_scale));
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
static constexpr uint8_t EVENT_LOG_LEGACY_FILE_VERSION = 1;
static constexpr uint8_t EVENT_LOG_PREVIOUS_FILE_VERSION = 2;
static constexpr uint8_t EVENT_LOG_FILE_VERSION = 3;
static constexpr uint8_t EVENT_LOG_FILE_FLAG_ZSTD = 1U << 0;
static constexpr uint8_t EVENT_LOG_FILE_FLAG_FINALIZED = 1U << 1;
// Rotated files are independently finalized segments, not self-contained
// ledgers. They remain readable for inspection but require manifest stitching
// before they can be authoritative accounting input.
static constexpr uint8_t EVENT_LOG_FILE_FLAG_SEGMENTED = 1U << 2;
static constexpr uint8_t EVENT_LOG_FILE_KNOWN_FLAGS =
    EVENT_LOG_FILE_FLAG_ZSTD | EVENT_LOG_FILE_FLAG_FINALIZED
    | EVENT_LOG_FILE_FLAG_SEGMENTED;
static constexpr std::streamoff EVENT_LOG_FILE_PREAMBLE_BYTES = 6;
static constexpr uint32_t EVENT_LOG_SEAL_MAGIC = 0x534C5454;
static constexpr uint32_t EVENT_LOG_SEAL_VERSION = 1;
static constexpr std::streamoff EVENT_LOG_INDEX_TRAILER_BYTES = 16;
static constexpr std::streamoff EVENT_LOG_SEAL_BYTES = 40;

constexpr uint32_t event_log_crc32c_table_entry(uint32_t index) noexcept
{
    uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit)
        value = (value >> 1U) ^
            ((value & 1U) != 0 ? 0x82F63B78U : 0U);
    return value;
}

constexpr std::array<uint32_t, 256> make_event_log_crc32c_table() noexcept
{
    std::array<uint32_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = event_log_crc32c_table_entry(
            static_cast<uint32_t>(i));
    return table;
}

inline constexpr auto EVENT_LOG_CRC32C_TABLE =
    make_event_log_crc32c_table();

class EventLogCrc32c final
{
public:
    void reset() noexcept { state_ = 0xFFFFFFFFU; }

    void update(const void* bytes, std::size_t size) noexcept
    {
        const auto* cursor = static_cast<const uint8_t*>(bytes);
        for (std::size_t i = 0; i < size; ++i)
            state_ = EVENT_LOG_CRC32C_TABLE[
                (state_ ^ cursor[i]) & 0xFFU] ^ (state_ >> 8U);
    }

    [[nodiscard]] uint32_t value() const noexcept
    {
        return state_ ^ 0xFFFFFFFFU;
    }

private:
    uint32_t state_ = 0xFFFFFFFFU;
};

class EventLogWriterLock final
{
public:
    EventLogWriterLock() noexcept = default;
    ~EventLogWriterLock() noexcept { release(); }

    EventLogWriterLock(const EventLogWriterLock&) = delete;
    EventLogWriterLock& operator=(const EventLogWriterLock&) = delete;

    void acquire(const std::string& path, bool create)
    {
        release();
        fd_ = open_and_lock(path, create);
    }

    // Rotation must keep the sealed segment exclusively owned until the new
    // base pathname is also locked.  Acquire the successor first so a failure
    // leaves the old lock intact and the logger can fail closed without ever
    // creating a two-writer interval.
    void handoff_to(const std::string& path, bool create)
    {
        const int successor = open_and_lock(path, create);
        release();
        fd_ = successor;
    }

    [[nodiscard]] std::string open_path() const
    {
        if (fd_ < 0)
            throw std::logic_error(
                "EventLogger: writer lock has no open file");
        return "/proc/self/fd/" + std::to_string(fd_);
    }

    void release() noexcept
    {
        if (fd_ < 0)
            return;
        (void)::flock(fd_, LOCK_UN);
        (void)::close(fd_);
        fd_ = -1;
    }

private:
    static int open_and_lock(const std::string& path, bool create)
    {
        int flags = O_RDWR | O_CLOEXEC | O_NONBLOCK;
        if (create)
            flags |= O_CREAT;
        const int candidate = ::open(path.c_str(), flags, 0666);
        if (candidate < 0)
            throw std::runtime_error(
                "EventLogger: cannot open writer lock for " + path + ": " +
                std::strerror(errno));
        if (::flock(candidate, LOCK_EX | LOCK_NB) != 0)
        {
            const int saved_errno = errno;
            ::close(candidate);
            throw std::runtime_error(
                "EventLogger: another writer owns " + path + ": " +
                std::strerror(saved_errno));
        }
        struct stat locked {};
        struct stat visible {};
        if (::fstat(candidate, &locked) != 0 ||
            ::stat(path.c_str(), &visible) != 0 ||
            locked.st_dev != visible.st_dev ||
            locked.st_ino != visible.st_ino)
        {
            (void)::flock(candidate, LOCK_UN);
            (void)::close(candidate);
            throw std::runtime_error(
                "EventLogger: writer target changed while acquiring lock: " +
                path);
        }
        return candidate;
    }

    int fd_ = -1;
};

inline bool durable_log_directory_is_trusted(const struct stat& st) noexcept
{
    if (!S_ISDIR(st.st_mode))
        return false;
    static const uid_t namespace_root_owner = []() noexcept {
        struct stat root {};
        return ::lstat("/", &root) == 0 ? root.st_uid : uid_t{0};
    }();
    const uid_t owner = st.st_uid;
    if (owner != 0 && owner != namespace_root_owner &&
        owner != ::geteuid())
        return false;
    const bool writable_by_others =
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    // Sticky directories (for example /tmp) prevent an unrelated uid from
    // unlinking an entry it does not own. A writable non-sticky ancestor can
    // replace the ledger pathname after its record fsync but before submit.
    return !writable_by_others || (st.st_mode & S_ISVTX) != 0;
}


// Rejects /dev/null, FIFOs, device nodes, and symlinks resolving to any of
// those as a durable event-log target. std::ofstream::open() succeeds
// silently against all of these while every write is discarded, which would
// defeat the "--log-events is the mandatory durable truth" guarantee
// (docs/governance/01-prod.md, docs/operations/01-futures-phase0-operator-sop.md).
//
// Deliberately NOT enforced inside EventLogger's own constructor: this repo's
// existing fault-injection tests (EventLog.*DurableFinalizeFailureHaltsEngine)
// construct EventLogger directly against /dev/full — a character device that
// *fails writes loudly* (ENOSPC), the opposite problem from /dev/null, and a
// legitimate way to exercise the halt-on-durable-write-failure path. Blocking
// all non-regular files at the EventLogger layer would break that. This
// predicate is for operator-facing path validation instead — see its call
// site in the mainnet CLI gate (src/bin/main.inc).
// See docs/todos/08-H-persistence-observability.md H-07.
inline bool is_acceptable_durable_log_target(const std::string& path)
{
    if (path.empty())
        return false;

    const std::filesystem::path target(path);
    const auto filename = target.filename();
    if (filename.empty() || filename == "." || filename == "..")
        return false;

    std::error_code ec;
    const auto target_status = std::filesystem::symlink_status(target, ec);
    if (ec)
    {
        if (ec != std::errc::no_such_file_or_directory)
            return false;
        ec.clear();
    }

    // Mainnet ledgers are immutable session artifacts. Reusing even an empty
    // path would make startup semantics depend on who created it and could
    // truncate an earlier authoritative ledger. Actual acquisition below is
    // the atomic O_EXCL authority; this predicate only keeps --dry-run aligned.
    if (std::filesystem::exists(target_status))
        return false;

    const auto parent = target.parent_path().empty()
        ? std::filesystem::path{"."}
        : target.parent_path();
    const auto absolute_parent =
        std::filesystem::absolute(parent, ec).lexically_normal();
    if (ec)
        return false;
    auto current = std::filesystem::path{"/"};
    struct stat directory_stat {};
    if (::lstat(current.c_str(), &directory_stat) != 0 ||
        !durable_log_directory_is_trusted(directory_stat))
        return false;
    const auto components = absolute_parent.relative_path();
    for (const auto& component : components)
    {
        if (component.empty() || component == ".")
            continue;
        current /= component;
        const auto component_status =
            std::filesystem::symlink_status(current, ec);
        if (ec || std::filesystem::is_symlink(component_status) ||
            !std::filesystem::is_directory(component_status) ||
            ::lstat(current.c_str(), &directory_stat) != 0 ||
            !durable_log_directory_is_trusted(directory_stat))
            return false;
    }
    return ::access(absolute_parent.c_str(), W_OK | X_OK) == 0;
}

// Holds an already-open, kernel-verified regular file across provider startup.
// EventLogger subsequently opens /proc/self/fd/<fd>, so pathname replacement
// between CLI validation and logger construction cannot redirect the durable
// ledger to a FIFO/device/different inode. The reservation is intentionally
// Linux-specific, matching the supported production target.
class DurableEventLogReservation final
{
public:
    static std::shared_ptr<DurableEventLogReservation> acquire(
        const std::string& path)
    {
        if (path.empty())
            throw std::runtime_error(
                "durable event log: target path is empty");

        const std::filesystem::path target(path);
        std::error_code ec;
        const auto absolute_target =
            std::filesystem::absolute(target, ec).lexically_normal();
        if (ec)
            throw std::runtime_error(
                "durable event log: cannot resolve target path: " +
                ec.message());
        const auto filename_path = absolute_target.filename();
        if (filename_path.empty() || filename_path == "." ||
            filename_path == "..")
            throw std::runtime_error(
                "durable event log: target must name a new file");

        const auto parent_path = absolute_target.parent_path();
        unique_fd parent_fd(open_directory_without_symlinks(parent_path));

        const std::string filename = filename_path.string();
        const int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                          O_NOFOLLOW | O_NONBLOCK;
        unique_fd file_fd(::openat(
            parent_fd.get(), filename.c_str(), flags, 0640));
        if (file_fd.get() < 0)
            throw_system_error(
                errno == EEXIST
                    ? "target already exists; choose a unique session path"
                    : "cannot create target");

        struct stat st {};
        if (::fstat(file_fd.get(), &st) != 0)
            throw_system_error("cannot inspect created target");
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            st.st_uid != ::geteuid())
            throw std::runtime_error(
                "durable event log: created target failed regular-file "
                "ownership/link-count checks");

        auto owned = std::unique_ptr<DurableEventLogReservation>(
            new DurableEventLogReservation(
                path, absolute_target.string(), filename,
                file_fd.get(), parent_fd.get(), st.st_dev, st.st_ino,
                st.st_uid));
        file_fd.release();
        parent_fd.release();
        return std::shared_ptr<DurableEventLogReservation>(std::move(owned));
    }

    ~DurableEventLogReservation() noexcept
    {
        if (fd_ >= 0) ::close(fd_);
        if (parent_fd_ >= 0) ::close(parent_fd_);
    }

    DurableEventLogReservation(const DurableEventLogReservation&) = delete;
    DurableEventLogReservation& operator=(
        const DurableEventLogReservation&) = delete;

    const std::string& logical_path() const noexcept { return logical_path_; }
    const std::string& open_path() const noexcept { return open_path_; }

    bool sync() const noexcept
    {
        // The file fsync persists contents/metadata; the directory fsync is
        // what makes the freshly-created pathname itself crash-durable.
        return sync_fd(fd_) && sync_fd(parent_fd_);
    }

    bool still_refers_to_reserved_file() const noexcept
    {
        struct stat st {};
        return fd_ >= 0 && ::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode) &&
               st.st_dev == device_ && st.st_ino == inode_ &&
               st.st_uid == owner_ && st.st_nlink == 1;
    }

    bool logical_path_still_refers_to_reserved_file() const noexcept
    {
        struct stat pinned_parent_entry {};
        struct stat visible_entry {};
        return parent_fd_ >= 0 &&
               ::fstatat(parent_fd_, filename_.c_str(),
                         &pinned_parent_entry, AT_SYMLINK_NOFOLLOW) == 0 &&
               ::lstat(identity_path_.c_str(), &visible_entry) == 0 &&
               same_reserved_file(pinned_parent_entry) &&
               same_reserved_file(visible_entry);
    }

    bool ready_for_venue_mutation() const noexcept
    {
        return still_refers_to_reserved_file() &&
               logical_path_still_refers_to_reserved_file();
    }

    // A retained inode is a one-session capability, not a reusable file
    // handle.  Claiming before EventLogger opens/truncates it prevents a
    // second logger (or a constructor that will later reject its arguments)
    // from erasing the first writer's ledger.
    void claim_for_logger()
    {
        bool expected = false;
        if (!logger_claimed_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            throw std::runtime_error(
                "durable event log: reservation already claimed");
    }

private:
    class unique_fd final
    {
    public:
        explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}
        ~unique_fd() noexcept { if (fd_ >= 0) ::close(fd_); }
        unique_fd(const unique_fd&) = delete;
        unique_fd& operator=(const unique_fd&) = delete;
        unique_fd(unique_fd&& other) noexcept : fd_(other.fd_)
        {
            other.fd_ = -1;
        }
        unique_fd& operator=(unique_fd&& other) noexcept
        {
            if (this == &other) return *this;
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
            return *this;
        }
        [[nodiscard]] int get() const noexcept { return fd_; }
        void release() noexcept { fd_ = -1; }

    private:
        int fd_ = -1;
    };

    [[noreturn]] static void throw_system_error(std::string_view operation)
    {
        const int saved_errno = errno;
        throw std::runtime_error(
            "durable event log: " + std::string(operation) + ": " +
            std::strerror(saved_errno));
    }

    static int open_directory_without_symlinks(
        const std::filesystem::path& input)
    {
        const auto normalized = input.lexically_normal();
        unique_fd current(::open(
            normalized.is_absolute() ? "/" : ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (current.get() < 0)
            throw_system_error("cannot open path root");

        const auto validate = [](int fd) {
            struct stat st {};
            if (::fstat(fd, &st) != 0)
                throw_system_error("cannot inspect parent directory");
            if (!durable_log_directory_is_trusted(st))
                throw std::runtime_error(
                    "durable event log: parent directory ancestry is "
                    "untrusted or writable without sticky protection");
        };
        validate(current.get());

        const auto relative = normalized.is_absolute()
            ? normalized.relative_path()
            : normalized;
        for (const auto& component : relative)
        {
            const auto name = component.string();
            if (name.empty() || name == ".")
                continue;
            unique_fd next(::openat(
                current.get(), name.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (next.get() < 0)
                throw_system_error(
                    "cannot open parent directory component without symlinks");
            validate(next.get());
            current = std::move(next);
        }
        const int result = current.get();
        current.release();
        return result;
    }

    static bool sync_fd(int fd) noexcept
    {
        if (fd < 0) return false;
        int result = 0;
        do
        {
            result = ::fsync(fd);
        }
        while (result != 0 && errno == EINTR);
        return result == 0;
    }

    bool same_reserved_file(const struct stat& st) const noexcept
    {
        return S_ISREG(st.st_mode) && st.st_dev == device_ &&
               st.st_ino == inode_ && st.st_uid == owner_ &&
               st.st_nlink == 1;
    }

    DurableEventLogReservation(std::string path,
                               std::string identity_path,
                               std::string filename,
                               int fd, int parent_fd,
                               dev_t device, ino_t inode, uid_t owner)
        : logical_path_(std::move(path))
        , identity_path_(std::move(identity_path))
        , filename_(std::move(filename))
        , open_path_("/proc/self/fd/" + std::to_string(fd))
        , fd_(fd)
        , parent_fd_(parent_fd)
        , device_(device)
        , inode_(inode)
        , owner_(owner)
    {
    }

    std::string logical_path_;
    std::string identity_path_;
    std::string filename_;
    std::string open_path_;
    int fd_ = -1;
    int parent_fd_ = -1;
    dev_t device_{};
    ino_t inode_{};
    uid_t owner_{};
    std::atomic<bool> logger_claimed_{false};
};

class EventLogger
{
public:
    explicit EventLogger(const std::string& path,
                         bool compress = true,
                         std::uint64_t max_bytes = 0,
                         int max_files = 5,
                         std::shared_ptr<DurableEventLogReservation>
                             reservation = {})
        : path_(path)
        , reservation_(std::move(reservation))
        , out_()
        , compress_(compress)
        , cctx_(nullptr, &ZSTD_freeCCtx)
        , event_count_(0)
        , max_bytes_(max_bytes)
        , max_files_(max_files)
    {
        if (reservation_)
        {
            // Claim first and never release the claim.  Any later constructor
            // failure abandons this unique session path rather than making it
            // reusable after a partially initialized writer.
            reservation_->claim_for_logger();
            if (reservation_->logical_path() != path_ ||
                !reservation_->still_refers_to_reserved_file() ||
                !reservation_->logical_path_still_refers_to_reserved_file())
                throw std::runtime_error(
                    "EventLogger: durable target reservation mismatch");
            if (max_bytes_ > 0)
                throw std::runtime_error(
                    "EventLogger: rotation is unsupported for a reserved "
                    "durable target");
        }
        writer_lock_.acquire(
            reservation_ ? reservation_->open_path() : path,
            /*create=*/!reservation_);
        out_.open(writer_lock_.open_path(),
                  std::ios::binary | std::ios::trunc);
        if (!out_)
            throw std::runtime_error("EventLogger: cannot open " + path);
        if (compress_) {
            cctx_.reset(ZSTD_createCCtx());
            if (!cctx_)
                throw std::runtime_error("EventLogger: failed to create zstd context");
        }
        write_file_preamble();
    }

    ~EventLogger() noexcept
    {
        // A reserved mainnet ledger may only be sealed by the engine after it
        // has quiesced every producer, drained the logging ring, and proved
        // that no record or durability acknowledgement was lost.  Destructor
        // finalization has no access to that protocol state and must therefore
        // leave an open reserved file as a diagnostic-only prefix.
        if (reservation_ || state_ != logger_state::open)
            return;
        try {
            finalize();
        } catch (...) {
            // A destructor cannot surface a failed final trailer. Explicit
            // callers receive the stored failure and the logger stays closed.
        }
    }

    EventLogger(const EventLogger&) = delete;
    EventLogger& operator=(const EventLogger&) = delete;

    [[nodiscard]] const std::string& logical_path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] bool has_durable_reservation() const noexcept
    {
        return static_cast<bool>(reservation_);
    }

    [[nodiscard]] bool uses_durable_reservation(
        const std::shared_ptr<DurableEventLogReservation>& expected) const
        noexcept
    {
        return reservation_ == expected;
    }

    // Mainnet startup calls this before provider open. Flushing the preamble
    // proves that the exact retained logger stream is writable; syncing the
    // independently retained descriptor proves those bytes reached the same
    // kernel-verified inode. The logger remains open and is transferred into
    // the engine rather than reopening a mutable pathname later.
    void verify_durable_ready()
    {
        if (!reservation_)
            throw std::logic_error(
                "EventLogger: durable readiness requires a reservation");
        try
        {
            ensure_open();
            out_.flush();
            if (!out_)
                throw std::runtime_error(
                    "EventLogger: durable preamble flush failed");
            if (!reservation_->still_refers_to_reserved_file() ||
                !reservation_->logical_path_still_refers_to_reserved_file() ||
                !reservation_->sync() ||
                !reservation_->logical_path_still_refers_to_reserved_file())
                throw std::runtime_error(
                    "EventLogger: durable preamble sync failed");
            durable_records_since_sync_ = 0;
            last_durable_sync_ = std::chrono::steady_clock::now();
        }
        catch (...)
        {
            poison(std::current_exception());
        }
    }

    void log(const event& e)
    {
        ensure_open();

        // Any record-construction failure is terminal for this ledger, even
        // when it happens before the first byte is written.  Otherwise a
        // worker could skip one economic event, continue/finalize later, and
        // incorrectly mark the incomplete file authoritative.
        try
        {
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

            const auto record_position = out_.tellp();
            if (record_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine record offset");
            const auto record_offset = static_cast<std::streamoff>(record_position);
            if (record_offset < 0)
                throw std::runtime_error("EventLogger: invalid record offset");
            if (sample_index)
                index_entry.file_offset = static_cast<uint64_t>(record_offset);

            const auto type_byte = static_cast<uint8_t>(e.get_type());
            EventLogCrc32c record_crc;
            record_crc.update(&type_byte, sizeof(type_byte));
            record_crc.update(&encoded_size_u32, sizeof(encoded_size_u32));
            record_crc.update(encoded_data, encoded_size);
            const uint32_t record_checksum = record_crc.value();

            write_tracked(&type_byte, sizeof(type_byte));
            write_tracked(&encoded_size_u32, sizeof(encoded_size_u32));
            write_tracked(encoded_data, encoded_size);
            write_tracked(&record_checksum, sizeof(record_checksum));
            if (!out_)
                throw std::runtime_error("EventLogger: record write failed");

            if (sample_index)
                index_.push_back(index_entry);
            ++event_count_;

            const auto end_position = out_.tellp();
            if (end_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine log size");
            const auto log_size = static_cast<std::streamoff>(end_position);
            if (log_size < 0)
                throw std::runtime_error("EventLogger: invalid log size");
            if (max_bytes_ > 0 && static_cast<std::uint64_t>(log_size) >= max_bytes_)
                rotate();

            if (reservation_)
            {
                ++durable_records_since_sync_;
                const auto now = std::chrono::steady_clock::now();
                if (requires_immediate_durable_sync(e.get_type()) ||
                    durable_records_since_sync_ >=
                        DURABLE_SYNC_RECORD_INTERVAL ||
                    now - last_durable_sync_ >= DURABLE_SYNC_TIME_INTERVAL)
                    durable_checkpoint();
            }
        }
        catch (...)
        {
            poison(std::current_exception());
        }
    }

    void flush()
    {
        if (state_ == logger_state::finalized) return;
        rethrow_if_poisoned();
        try {
            if (reservation_)
            {
                durable_checkpoint();
                return;
            }
            out_.flush();
            if (!out_)
                throw std::runtime_error("EventLogger: flush failed");
        } catch (...) {
            poison(std::current_exception());
        }
    }

    void finalize()
    {
        finalize_impl(/*release_writer_lock=*/true);
    }

private:
    void finalize_impl(bool release_writer_lock)
    {
        if (state_ == logger_state::finalized) return;
        rethrow_if_poisoned();
        if (state_ == logger_state::abandoned)
            throw std::logic_error(
                "EventLogger: cannot finalize an abandoned ledger");

        try {
            const auto index_position = out_.tellp();
            if (index_position == std::streampos(-1))
                throw std::runtime_error("EventLogger: cannot determine index offset");
            const auto signed_index_offset =
                static_cast<std::streamoff>(index_position);
            if (signed_index_offset < 0)
                throw std::runtime_error("EventLogger: invalid index offset");

            const auto index_count = event_serial::checked_u32_length(
                index_.size(), "index count");
            for (const auto& entry : index_) {
                write_tracked(
                    &entry.timestamp_us, sizeof(entry.timestamp_us));
                write_tracked(
                    &entry.file_offset, sizeof(entry.file_offset));
            }
            const auto index_offset =
                static_cast<uint64_t>(signed_index_offset);
            write_tracked(&index_offset, sizeof(index_offset));
            write_tracked(&index_count, sizeof(index_count));
            write_tracked(
                &EVENT_LOG_INDEX_MAGIC, sizeof(EVENT_LOG_INDEX_MAGIC));

            const auto seal_position = out_.tellp();
            if (seal_position == std::streampos(-1))
                throw std::runtime_error(
                    "EventLogger: cannot determine seal offset");
            const auto signed_covered_length =
                static_cast<std::streamoff>(seal_position);
            if (signed_covered_length < 0)
                throw std::runtime_error(
                    "EventLogger: invalid seal offset");

            const uint64_t record_count =
                static_cast<uint64_t>(event_count_);
            const uint64_t covered_length =
                static_cast<uint64_t>(signed_covered_length);
            const uint32_t contents_crc = file_crc_.value();
            EventLogCrc32c seal_crc;
            seal_crc.update(
                &EVENT_LOG_SEAL_MAGIC, sizeof(EVENT_LOG_SEAL_MAGIC));
            seal_crc.update(
                &EVENT_LOG_SEAL_VERSION, sizeof(EVENT_LOG_SEAL_VERSION));
            seal_crc.update(&record_count, sizeof(record_count));
            seal_crc.update(&index_offset, sizeof(index_offset));
            seal_crc.update(&covered_length, sizeof(covered_length));
            seal_crc.update(&contents_crc, sizeof(contents_crc));
            const uint32_t seal_checksum = seal_crc.value();

            // Commit in two durable phases.  The index/trailer prefix reaches
            // stable storage first while the ledger is still unsealed.  Only
            // then is the fixed terminal seal appended and synced.  A valid
            // seal is the crash-recovery commit authority; if the final sync
            // reports an error after all seal bytes reached the file, runtime
            // remains failed/uncertain even though recovery may accept the
            // independently verifiable seal.
            out_.flush();
            if (!out_)
                throw std::runtime_error(
                    "EventLogger: finalize prefix write failed");
            if (reservation_)
            {
                if (!reservation_->still_refers_to_reserved_file() ||
                    !reservation_->logical_path_still_refers_to_reserved_file() ||
                    !reservation_->sync() ||
                    !reservation_->logical_path_still_refers_to_reserved_file())
                    throw std::runtime_error(
                        "EventLogger: durable finalize prefix sync/path check "
                        "failed");
            }

            event_serial::write_u32(out_, EVENT_LOG_SEAL_MAGIC);
            event_serial::write_u32(out_, EVENT_LOG_SEAL_VERSION);
            event_serial::write_u64(out_, record_count);
            event_serial::write_u64(out_, index_offset);
            event_serial::write_u64(out_, covered_length);
            event_serial::write_u32(out_, contents_crc);
            event_serial::write_u32(out_, seal_checksum);
            out_.flush();
            if (!out_)
                throw std::runtime_error("EventLogger: finalize write failed");
            if (reservation_)
            {
                if (!reservation_->still_refers_to_reserved_file() ||
                    !reservation_->logical_path_still_refers_to_reserved_file() ||
                    !reservation_->sync() ||
                    !reservation_->logical_path_still_refers_to_reserved_file())
                    throw std::runtime_error(
                        "EventLogger: durable seal sync/path check failed");
            }
            state_ = logger_state::finalized;
            if (release_writer_lock)
                writer_lock_.release();
        } catch (...) {
            poison(std::current_exception());
        }
    }

public:
    // Called only by the logging-worker thread itself or by the engine after
    // that thread has joined. It is sticky and deliberately omits a complete
    // integrity seal. The flushed prefix may still be useful for diagnosis,
    // but it can never become an authoritative replay ledger.
    void abandon() noexcept
    {
        if (state_ == logger_state::finalized ||
            state_ == logger_state::poisoned ||
            state_ == logger_state::abandoned)
            return;
        state_ = logger_state::abandoned;
        try
        {
            out_.flush();
            out_.close();
            writer_lock_.release();
        }
        catch (...)
        {
        }
    }

private:
    enum class logger_state { open, finalized, poisoned, abandoned };

    std::string path_;
    std::shared_ptr<DurableEventLogReservation> reservation_;
    EventLogWriterLock writer_lock_;
    std::ofstream out_;
    bool compress_;
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx_;
    std::vector<uint8_t> compressed_buf_;
    std::vector<EventLogIndexEntry> index_;
    EventLogCrc32c file_crc_;
    size_t event_count_;
    logger_state state_ = logger_state::open;
    std::exception_ptr failure_;
    std::uint64_t max_bytes_ = 0;
    int max_files_ = 5;
    std::size_t durable_records_since_sync_ = 0;
    std::chrono::steady_clock::time_point last_durable_sync_{};
    static constexpr std::size_t DURABLE_SYNC_RECORD_INTERVAL = 256;
    static constexpr auto DURABLE_SYNC_TIME_INTERVAL =
        std::chrono::milliseconds(100);

    static bool requires_immediate_durable_sync(event_type type) noexcept
    {
        switch (type)
        {
        case event_type::order:
        case event_type::fill:
        case event_type::cancel:
        case event_type::amend:
        case event_type::rejection:
        case event_type::funding:
            return true;
        case event_type::market:
        case event_type::signal:
        case event_type::tick:
        case event_type::l2_snapshot:
        case event_type::l2_update:
            return false;
        }
        return true;
    }

    void durable_checkpoint()
    {
        if (!reservation_)
            return;
        out_.flush();
        if (!out_)
            throw std::runtime_error(
                "EventLogger: durable checkpoint flush failed");
        if (!reservation_->still_refers_to_reserved_file() ||
            !reservation_->logical_path_still_refers_to_reserved_file() ||
            !reservation_->sync() ||
            !reservation_->logical_path_still_refers_to_reserved_file())
            throw std::runtime_error(
                "EventLogger: durable checkpoint sync/path check failed");
        durable_records_since_sync_ = 0;
        // Measure the interval from completion, not from the pre-fsync call
        // site. A slow fsync must not make every following market record look
        // immediately overdue and collapse into an fsync feedback loop.
        last_durable_sync_ = std::chrono::steady_clock::now();
    }

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
        if (state_ == logger_state::abandoned)
            throw std::logic_error(
                "EventLogger: cannot use an abandoned ledger");
        if (state_ == logger_state::finalized)
            throw std::logic_error("EventLogger: cannot log after finalize");
    }

    void rethrow_if_poisoned() const
    {
        if (state_ == logger_state::poisoned)
            std::rethrow_exception(failure_);
    }

    void write_tracked(const void* bytes, std::size_t size)
    {
        if (size > static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max()))
            throw std::length_error(
                "EventLogger: tracked write is not stream-writable");
        if (size != 0)
        {
            out_.write(static_cast<const char*>(bytes),
                       static_cast<std::streamsize>(size));
            file_crc_.update(bytes, size);
        }
    }

    [[noreturn]] void poison(std::exception_ptr failure)
    {
        if (state_ != logger_state::poisoned)
        {
            state_ = logger_state::poisoned;
            failure_ = failure;
        }
        std::rethrow_exception(failure_);
    }

    void write_file_preamble()
    {
        const uint8_t version = EVENT_LOG_FILE_VERSION;
        const uint8_t flags = file_flags();
        write_tracked(EVENT_LOG_FILE_MAGIC.data(), EVENT_LOG_FILE_MAGIC.size());
        write_tracked(&version, sizeof(version));
        write_tracked(&flags, sizeof(flags));
        if (!out_)
            throw std::runtime_error("EventLogger: preamble write failed");
    }

    [[nodiscard]] uint8_t file_flags() const noexcept
    {
        uint8_t flags = compress_ ? EVENT_LOG_FILE_FLAG_ZSTD : uint8_t{0};
        if (max_bytes_ > 0)
            flags |= EVENT_LOG_FILE_FLAG_SEGMENTED;
        return flags;
    }

    void rotate()
    {
        finalize_impl(/*release_writer_lock=*/false);
        out_.close();
        if (out_.fail())
            throw std::runtime_error("EventLogger: close before rotate failed");

        auto nth = [&](int i) -> std::string {
            return path_ + "." + std::to_string(i);
        };
        auto remove_path = [](const std::string& path) {
            std::error_code ec;
            (void)std::filesystem::remove(path, ec);
            if (ec)
                throw std::runtime_error(
                    "EventLogger: rotate remove failed for " + path +
                    ": " + ec.message());
        };
        auto rename_if_present = [](const std::string& from,
                                    const std::string& to) {
            std::error_code ec;
            const bool exists = std::filesystem::exists(from, ec);
            if (ec)
                throw std::runtime_error(
                    "EventLogger: rotate stat failed for " + from +
                    ": " + ec.message());
            if (!exists)
                return;
            std::filesystem::rename(from, to, ec);
            if (ec)
                throw std::runtime_error(
                    "EventLogger: rotate rename failed from " + from +
                    " to " + to + ": " + ec.message());
        };
        if (max_files_ > 0) {
            std::string oldest = nth(max_files_);
            remove_path(oldest);
            for (int i = max_files_ - 1; i >= 1; --i)
                rename_if_present(nth(i), nth(i + 1));
            rename_if_present(path_, nth(1));
        } else {
            remove_path(path_);
        }

        writer_lock_.handoff_to(path_, /*create=*/true);
        out_.open(writer_lock_.open_path(),
                  std::ios::binary | std::ios::trunc);
        if (!out_)
            throw std::runtime_error("EventLogger: reopen after rotate failed: " + path_);
        index_.clear();
        event_count_ = 0;
        file_crc_.reset();
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

class EventReplaySource final
{
public:
    explicit EventReplaySource(const std::string& path)
        : path_(path)
    {
        const int candidate =
            ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (candidate < 0)
            throw std::runtime_error(
                "EventReplayer: cannot open " + path + ": " +
                std::strerror(errno));
        struct stat st {};
        if (::fstat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
        {
            (void)::close(candidate);
            throw std::runtime_error(
                "EventReplayer: source is not a readable regular file: " +
                path);
        }
        try
        {
            open_path_ = "/proc/self/fd/" + std::to_string(candidate);
        }
        catch (...)
        {
            (void)::close(candidate);
            throw;
        }
        fd_ = candidate;
    }

    ~EventReplaySource() noexcept
    {
        if (fd_ >= 0)
        {
            if (shared_lock_)
                (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
        }
    }

    EventReplaySource(const EventReplaySource&) = delete;
    EventReplaySource& operator=(const EventReplaySource&) = delete;

    [[nodiscard]] const std::string& open_path() const noexcept
    {
        return open_path_;
    }

    void acquire_shared_lock()
    {
        if (shared_lock_)
            return;
        if (::flock(fd_, LOCK_SH | LOCK_NB) != 0)
            throw std::runtime_error(
                "EventReplayer: sealed source is still owned by a writer");
        shared_lock_ = true;
    }

    void capture_snapshot()
    {
        struct stat descriptor {};
        struct stat visible {};
        if (::fstat(fd_, &descriptor) != 0 ||
            ::stat(path_.c_str(), &visible) != 0 ||
            !same_identity(descriptor, visible))
            throw std::runtime_error(
                "EventReplayer: source identity changed during open");
        snapshot_ = fingerprint(descriptor);
        snapshot_captured_ = true;
    }

    [[nodiscard]] bool stable() const noexcept
    {
        if (!snapshot_captured_ || fd_ < 0)
            return false;
        struct stat descriptor {};
        struct stat visible {};
        return ::fstat(fd_, &descriptor) == 0 &&
               ::stat(path_.c_str(), &visible) == 0 &&
               same_identity(descriptor, visible) &&
               same_fingerprint(snapshot_, fingerprint(descriptor));
    }

    void require_stable() const
    {
        if (!stable())
            throw std::runtime_error(
                "EventReplayer: source changed during replay");
    }

    [[nodiscard]] std::streamoff snapshot_size() const
    {
        if (!snapshot_captured_ || snapshot_.size < 0 ||
            static_cast<uint64_t>(snapshot_.size) >
                static_cast<uint64_t>(
                    std::numeric_limits<std::streamoff>::max()))
            throw std::runtime_error(
                "EventReplayer: invalid source snapshot size");
        return static_cast<std::streamoff>(snapshot_.size);
    }

private:
    struct source_fingerprint
    {
        dev_t device{};
        ino_t inode{};
        uid_t owner{};
        mode_t mode{};
        nlink_t links{};
        off_t size{};
        time_t modified_seconds{};
        long modified_nanoseconds{};
        time_t changed_seconds{};
        long changed_nanoseconds{};
    };

    static source_fingerprint fingerprint(const struct stat& st) noexcept
    {
        return {
            st.st_dev,
            st.st_ino,
            st.st_uid,
            st.st_mode,
            st.st_nlink,
            st.st_size,
            st.st_mtim.tv_sec,
            st.st_mtim.tv_nsec,
            st.st_ctim.tv_sec,
            st.st_ctim.tv_nsec,
        };
    }

    static bool same_identity(const struct stat& lhs,
                              const struct stat& rhs) noexcept
    {
        return S_ISREG(lhs.st_mode) && S_ISREG(rhs.st_mode) &&
               lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
               lhs.st_uid == rhs.st_uid;
    }

    static bool same_fingerprint(const source_fingerprint& lhs,
                                 const source_fingerprint& rhs) noexcept
    {
        return lhs.device == rhs.device && lhs.inode == rhs.inode &&
               lhs.owner == rhs.owner && lhs.mode == rhs.mode &&
               lhs.links == rhs.links && lhs.size == rhs.size &&
               lhs.modified_seconds == rhs.modified_seconds &&
               lhs.modified_nanoseconds == rhs.modified_nanoseconds &&
               lhs.changed_seconds == rhs.changed_seconds &&
               lhs.changed_nanoseconds == rhs.changed_nanoseconds;
    }

    std::string path_;
    std::string open_path_;
    int fd_ = -1;
    source_fingerprint snapshot_{};
    bool snapshot_captured_ = false;
    bool shared_lock_ = false;
};


class EventReplayer
{
    struct integrity_seal
    {
        uint64_t record_count = 0;
        uint64_t index_offset = 0;
        uint64_t covered_length = 0;
        uint32_t contents_crc = 0;
    };

public:
    explicit EventReplayer(const std::string& path,
                           int64_t replay_from_us = 0,
                           int64_t replay_to_us = INT64_MAX,
                           EventReplayLimits limits = {})
        : source_(path)
        , in_(source_.open_path(), std::ios::binary)
        , compressed_(false)
        , dctx_(nullptr, &ZSTD_freeDCtx)
        , replay_from_us_(replay_from_us)
        , replay_to_us_(replay_to_us)
        , limits_(limits)
    {
        if (!in_)
            throw std::runtime_error("EventReplayer: cannot open " + path);
        source_.capture_snapshot();

        in_.seekg(0, std::ios::end);
        const auto file_end = in_.tellg();
        if (file_end == std::streampos(-1))
            throw std::runtime_error("EventReplayer: cannot determine file size");

        const auto file_size = static_cast<std::streamoff>(file_end);
        if (file_size < 0)
            throw std::runtime_error("EventReplayer: invalid file size");
        const bool has_file_preamble = read_file_preamble(file_size);
        data_end_ = file_size;

        if (integrity_records_)
        {
            integrity_seal seal{};
            if (try_read_integrity_seal(file_size, seal))
            {
                file_finalized_ = true;
                if (seal.covered_length <
                    static_cast<uint64_t>(
                        EVENT_LOG_FILE_PREAMBLE_BYTES +
                        EVENT_LOG_INDEX_TRAILER_BYTES))
                    throw std::runtime_error(
                        "EventReplayer: integrity seal covered length is "
                        "too small");
                const uint64_t trailer_offset = seal.covered_length -
                    static_cast<uint64_t>(EVENT_LOG_INDEX_TRAILER_BYTES);
                seek_absolute(static_cast<std::streamoff>(trailer_offset));
                uint64_t index_offset = 0;
                uint32_t entry_count = 0;
                uint32_t magic = 0;
                read_exact(&index_offset, 8, "index offset");
                read_exact(&entry_count, 4, "index count");
                read_exact(&magic, 4, "index magic");
                if (magic != EVENT_LOG_INDEX_MAGIC ||
                    index_offset != seal.index_offset)
                    throw std::runtime_error(
                        "EventReplayer: integrity seal/index mismatch");
                const uint64_t index_bytes =
                    static_cast<uint64_t>(entry_count) * 16U;
                if (entry_count > limits_.max_index_entries ||
                    index_bytes > trailer_offset ||
                    index_offset < static_cast<uint64_t>(data_begin_) ||
                    index_offset != trailer_offset - index_bytes)
                    throw std::runtime_error(
                        "EventReplayer: invalid sealed index trailer");

                data_end_ = static_cast<std::streamoff>(index_offset);
                const uint64_t observed_records =
                    validate_index_and_seek(index_offset, entry_count);
                if (observed_records != seal.record_count)
                    throw std::runtime_error(
                        "EventReplayer: sealed record count mismatch");
            }
            else
            {
                seek_absolute(data_begin_);
            }
        }
        else
        {
            const bool requires_index_trailer =
                has_file_preamble && file_finalized_;
            if (requires_index_trailer &&
                file_size < EVENT_LOG_INDEX_TRAILER_BYTES)
                throw std::runtime_error(
                    "EventReplayer: finalized file is missing index trailer");

            if (requires_index_trailer ||
                (!has_file_preamble &&
                 file_size >= EVENT_LOG_INDEX_TRAILER_BYTES))
            {
            seek_absolute(file_size - EVENT_LOG_INDEX_TRAILER_BYTES);
            uint64_t index_offset = 0;
            uint32_t entry_count = 0;
            uint32_t magic = 0;
            read_exact(&index_offset, 8, "index offset");
            read_exact(&entry_count, 4, "index count");
            read_exact(&magic, 4, "index magic");

            if (magic == EVENT_LOG_INDEX_MAGIC) {
                const auto trailer_offset =
                    static_cast<uint64_t>(
                        file_size - EVENT_LOG_INDEX_TRAILER_BYTES);
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
                (void)validate_index_and_seek(index_offset, entry_count);
            } else if (requires_index_trailer) {
                throw std::runtime_error(
                    "EventReplayer: finalized file is missing index trailer");
            } else {
                seek_absolute(data_begin_);
            }
            }
            else
            {
            seek_absolute(data_begin_);
            }
        }

        if (!has_file_preamble)
            compressed_ = detect_legacy_compression();
        if (compressed_)
            create_decompression_context();
        source_.require_stable();
    }

    ~EventReplayer() = default;

    EventReplayer(const EventReplayer&) = delete;
    EventReplayer& operator=(const EventReplayer&) = delete;

    bool has_next() const
    {
        source_.require_stable();
        if (failed_ || !in_.good()) return false;
        const auto pos = in_.tellg();
        if (pos == std::streampos(-1)) return false;
        return static_cast<std::streamoff>(pos) < data_end_;
    }

    uint8_t file_version() const noexcept { return file_version_; }
    bool file_finalized() const noexcept
    {
        return file_finalized_ && source_.stable();
    }
    bool file_segmented() const noexcept { return file_segmented_; }

    event_pointer next()
    {
        if (failed_)
            std::rethrow_exception(failure_);

        try {
            source_.require_stable();
            auto replayed = next_impl();
            source_.require_stable();
            return replayed;
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
            const std::streamoff record_overhead =
                integrity_records_ ? 9 : 5;
            if (data_end_ - record_offset < record_overhead)
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
            const uint64_t required_tail =
                static_cast<uint64_t>(payload_size) +
                (integrity_records_ ? sizeof(uint32_t) : 0U);
            if (remaining < 0 ||
                static_cast<uint64_t>(remaining) < required_tail)
                throw std::runtime_error("EventReplayer: truncated payload");

            std::vector<uint8_t> raw(static_cast<std::size_t>(payload_size));
            if (!raw.empty())
                read_exact(raw.data(), static_cast<std::streamsize>(raw.size()),
                           "payload");

            if (integrity_records_)
            {
                uint32_t stored_checksum = 0;
                read_exact(&stored_checksum, sizeof(stored_checksum),
                           "record checksum");
                EventLogCrc32c record_crc;
                record_crc.update(&type_byte, sizeof(type_byte));
                record_crc.update(&payload_size, sizeof(payload_size));
                record_crc.update(raw.data(), raw.size());
                if (stored_checksum != record_crc.value())
                    throw std::runtime_error(
                        "EventReplayer: record checksum mismatch");
            }

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

    EventReplaySource source_;
    mutable std::ifstream in_;
    bool compressed_;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx_;
    int64_t replay_from_us_;
    int64_t replay_to_us_;
    EventReplayLimits limits_;
    std::streamoff data_begin_ = 0;
    std::streamoff data_end_ = 0;
    bool file_finalized_ = false;
    bool file_segmented_ = false;
    uint8_t file_version_ = 0; // 0 = headerless legacy stream
    bool integrity_records_ = false;
    bool failed_ = false;
    std::exception_ptr failure_;

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
        if (version != EVENT_LOG_FILE_VERSION &&
            version != EVENT_LOG_PREVIOUS_FILE_VERSION &&
            version != EVENT_LOG_LEGACY_FILE_VERSION)
            throw std::runtime_error("EventReplayer: unsupported file version");
        if ((flags & ~EVENT_LOG_FILE_KNOWN_FLAGS) != 0)
            throw std::runtime_error("EventReplayer: unsupported file flags");

        file_version_ = version;
        data_begin_ = EVENT_LOG_FILE_PREAMBLE_BYTES;
        compressed_ = (flags & EVENT_LOG_FILE_FLAG_ZSTD) != 0;
        integrity_records_ = version == EVENT_LOG_FILE_VERSION;
        if (integrity_records_ &&
            (flags & EVENT_LOG_FILE_FLAG_FINALIZED) != 0)
            throw std::runtime_error(
                "EventReplayer: v3 finalization must use a terminal seal");
        file_finalized_ = !integrity_records_ &&
            (flags & EVENT_LOG_FILE_FLAG_FINALIZED) != 0;
        file_segmented_ = (flags & EVENT_LOG_FILE_FLAG_SEGMENTED) != 0;
        return true;
    }

    bool try_read_integrity_seal(std::streamoff file_size,
                                 integrity_seal& seal)
    {
        if (file_size < EVENT_LOG_FILE_PREAMBLE_BYTES + EVENT_LOG_SEAL_BYTES)
            return false;

        const auto seal_offset = file_size - EVENT_LOG_SEAL_BYTES;
        seek_absolute(seal_offset);

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t seal_checksum = 0;
        read_exact(&magic, sizeof(magic), "integrity seal magic");
        if (magic != EVENT_LOG_SEAL_MAGIC)
            return false;
        // A terminal seal is authoritative only while this reader owns a
        // shared advisory lock and the pinned inode/path metadata remains the
        // exact snapshot validated below. Re-read the seal after locking; the
        // first magic read is only a lock-routing hint.
        source_.acquire_shared_lock();
        source_.capture_snapshot();
        if (source_.snapshot_size() != file_size)
            throw std::runtime_error(
                "EventReplayer: source size changed before seal validation");
        seek_absolute(seal_offset);
        read_exact(&magic, sizeof(magic), "integrity seal magic");
        if (magic != EVENT_LOG_SEAL_MAGIC)
            throw std::runtime_error(
                "EventReplayer: integrity seal changed while locking source");
        read_exact(&version, sizeof(version), "integrity seal version");
        read_exact(&seal.record_count, sizeof(seal.record_count),
                   "integrity seal record count");
        read_exact(&seal.index_offset, sizeof(seal.index_offset),
                   "integrity seal index offset");
        read_exact(&seal.covered_length, sizeof(seal.covered_length),
                   "integrity seal covered length");
        read_exact(&seal.contents_crc, sizeof(seal.contents_crc),
                   "integrity seal contents checksum");
        read_exact(&seal_checksum, sizeof(seal_checksum),
                   "integrity seal checksum");

        if (version != EVENT_LOG_SEAL_VERSION)
            throw std::runtime_error(
                "EventReplayer: unsupported integrity seal version");
        if (seal.covered_length != static_cast<uint64_t>(seal_offset))
            throw std::runtime_error(
                "EventReplayer: integrity seal is not terminal");

        EventLogCrc32c expected_seal_crc;
        expected_seal_crc.update(&magic, sizeof(magic));
        expected_seal_crc.update(&version, sizeof(version));
        expected_seal_crc.update(
            &seal.record_count, sizeof(seal.record_count));
        expected_seal_crc.update(
            &seal.index_offset, sizeof(seal.index_offset));
        expected_seal_crc.update(
            &seal.covered_length, sizeof(seal.covered_length));
        expected_seal_crc.update(
            &seal.contents_crc, sizeof(seal.contents_crc));
        if (seal_checksum != expected_seal_crc.value())
            throw std::runtime_error(
                "EventReplayer: integrity seal checksum mismatch");

        EventLogCrc32c contents_crc;
        seek_absolute(0);
        std::array<uint8_t, 64U * 1024U> buffer{};
        uint64_t remaining = seal.covered_length;
        while (remaining != 0)
        {
            const auto chunk = static_cast<std::size_t>(std::min<uint64_t>(
                remaining, static_cast<uint64_t>(buffer.size())));
            read_exact(buffer.data(), static_cast<std::streamsize>(chunk),
                       "integrity-covered contents");
            contents_crc.update(buffer.data(), chunk);
            remaining -= chunk;
        }
        if (seal.contents_crc != contents_crc.value())
            throw std::runtime_error(
                "EventReplayer: integrity-covered contents checksum mismatch");
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

    uint64_t validate_index_and_seek(uint64_t index_offset,
                                     uint32_t entry_count)
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
        uint64_t record_count = 0;

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

            const std::streamoff record_overhead =
                integrity_records_ ? 9 : 5;
            if (index_end - record_offset < record_overhead) {
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
            const uint64_t required_tail =
                static_cast<uint64_t>(payload_size) +
                (integrity_records_ ? sizeof(uint32_t) : 0U);
            if (remaining < 0 ||
                static_cast<uint64_t>(remaining) < required_tail) {
                throw std::runtime_error(
                    "EventReplayer: index cuts through a record");
            }
            record_offset = payload_offset +
                            static_cast<std::streamoff>(required_tail);
            if (record_count == std::numeric_limits<uint64_t>::max())
                throw std::runtime_error(
                    "EventReplayer: record count overflow");
            ++record_count;
            seek_absolute(record_offset);
        }

        if (record_offset != index_end || have_pending) {
            throw std::runtime_error("EventReplayer: invalid index record boundary");
        }

        if (timestamps_sorted && have_best_offset)
            seek_absolute(static_cast<std::streamoff>(best_offset));
        else
            seek_absolute(data_begin_);
        return record_count;
    }
};
