#pragma once

#include "execution/portfolio.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace checkpoint {

constexpr uint32_t kMagic   = 0x43484b50;
constexpr uint32_t kVersion = 1;

struct CheckpointData
{
    uint64_t event_count = 0;
    int64_t  wall_ms = 0;
    double   cash = 0.0;
    double   initial_balance = 0.0;
    uint64_t total_trades = 0;
    struct PosEntry { std::string symbol; double qty; double cost_basis; };
    std::vector<PosEntry> positions;
};

inline void write_file(const std::string& path, const portfolio& p,
                       uint64_t event_count, int64_t wall_ms)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) throw std::runtime_error("checkpoint: cannot open " + path);

    auto write_u32 = [&](uint32_t v) { ofs.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u64 = [&](uint64_t v) { ofs.write(reinterpret_cast<const char*>(&v), 8); };
    auto write_i64 = [&](int64_t v)  { ofs.write(reinterpret_cast<const char*>(&v), 8); };
    auto write_f64 = [&](double v)   { ofs.write(reinterpret_cast<const char*>(&v), 8); };

    write_u32(kMagic);
    write_u32(kVersion);
    write_u64(event_count);
    write_i64(wall_ms);
    write_f64(p.get_cash());
    write_f64(p.get_initial_balance());
    write_u64(static_cast<uint64_t>(p.get_total_trades()));

    const auto& positions = p.get_positions();
    uint32_t live = 0;
    for (const auto& [_, pos] : positions)
        if (std::abs(pos.qty) > 1e-12) ++live;
    write_u32(live);

    for (const auto& [sym, pos] : positions)
    {
        if (std::abs(pos.qty) <= 1e-12) continue;
        uint16_t slen = static_cast<uint16_t>(sym.size());
        ofs.write(reinterpret_cast<const char*>(&slen), 2);
        ofs.write(sym.data(), slen);
        write_f64(pos.qty);
        write_f64(pos.cost_basis);
    }

    if (!ofs) throw std::runtime_error("checkpoint: write failed");
}

inline CheckpointData read_file(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("checkpoint: cannot open " + path);

    auto read_u32 = [&]() -> uint32_t { uint32_t v; ifs.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto read_u64 = [&]() -> uint64_t { uint64_t v; ifs.read(reinterpret_cast<char*>(&v), 8); return v; };
    auto read_i64 = [&]() -> int64_t  { int64_t v;  ifs.read(reinterpret_cast<char*>(&v), 8); return v; };
    auto read_f64 = [&]() -> double   { double v;   ifs.read(reinterpret_cast<char*>(&v), 8); return v; };

    uint32_t magic = read_u32();
    if (magic != kMagic) throw std::runtime_error("checkpoint: bad magic");
    uint32_t version = read_u32();
    if (version != kVersion)
        throw std::runtime_error("checkpoint: unsupported version " + std::to_string(version));

    CheckpointData cp;
    cp.event_count     = read_u64();
    cp.wall_ms         = read_i64();
    cp.cash            = read_f64();
    cp.initial_balance = read_f64();
    cp.total_trades    = read_u64();

    uint32_t n = read_u32();
    cp.positions.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t slen = 0;
        ifs.read(reinterpret_cast<char*>(&slen), 2);
        std::string sym(slen, '\0');
        ifs.read(sym.data(), slen);
        double qty = read_f64();
        double basis = read_f64();
        cp.positions.push_back({std::move(sym), qty, basis});
    }

    if (!ifs) throw std::runtime_error("checkpoint: read truncated");
    return cp;
}

}

class engine_config;  // fwd
class portfolio;

// CheckpointManager (final extraction). Encapsulates due-check + restore.
// Uses the low-level write/read in namespace above. Cold path.
class CheckpointManager {
public:
    explicit CheckpointManager(const engine_config& cfg);

    void write_if_due(const portfolio& p, std::size_t event_count);
    void restore(portfolio& p);

private:
    const engine_config& cfg_;
};
