#pragma once
#ifdef HAS_BYBIT

#include "providers/bybit/bybit_rest_client.h"

#include <climits>
#include <string>

namespace bybit {

struct clock_check
{
    bool ok = false;
    long long offset_ms = 0;
    std::string note;
};

// Pure-logic decision so tests can drive it without a live BybitRestClient.
// Default tolerance 1000 ms (recv_window policy; guide §10.4).
inline clock_check verify_clock_skew_offset(long long offset_ms,
                                            long long tolerance_ms = 1000)
{
    clock_check out;
    if (offset_ms == LLONG_MIN)
    {
        out.ok = false;
        out.offset_ms = 0;
        out.note = "clock skew: failed to fetch server time";
        return out;
    }
    long long abs_off = offset_ms < 0 ? -offset_ms : offset_ms;
    out.offset_ms = offset_ms;
    if (abs_off > tolerance_ms)
    {
        out.ok = false;
        out.note = "clock skew: drift " + std::to_string(offset_ms)
                   + " ms exceeds tolerance " + std::to_string(tolerance_ms)
                   + " ms";
        return out;
    }
    out.ok = true;
    out.note = "clock skew within tolerance";
    return out;
}

inline clock_check verify_clock_skew(BybitRestClient& rest,
                                     long long tolerance_ms = 1000)
{
    long long offset = rest.server_time_offset_ms();
    return verify_clock_skew_offset(offset, tolerance_ms);
}

} // namespace bybit

#endif // HAS_BYBIT
