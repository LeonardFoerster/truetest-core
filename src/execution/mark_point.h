#pragma once

// Authoritative per-instrument mark state (risk register R3).
//
// A mark without a timestamp cannot be distinguished from an arbitrarily old
// one, which is exactly how "mark-to-market" risk silently degrades into
// "mark-to-whatever-we-last-saw". The engine therefore stores price *and* the
// simulation/event timestamp it was observed at, and risk classifies the mark
// as valid / stale / missing from that pair. Using the sim clock (not wall
// clock) keeps backtests deterministic.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

struct mark_point
{
    double price = 0.0;
    std::chrono::system_clock::time_point ts{};

    [[nodiscard]] bool usable() const noexcept
    {
        return std::isfinite(price) && price > 0.0;
    }
};

// Age in milliseconds relative to `now`. Returns -1 when the age is unknown
// (no observation timestamp recorded) and 0 for a mark stamped in the future,
// which a replay boundary can legitimately produce.
[[nodiscard]] inline std::int64_t mark_age_ms(
    const mark_point& mark,
    std::chrono::system_clock::time_point now) noexcept
{
    if (mark.ts.time_since_epoch().count() == 0
        || now.time_since_epoch().count() == 0)
        return -1;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - mark.ts).count();
    return age > 0 ? age : 0;
}
