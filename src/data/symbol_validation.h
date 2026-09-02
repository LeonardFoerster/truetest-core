#pragma once

#include <cstddef>
#include <string_view>

namespace tt::symbol_validation {

inline constexpr std::size_t kMaximumLength = 256;

// Supported instrument identities are non-empty printable ASCII tokens.
// This deliberately rejects whitespace, controls and non-UTF-8/high bytes at
// ingress instead of allowing an opaque byte string to corrupt logs, JSON or
// cross-process identity comparisons later.
[[nodiscard]] constexpr bool valid(std::string_view symbol) noexcept
{
    if (symbol.empty() || symbol.size() > kMaximumLength) return false;
    for (const unsigned char byte : symbol)
        if (byte < 0x21 || byte > 0x7e) return false;
    return true;
}

} // namespace tt::symbol_validation
