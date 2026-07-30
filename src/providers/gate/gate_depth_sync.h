#pragma once
#ifdef HAS_GATE

// REST order_book snapshot (with_id) + U/u gap recovery helpers.
// Pure / unit-testable. Network fetch is optional cold-path only.

#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"
#include "providers/provider_event.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gate {

// ── Gap / sequence policy (Gate futures order_book_update docs) ───────────
//
// 1. Subscribe + buffer updates
// 2. REST order_book?with_id=true → base id
// 3. Apply first update where U <= base_id+1 <= u
// 4. Thereafter sequential; gap → resync
// 5. s=="0" → delete level (handled in parser as qty 0)
// 6. full:true → replace local book

enum class depth_action
{
    apply,   // apply this update
    drop,    // stale / already applied
    resync,  // gap detected — re-fetch REST snapshot
    buffer,  // waiting for first bridging update
};

struct depth_sync_state
{
    int64_t base_id = 0;   // REST snapshot id
    int64_t last_u = 0;    // last applied update u
    bool    synced = false;
};

// Classify an update against current sync state.
// When !synced: apply iff U <= base_id+1 <= u (bridge), else buffer.
// When synced: apply if U == last_u+1 (or U <= last_u+1 && u > last_u overlap);
//              drop if u <= last_u; resync if U > last_u+1.
inline depth_action classify_book_update(const depth_sync_state& st,
                                         int64_t U,
                                         int64_t u,
                                         bool full = false)
{
    if (full)
        return depth_action::apply;

    if (!st.synced)
    {
        // Need REST base first.
        if (st.base_id <= 0)
            return depth_action::buffer;
        // Bridge: U <= base_id+1 <= u
        const int64_t target = st.base_id + 1;
        if (U <= target && target <= u)
            return depth_action::apply;
        // Update entirely before base — drop.
        if (u <= st.base_id)
            return depth_action::drop;
        // Update after base without bridging — still buffer (wait).
        return depth_action::buffer;
    }

    if (u <= st.last_u)
        return depth_action::drop;
    // Contiguous or overlapping forward.
    if (U <= st.last_u + 1)
        return depth_action::apply;
    // Gap.
    return depth_action::resync;
}

// Advance state after a successful apply.
inline void apply_book_update(depth_sync_state& st, int64_t /*U*/, int64_t u,
                              bool /*full*/ = false)
{
    st.last_u = u;
    st.synced = true;
}

inline void reset_depth_sync(depth_sync_state& st, int64_t base_id = 0)
{
    st.base_id = base_id;
    st.last_u = 0;
    st.synced = false;
}

// ── REST snapshot parse ───────────────────────────────────────────────────

struct order_book_snapshot
{
    int64_t id = 0;
    provider::l2_snapshot book;
    bool ok = false;
};

// Parse GET /futures/{settle}/order_book response body.
// Shape: {"id":N,"current":...,"update":...,"asks":[[px,sz],...],"bids":[...]}
inline order_book_snapshot parse_rest_order_book(std::string_view body,
                                                 std::string_view symbol)
{
    order_book_snapshot snap;
    auto id_sv = extract_sv_number(body, "id");
    if (id_sv.empty() || !parse_int64_sv(id_sv, snap.id))
        return snap;

    snap.book.symbol.assign(symbol.data(), symbol.size());
    snap.book.timestamp = std::chrono::system_clock::now();

    auto cur = extract_sv_number(body, "current");
    // current may be fractional seconds — ignore for ts if not integer ms.
    int64_t cur_i = 0;
    if (!cur.empty() && parse_int64_sv(cur, cur_i) && cur_i > 1'000'000'000LL)
    {
        if (cur_i > 1'000'000'000'000LL)
            snap.book.timestamp = tp_from_ms(cur_i);
        else
            snap.book.timestamp = tp_from_s(cur_i);
    }

    append_rest_levels(body, "bids", snap.book.bids);
    append_rest_levels(body, "asks", snap.book.asks);
    snap.ok = !snap.book.bids.empty() || !snap.book.asks.empty();
    return snap;
}

// Query string for with_id snapshot.
inline std::string order_book_query(std::string_view contract,
                                    int limit = 100)
{
    std::string q;
    q.reserve(64 + contract.size());
    q.append("contract=");
    q.append(contract);
    q.append("&limit=");
    q.append(std::to_string(std::max(1, std::min(limit, 100))));
    q.append("&with_id=true");
    return q;
}

// Cold-path REST fetch (unsigned). Returns empty on network/HTTP failure.
class GateDepthRest
{
public:
    GateDepthRest(std::string rest_host = "api.gateio.ws",
                  std::string rest_port = "443",
                  std::string rest_prefix = "/api/v4",
                  settle_ccy settle = settle_ccy::usdt)
        : rest_host_(std::move(rest_host))
        , rest_port_(std::move(rest_port))
        , rest_prefix_(std::move(rest_prefix))
        , settle_(settle)
    {
    }

    explicit GateDepthRest(const endpoints& ep)
        : GateDepthRest(ep.rest_host, ep.rest_port, ep.rest_prefix, ep.settle)
    {
    }

    order_book_snapshot fetch(const std::string& contract,
                              int limit = 100) const
    {
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        try
        {
            const std::string settle =
                settle_ == settle_ccy::usdt ? "usdt" : "btc";
            const std::string target =
                rest_prefix_ + "/futures/" + settle + "/order_book?"
                + order_book_query(contract, limit);

            net::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            ssl::stream<tcp::socket> stream(ioc, ctx);
            SSL_set_tlsext_host_name(stream.native_handle(),
                                     rest_host_.c_str());

            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(rest_host_, rest_port_);
            net::connect(stream.next_layer(), results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req(http::verb::get, target, 11);
            req.set(http::field::host, rest_host_);
            req.set(http::field::user_agent, "TrueTest/1.0");
            req.set("Accept", "application/json");

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            if (res.result() != http::status::ok)
                return {};
            return parse_rest_order_book(res.body(), contract);
        }
        catch (...)
        {
            return {};
        }
    }

private:
    std::string rest_host_;
    std::string rest_port_;
    std::string rest_prefix_;
    settle_ccy settle_;
};

} // namespace gate

#endif // HAS_GATE
