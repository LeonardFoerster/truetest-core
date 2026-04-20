#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

enum class thread_preset
{
    inline_mode,
    light,
    standard,
    full,
    extended
};

inline thread_preset select_preset(std::size_t physical_cores)
{
    if (physical_cores <= 2) return thread_preset::inline_mode;
    if (physical_cores <= 3) return thread_preset::light;
    if (physical_cores <= 5) return thread_preset::standard;
    if (physical_cores <= 7) return thread_preset::full;
    return thread_preset::extended;
}

inline std::string preset_to_string(thread_preset p)
{
    switch (p) {
    case thread_preset::inline_mode: return "inline";
    case thread_preset::light:       return "light";
    case thread_preset::standard:    return "standard";
    case thread_preset::full:        return "full";
    case thread_preset::extended:    return "extended";
    }
    return "unknown";
}

inline thread_preset string_to_preset(const std::string& s)
{
    if (s == "inline")   return thread_preset::inline_mode;
    if (s == "light")    return thread_preset::light;
    if (s == "standard") return thread_preset::standard;
    if (s == "full")     return thread_preset::full;
    if (s == "extended") return thread_preset::extended;
    throw std::invalid_argument("unknown thread preset: " + s);
}

inline int preset_worker_count(thread_preset p)
{
    switch (p) {
    case thread_preset::inline_mode: return 0;
    case thread_preset::light:       return 1;
    case thread_preset::standard:    return 2;
    case thread_preset::full:        return 3;
    case thread_preset::extended:    return 4;
    }
    return 0;
}

inline bool preset_has_mm_worker(thread_preset p)
{
    return p == thread_preset::extended;
}

inline bool preset_has_separate_risk(thread_preset p)
{
    return p == thread_preset::full || p == thread_preset::extended;
}

inline bool preset_has_separate_logging(thread_preset p)
{
    return p == thread_preset::standard || p == thread_preset::full
        || p == thread_preset::extended;
}
