#pragma once

#include "event.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <zstd.h>


namespace event_serial {


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
        uint16_t len = static_cast<uint16_t>(s.size());
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
        uint16_t len = static_cast<uint16_t>(s.size());
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
        uint16_t len = static_cast<uint16_t>(s.size());
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
        double remaining = 0.0;
        uint64_t fill_id = 0;
        fill_source src = fill_source::unknown;
        try {
            remaining = r.read_f64();
            fill_id = r.read_u64();
            src = static_cast<fill_source>(r.read_u8());
        } catch (...) {}
        auto ev = std::make_shared<fill_event>(ts, symbol, oid, sd, qty, price,
                                               commission, remaining, fill_id);
        ev->set_source(src);
        return ev;
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
    case event_type::cancel: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto reason = r.read_str();
        return std::make_shared<cancel_event>(ts, symbol, oid, reason);
    }
    case event_type::amend: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        double new_price = r.read_f64();
        double new_qty = r.read_f64();
        return std::make_shared<amend_event>(ts, symbol, oid, new_price, new_qty);
    }
    case event_type::rejection: {
        auto ts = r.read_ts();
        auto symbol = r.read_str();
        uint64_t oid = r.read_u64();
        auto reason = r.read_str();
        return std::make_shared<rejection_event>(ts, symbol, oid, reason);
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


class EventLogger
{
public:
    explicit EventLogger(const std::string& path,
                         bool compress = true,
                         std::uint64_t max_bytes = 0,
                         int max_files = 5)
        : path_(path)
        , out_(path, std::ios::binary | std::ios::trunc)
        , compress_(compress)
        , cctx_(nullptr)
        , event_count_(0)
        , max_bytes_(max_bytes)
        , max_files_(max_files)
    {
        if (!out_)
            throw std::runtime_error("EventLogger: cannot open " + path);
        if (compress_) {
            cctx_ = ZSTD_createCCtx();
            if (!cctx_)
                throw std::runtime_error("EventLogger: failed to create zstd context");
        }
    }

    ~EventLogger()
    {
        finalize();
        if (cctx_) ZSTD_freeCCtx(cctx_);
    }

    EventLogger(const EventLogger&) = delete;
    EventLogger& operator=(const EventLogger&) = delete;

    void log(const event& e)
    {
        if (event_count_ % EVENT_LOG_INDEX_INTERVAL == 0) {
            EventLogIndexEntry entry{};
            auto ts = e.get_timestamp();
            entry.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                ts.time_since_epoch()).count();
            entry.file_offset = static_cast<uint64_t>(out_.tellp());
            index_.push_back(entry);
        }
        ++event_count_;

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
        case event_type::cancel:
            payload = event_serial::serialise(static_cast<const cancel_event&>(e)); break;
        case event_type::amend:
            payload = event_serial::serialise(static_cast<const amend_event&>(e)); break;
        case event_type::rejection:
            payload = event_serial::serialise(static_cast<const rejection_event&>(e)); break;
        }

        event_serial::write_u8(out_, static_cast<uint8_t>(e.get_type()));

        if (compress_) {
            size_t bound = ZSTD_compressBound(payload.size());
            compressed_buf_.resize(bound);
            size_t compressed_size = ZSTD_compressCCtx(
                cctx_, compressed_buf_.data(), bound,
                payload.data(), payload.size(), 1);
            if (ZSTD_isError(compressed_size))
                throw std::runtime_error(std::string("zstd compress: ") + ZSTD_getErrorName(compressed_size));
            event_serial::write_u32(out_, static_cast<uint32_t>(compressed_size));
            out_.write(reinterpret_cast<const char*>(compressed_buf_.data()),
                       static_cast<std::streamsize>(compressed_size));
        } else {
            event_serial::write_u32(out_, static_cast<uint32_t>(payload.size()));
            out_.write(reinterpret_cast<const char*>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
        }

        if (max_bytes_ > 0 &&
            static_cast<std::uint64_t>(out_.tellp()) >= max_bytes_)
        {
            rotate();
        }
    }

    void flush() { out_.flush(); }

    void finalize()
    {
        if (finalized_) return;
        finalized_ = true;

        uint64_t index_offset = static_cast<uint64_t>(out_.tellp());
        for (const auto& entry : index_) {
            event_serial::write_i64(out_, entry.timestamp_us);
            event_serial::write_u64(out_, entry.file_offset);
        }
        event_serial::write_u64(out_, index_offset);
        event_serial::write_u32(out_, static_cast<uint32_t>(index_.size()));
        event_serial::write_u32(out_, EVENT_LOG_INDEX_MAGIC);
        out_.flush();
    }

private:
    std::string path_;
    std::ofstream out_;
    bool compress_;
    ZSTD_CCtx* cctx_;
    std::vector<uint8_t> compressed_buf_;
    std::vector<EventLogIndexEntry> index_;
    size_t event_count_;
    bool finalized_ = false;
    std::uint64_t max_bytes_ = 0;
    int max_files_ = 5;

    void rotate()
    {
        finalize();
        out_.close();

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

        out_.open(path_, std::ios::binary | std::ios::trunc);
        if (!out_)
            throw std::runtime_error("EventLogger: reopen after rotate failed: " + path_);
        index_.clear();
        event_count_ = 0;
        finalized_ = false;
    }
};


