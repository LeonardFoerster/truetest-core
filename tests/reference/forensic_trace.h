#pragma once
// TEMPORARY FORENSIC INSTRUMENTATION (audit 2026-08-22).
// Env-gated: with TT_FORENSIC unset every call compiles to a predictable
// branch on a cached flag and writes nothing. No RNG, no clock, no state
// mutation, no allocation on the disabled path.
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace tt::forensic {

inline std::FILE* stream()
{
    static std::FILE* f = []() -> std::FILE* {
        const char* p = std::getenv("TT_FORENSIC");
        if (!p || !*p) return nullptr;
        std::FILE* out = std::fopen(p, "w");
        return out;
    }();
    return f;
}

inline bool on() { return stream() != nullptr; }

template <typename... Args>
inline void log(const char* fmt, Args... args)
{
    std::FILE* f = stream();
    if (!f) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    std::fprintf(f, fmt, args...);
    std::fputc('\n', f);
}

// Monotonic bar counter maintained by the engine loop so every record can be
// anchored to an engine iteration index.
inline long long& bar_index()
{
    static long long idx = -1;
    return idx;
}

} // namespace tt::forensic

#define TT_FTRACE(...) do { if (::tt::forensic::on()) ::tt::forensic::log(__VA_ARGS__); } while (0)
