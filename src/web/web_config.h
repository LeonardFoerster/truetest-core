#pragma once

#include <string>

namespace truetest::web {

// Runtime configuration for the embedded web UI server. Populated from the
// --web* CLI flags at the argument-parsing edge (src/bin/main.inc).
struct web_config
{
    std::string bind_addr = "127.0.0.1";  // localhost by default — never 0.0.0.0 implicitly
    int         port      = 8080;
    std::string token;                    // optional bearer token; empty = no auth
    std::string asset_dir;                // built SPA dir to serve at "/"; empty = API/WS only
    int         poll_hz   = 10;           // live snapshot broadcast rate over /stream

    // engine_live exposes data only — no order/flatten/kill routes are ever
    // registered. Kept as an explicit flag so the intent is visible at the
    // callsite; v1 is read-only on every target.
    bool        read_only = true;

    std::string listen_spec() const { return bind_addr + ":" + std::to_string(port); }
    std::string url() const { return "http://" + bind_addr + ":" + std::to_string(port) + "/"; }
};

} // namespace truetest::web