class EventReplayer
{
public:
    explicit EventReplayer(const std::string& path,
                           int64_t replay_from_us = 0,
                           int64_t replay_to_us = INT64_MAX)
        : in_(path, std::ios::binary)
        , compressed_(false)
        , dctx_(nullptr)
        , replay_from_us_(replay_from_us)
        , replay_to_us_(replay_to_us)
    {
        if (!in_)
            throw std::runtime_error("EventReplayer: cannot open " + path);

        in_.seekg(0, std::ios::end);
        auto file_size = in_.tellg();
        if (file_size >= 16) {
            in_.seekg(-16, std::ios::end);
            uint64_t index_offset = 0;
            uint32_t entry_count = 0;
            uint32_t magic = 0;
            in_.read(reinterpret_cast<char*>(&index_offset), 8);
            in_.read(reinterpret_cast<char*>(&entry_count), 4);
            in_.read(reinterpret_cast<char*>(&magic), 4);
            if (magic == EVENT_LOG_INDEX_MAGIC) {
                has_index_ = true;
                data_end_ = static_cast<std::streampos>(index_offset);
                if (entry_count > 0) {
                    in_.seekg(static_cast<std::streampos>(index_offset));
                    index_.resize(entry_count);
                    for (uint32_t i = 0; i < entry_count; ++i) {
                        in_.read(reinterpret_cast<char*>(&index_[i].timestamp_us), 8);
                        in_.read(reinterpret_cast<char*>(&index_[i].file_offset), 8);
                    }
                }
            } else {
                data_end_ = file_size;
            }
        }

        if (has_index_ && replay_from_us_ > 0 && !index_.empty()) {
            auto it = std::upper_bound(index_.begin(), index_.end(), replay_from_us_,
                [](int64_t ts, const EventLogIndexEntry& e) { return ts < e.timestamp_us; });
            if (it != index_.begin()) --it;
            in_.seekg(static_cast<std::streampos>(it->file_offset));
        } else {
            in_.seekg(0);
        }

        auto start_pos = in_.tellg();
        uint8_t type_byte = 0;
        in_.read(reinterpret_cast<char*>(&type_byte), 1);
        if (in_) {
            uint32_t payload_size = 0;
            in_.read(reinterpret_cast<char*>(&payload_size), 4);
            if (in_ && payload_size >= 4) {
                uint32_t zmagic = 0;
                in_.read(reinterpret_cast<char*>(&zmagic), 4);
                if (zmagic == 0xFD2FB528) {
                    compressed_ = true;
                    dctx_ = ZSTD_createDCtx();
                    if (!dctx_)
                        throw std::runtime_error("EventReplayer: failed to create zstd context");
                }
            }
        }
        in_.clear();
        in_.seekg(start_pos);
    }

    ~EventReplayer()
    {
        if (dctx_) ZSTD_freeDCtx(dctx_);
    }

    EventReplayer(const EventReplayer&) = delete;
    EventReplayer& operator=(const EventReplayer&) = delete;

    bool has_next() const
    {
        if (!in_.good()) return false;
        auto pos = in_.tellg();
        if (has_index_ && pos >= data_end_) return false;
        return in_.peek() != std::ifstream::traits_type::eof();
    }

    event_pointer next()
    {
        while (true) {
            if (has_index_ && in_.tellg() >= data_end_) return nullptr;

            uint8_t type_byte = 0;
            in_.read(reinterpret_cast<char*>(&type_byte), 1);
            if (!in_) return nullptr;

            uint32_t payload_size = 0;
            in_.read(reinterpret_cast<char*>(&payload_size), 4);
            if (!in_) return nullptr;

            std::vector<uint8_t> raw(payload_size);
            in_.read(reinterpret_cast<char*>(raw.data()), payload_size);
            if (!in_) return nullptr;

            event_pointer ev;
            if (compressed_) {
                unsigned long long decompressed_size = ZSTD_getFrameContentSize(raw.data(), raw.size());
                if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
                    decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
                    decompressed_size = payload_size * 10;
                }
                std::vector<uint8_t> decompressed(static_cast<size_t>(decompressed_size));
                size_t result = ZSTD_decompressDCtx(
                    dctx_, decompressed.data(), decompressed.size(),
                    raw.data(), raw.size());
                if (ZSTD_isError(result))
                    throw std::runtime_error(std::string("zstd decompress: ") + ZSTD_getErrorName(result));
                ev = event_serial::deserialise(
                    static_cast<event_type>(type_byte), decompressed.data(), result);
            } else {
                ev = event_serial::deserialise(
                    static_cast<event_type>(type_byte), raw.data(), payload_size);
            }

            auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                ev->get_timestamp().time_since_epoch()).count();
            if (ts_us > replay_to_us_) return nullptr;
            if (ts_us < replay_from_us_) continue;
            return ev;
        }
    }

private:
    mutable std::ifstream in_;
    bool compressed_;
    ZSTD_DCtx* dctx_;
    int64_t replay_from_us_;
    int64_t replay_to_us_;
    bool has_index_ = false;
    std::streampos data_end_ = 0;
    std::vector<EventLogIndexEntry> index_;
};
