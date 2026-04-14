#pragma once

#include <stdexcept>
#include <string>

enum class spin_policy { spin, yield, adaptive };

inline std::string spin_policy_to_string(spin_policy p)
{
    switch (p) {
    case spin_policy::spin:     return "spin";
    case spin_policy::yield:    return "yield";
    case spin_policy::adaptive: return "adaptive";
    }
    return "adaptive";
}

inline spin_policy string_to_spin_policy(const std::string& s)
{
    if (s == "spin")     return spin_policy::spin;
    if (s == "yield")    return spin_policy::yield;
    if (s == "adaptive") return spin_policy::adaptive;
    throw std::invalid_argument("unknown spin policy: " + s);
}
