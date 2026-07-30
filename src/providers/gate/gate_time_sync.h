#pragma once
#ifdef HAS_GATE

// Clock-skew refuse helpers for Gate REST Timestamp (Unix seconds).
// /spot/time returns ms; offset is computed in ms; SIGN uses seconds.

#include "providers/gate/gate_rest_client.h"

#include <climits>
#include <string>

namespace gate {

struct clock_check
{
    bool ok = false;
    long long offset_ms = 0;
    std::string note;
};

// Pure-logic decision so tests can drive it without a live GateRestClient.
// Default tolerance 2000 ms (stricter than Gate's ~15 min venue window).
inline clock_check verify_clock_skew_offset(long long offset_ms,
                                            long long tolerance_ms = 2000)
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

inline clock_check verify_clock_skew(GateRestClient& rest,
                                     long long tolerance_ms = 2000)
{
    long long offset = rest.server_time_offset_ms();
    return verify_clock_skew_offset(offset, tolerance_ms);
}

} // namespace gate

#endif // HAS_GATE
