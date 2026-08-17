#pragma once

#include "execution/portfolio.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace checkpoint {

constexpr uint32_t kMagic   = 0x43484b50;
constexpr uint32_t kVersion = 1;

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

}

class engine_config;  // fwd
class portfolio;

// CheckpointManager (final extraction). Encapsulates due-check + V1 refusal.
// Uses the low-level diagnostic writer in the namespace above. Cold path.
class CheckpointManager {
public:
    explicit CheckpointManager(const engine_config& cfg);

    void write_if_due(const portfolio& p, std::size_t event_count);
    void restore(portfolio& p);

private:
    const engine_config& cfg_;
};
