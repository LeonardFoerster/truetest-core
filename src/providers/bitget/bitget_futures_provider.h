#pragma once
#ifdef HAS_BITGET

// Bitget UTA v3 USDT-M futures provider.
// Public market data + HybridExecutor / TradeTapeShadowAdapter for non-live.
// Live: REST clock/instruments/one-way gate + ExecutionBridge + reconciler
// + kill-switch (regular/strategy order readback + flat-position proof)
// + optional DMS.

#include "engine/engine_config.h"
#include "execution/execution_bridge.h"
#include "execution/rate_limiter.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "orderbook/orderbook.h"
#include "core/event.h"
#include "providers/bitget/bitget_backfill.h"
#include "providers/bitget/bitget_combined_transport.h"
#include "providers/bitget/bitget_endpoints.h"
#include "providers/bitget/bitget_futures_bracket_adapter.h"
#include "providers/bitget/bitget_futures_dead_mans_switch.h"
#include "providers/bitget/bitget_futures_kill_switch.h"
#include "providers/bitget/bitget_futures_order_encoder.h"
#include "providers/bitget/bitget_futures_reconciler.h"
#include "providers/bitget/bitget_futures_safety.h"
#include "providers/bitget/bitget_futures_user_data_parser.h"
#include "providers/bitget/bitget_hybrid_executor.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_private_ws_transport.h"
#include "providers/bitget/bitget_rest_client.h"
#include "providers/bitget/bitget_rest_order_transport.h"
#include "providers/bitget/bitget_time_sync.h"
#include "providers/bitget/bitget_transport.h"
#include "providers/prepend_transport.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"
#include "risk/maintenance_margin_table.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bitget {

// Short clientOid minter: Bitget caps at 32 chars and charset
// ^[.A-Z:/a-z0-9_-]{1,32}$. Stock ClientOrderIdMinter embeds full epoch+seed
// hex and routinely exceeds 32.
class ShortClientOidMinter
{
public:
    explicit ShortClientOidMinter(std::uint64_t seed,
                                  std::int64_t epoch_ms = now_epoch_ms())
        : seq_(0)
    {
        // "tt" + 8 hex (mixed epoch) + 4 hex (seed low) = 14-char prefix.
        // Counter as up to 8 hex → total ≤ 22 << 32.
        const auto mix = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(epoch_ms)
             ^ (static_cast<std::uint64_t>(epoch_ms) >> 32))
            & 0xffffffffu);
        const auto s16 = static_cast<std::uint32_t>(seed & 0xffffu);
        char buf[20];
        std::snprintf(buf, sizeof(buf), "tt%08x%04x", mix, s16);
        prefix_ = buf;
    }

    std::string next()
    {
        const std::uint64_t n = ++seq_;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s%06llx",
                      prefix_.c_str(),
                      static_cast<unsigned long long>(n));
        std::string id(buf);
        if (id.size() > 32)
            id.resize(32);
        return id;
    }

    const std::string& prefix() const { return prefix_; }

private:
    static std::int64_t now_epoch_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string prefix_;
    std::atomic<std::uint64_t> seq_;
};

// Pure hold-mode gate (unit-testable). Returns empty on one-way; error note
// on hedge / missing / bad envelope.
inline std::string check_one_way_hold_mode(std::string_view body)
{
    if (!is_business_success(200, body))
    {
        auto code = extract_business_code(body);
        std::string note = "account/settings business code ";
        note.append(code.empty() ? "<missing>" : code);
        return note;
    }
    std::string_view data;
    std::string_view hold;
    if (!provider_recovery::top_level_member(body, "data", data)
        || !provider_recovery::is_authoritative_object(data)
        || !provider_recovery::top_level_plain_string(
            data, "holdMode", hold)
        || hold.empty())
        return "account/settings missing holdMode";
    if (hold == "hedge_mode" || hold == "hedge" || hold == "dual_long_short_mode")
        return "account is in hedge mode (holdMode=" + std::string(hold)
             + "). Switch to one_way_mode in the Bitget UI.";
    if (hold == "one_way_mode" || hold == "oneway" || hold == "one_way"
        || hold == "net_mode")
        return {};
    return "account/settings unknown holdMode '" + std::string(hold) + "'";
}

// Canonicalize margin mode for comparison (crossed/cross → CROSSED, etc.).
inline std::string canonical_margin_mode(std::string_view mode)
{
    if (mode.empty()) return {};
    std::string lower;
    lower.reserve(mode.size());
    for (unsigned char c : mode)
        lower.push_back(static_cast<char>(std::tolower(c)));
    if (lower == "crossed" || lower == "cross" || lower == "c")
        return "CROSSED";
    if (lower == "isolated" || lower == "isola" || lower == "i")
        return "ISOLATED";
    return {};
}

// Extract marginMode for symbol from account/settings body
// (data.symbolConfigList[]). Empty if not found.
inline std::string extract_symbol_margin_mode(std::string_view body,
                                              std::string_view want_symbol)
{
    if (!is_business_success(200, body)) return {};
    std::string_view data;
    std::string_view arr;
    if (!provider_recovery::top_level_member(body, "data", data)
        || !provider_recovery::is_authoritative_object(data)
        || !provider_recovery::top_level_member(
            data, "symbolConfigList", arr)
        || !provider_recovery::is_authoritative_object_array(arr))
        return {};
    std::string found;
    bool duplicate_match = false;
    const bool rows_ok = provider_recovery::every_top_level_object(
        arr, [&](std::string_view obj) {
            std::string_view sym;
            if (!provider_recovery::top_level_plain_string(
                    obj, "symbol", sym))
                return false;

            std::string_view margin;
            const auto margin_state = provider_recovery::payload_parser(obj)
                .inspect_top_level_member("marginMode", margin);
            if (margin_state
                == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                return false;
            if (margin_state
                == provider_recovery::payload_parser::member_result::unique)
            {
                if (!provider_recovery::top_level_plain_string(
                        obj, "marginMode", margin))
                    return false;
            }
            else if (!provider_recovery::top_level_plain_string(
                         obj, "marginType", margin))
                return false;

            if (sym != want_symbol) return true;
            if (!found.empty())
            {
                duplicate_match = true;
                return false;
            }
            found.assign(margin);
            return true;
        });
    return rows_ok && !duplicate_match ? found : std::string{};
}

// Strict margin gate. Empty = pass. Non-empty = refuse reason.
// Fail-closed: missing symbol config or mismatch refuses.
inline std::string check_margin_type_strict(std::string_view settings_body,
                                            std::string_view symbol,
                                            std::string_view expected_margin)
{
    if (expected_margin.empty()) return {};
    const std::string want = canonical_margin_mode(expected_margin);
    if (want.empty())
    {
        return "margin-type-strict: unsupported expected margin mode '"
             + std::string(expected_margin) + "'";
    }

    const std::string raw = extract_symbol_margin_mode(settings_body, symbol);
    if (raw.empty())
    {
        return "margin-type-strict: symbol '" + std::string(symbol)
             + "' marginMode not found in account/settings "
               "symbolConfigList (cannot enforce expected="
             + want + ")";
    }
    const std::string got = canonical_margin_mode(raw);
    if (got.empty())
    {
        return "margin-type-strict: symbol '" + std::string(symbol)
             + "' returned unsupported venue marginMode='" + raw + "'";
    }
    if (got != want)
    {
        return "margin-type-strict: symbol '" + std::string(symbol)
             + "' marginMode=" + got + " (venue raw='" + raw
             + "') does not match expected " + want;
    }
    return {};
}

} // namespace bitget

class BitgetFuturesProvider : public IProvider
{
public:
    BitgetFuturesProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& api_passphrase = "",
        const std::string& host = "ws.bitget.com",
        const std::string& port = "443")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , api_passphrase_(api_passphrase)
        , host_(host)
        , port_(port)
        , endpoints_(bitget::uta_mainnet())
    {
        if (!host.empty()) endpoints_.ws_public_host = host;
        if (!port.empty()) endpoints_.ws_port = port;
    }

    void set_depth_stream(const std::string& depth_stream)
    {
        depth_stream_ = depth_stream;
    }

    const std::string& depth_stream() const { return depth_stream_; }

    void set_category(std::string category)
    {
        category_ = std::move(category);
    }

    void set_api_surface(std::string surface)
    {
        api_surface_ = std::move(surface);
    }

    // Operator-set margin/advisory inputs. margin_type_strict is enforced
    // at live open; liquidation_warn_pct drives position-risk advisories.
    void set_expected_margin_type(std::string mt)
    {
        expected_margin_type_ = std::move(mt);
    }
    void set_liquidation_warn_pct(double pct)
    {
        liquidation_warn_pct_ = pct;
    }
    void set_margin_type_strict(bool strict)
    {
        margin_type_strict_ = strict;
    }

    // Position-based pre-trade risk caps. 0 disables each; all-zero means
    // no FuturesRiskCheck is constructed.
    void set_max_notional_usdt(double v)             { rc_cfg_.max_notional_usdt = v; }
    void set_max_leverage(double v)                  { rc_cfg_.max_leverage = v; }
    void set_min_liquidation_distance_pct(double v)  { rc_cfg_.min_liquidation_distance_pct = v; }
    void set_maintenance_margin_pct(double v)        { rc_cfg_.maintenance_margin_pct = v; }

    // DMS knobs: countdown_ms > 0 arms UTA countdown-cancel-all on live open.
    void set_dead_man_countdown_ms(int64_t v)        { dead_man_countdown_ms_ = v; }
    void set_dead_man_heartbeat_ms(int64_t v)        { dead_man_heartbeat_ms_ = v; }

    void set_endpoints(bitget::endpoints ep)
    {
        endpoints_ = std::move(ep);
        host_ = endpoints_.ws_public_host;
        port_ = endpoints_.ws_port;
    }

    bool is_demo() const { return endpoints_.is_demo; }

    std::string name() const override { return "bitget-futures"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    lifecycle lifecycle_state() const override { return state_; }

    void configure(const engine_config& cfg) override
    {
        mode_ = cfg.mode;
        fee_model_ = cfg.fee_model;
        fill_model_ = cfg.fill_model;
        wire_latency_model_ = cfg.wire_latency_model;
        queue_model_ = cfg.queue_position_model;
        maker_queue_model_ = cfg.maker_queue_model;
        qty_scale_ = cfg.qty_scale;
        spread_step_factor_ = cfg.spread_step_factor;
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        if (paper_exec_)
            paper_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        if (mode_ == engine_mode::live &&
            (api_key_.empty() || api_secret_.empty() || api_passphrase_.empty()))
        {
            std::cerr << "BitgetFuturesProvider: refusing live open — API key, "
                         "secret, and passphrase are all required.\n";
            state_ = lifecycle::error;
            return false;
        }
        if (!std::isfinite(rc_cfg_.min_liquidation_distance_pct) ||
            rc_cfg_.min_liquidation_distance_pct < 0.0 ||
            rc_cfg_.min_liquidation_distance_pct > 1.0)
        {
            std::cerr << "BitgetFuturesProvider: refusing open — minimum "
                         "liquidation distance must be a fraction in [0,1].\n";
            state_ = lifecycle::error;
            return false;
        }
        if (dead_man_countdown_ms_ < 0 || dead_man_heartbeat_ms_ < 0
            || (dead_man_countdown_ms_ == 0 && dead_man_heartbeat_ms_ > 0)
            || (dead_man_countdown_ms_ > 0 && dead_man_heartbeat_ms_ > 0
                && dead_man_heartbeat_ms_ >= dead_man_countdown_ms_))
        {
            std::cerr << "BitgetFuturesProvider: refusing open — invalid "
                         "dead-man countdown/heartbeat relationship.\n";
            state_ = lifecycle::error;
            return false;
        }

        // Classic mix/v2 remains unsupported (no countdown DMS; dual surface
        // is documented only). Only empty/"uta" allowed.
        if (!api_surface_is_uta(api_surface_))
        {
            std::cerr << "BitgetFuturesProvider: refusing open — "
                         "api_surface='" << api_surface_
                      << "' is not implemented (only empty/'uta'). "
                         "Classic mix/v2 is out of scope for this provider.\n";
            state_ = lifecycle::error;
            return false;
        }

        // Risk check before mode dispatch — applies in shadow/paper/live.
        if (rc_cfg_.max_notional_usdt > 0.0
            || rc_cfg_.max_leverage > 0.0
            || rc_cfg_.min_liquidation_distance_pct > 0.0)
        {
            risk_check_ = std::make_shared<FuturesRiskCheck>(rc_cfg_, mm_table_);
        }

        std::cerr << "  BitgetFuturesProvider: "
                  << (endpoints_.is_demo ? "[DEMO] " : "")
                  << "ws=" << endpoints_.ws_public_host << ":" << endpoints_.ws_port
                  << endpoints_.ws_public_path
                  << " rest=" << endpoints_.rest_host << ":" << endpoints_.rest_port
                  << " category=" << category_
                  << "\n";

        std::cerr << "  BitgetFuturesProvider: creating transport (stream="
                  << stream_type_
                  << ", depth=" << (depth_stream_.empty() ? "none" : depth_stream_)
                  << ")\n";

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_stream_.empty())
        {
            bitget_transport_ = std::make_shared<BitgetTransport>(
                symbol_, stream_type_, endpoints_);
            live_transport = bitget_transport_;
        }
        else
        {
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(stream_type_);
            streams.push_back(depth_stream_);
            bitget_combined_transport_ = std::make_shared<BitgetCombinedTransport>(
                symbol_, streams, endpoints_);
            live_transport = bitget_combined_transport_;
        }

        apply_halt_cb_to_transports();

        // REST candle backfill for kline streams → PrependTransport.
        std::vector<std::string> prepend;
        if (backfill_bars_ > 0 && is_kline_stream())
        {
            std::string interval = backfill_interval_;
            if (interval.empty())
                interval = kline_interval_from_stream();
            if (interval.empty())
                interval = "1m";
            interval = bitget::normalize_kline_interval(interval);

            const std::string rest_host = !backfill_host_override_.empty()
                ? backfill_host_override_
                : endpoints_.rest_host;

            bitget::BitgetBackfill backfiller(
                rest_host, endpoints_.rest_port, category_);
            std::cerr << "  BitgetFuturesProvider: backfilling "
                      << backfill_bars_ << " bars for " << symbol_
                      << " (" << interval << ") via " << rest_host << "...\n";
            auto bars = backfiller.fetch(
                upper(symbol_), interval, backfill_bars_);
            std::cerr << "  BitgetFuturesProvider: backfill loaded "
                      << bars.size() << " bars\n";
            prepend = bitget::BitgetBackfill::to_prepend_frames(
                bars, upper(symbol_), interval);
        }

        if (!prepend.empty())
            transport_ = std::make_shared<PrependTransport>(
                live_transport, std::move(prepend));
        else
            transport_ = live_transport;

        paper_exec_ = std::make_shared<BitgetPaperExecutor>();
        paper_exec_->set_symbol(symbol_);
        if (auto dash = dashboard_.lock())
            paper_exec_->set_dashboard(dashboard_);
        if (fee_model_)
            paper_exec_->set_fee_model(fee_model_);

        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            if (!open_live_path())
            {
                // Partial live setup is already owned by LiveSafetySession.
                // Stop new mutations here; the session performs the single
                // kill-before-finish sequence and decides whether DMS may be
                // disarmed from the kill result.
                quiesce_for_live_shutdown();
                state_ = lifecycle::error;
                return false;
            }
        }
        else if (mode_ == engine_mode::shadow)
        {
            shadow_exec_ = std::make_shared<TradeTapeShadowAdapter>(
                wire_latency_model_, fee_model_);
            if (queue_model_)
                shadow_exec_->set_queue_model(queue_model_);
            executor_ = shadow_exec_;
        }
        else
        {
            // paper / backtest / live-without-keys
            auto book = std::make_shared<orderbook>();
            hybrid_exec_ = std::make_shared<BitgetHybridExecutor>(
                paper_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_, wire_latency_model_,
                maker_queue_model_);
            executor_ = hybrid_exec_;
        }

        if (!live_transport->open())
        {
            // Public WS failed after live setup (DMS may already be armed).
            // Quiesce locally; LiveSafetySession retains responsibility for
            // kill, transport finish, and preserve-vs-disarm disposition.
            quiesce_for_live_shutdown();
            state_ = lifecycle::error;
            return false;
        }

        state_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        quiesce_for_live_shutdown();
        // Public close has no proof that flatten succeeded. Preserve the
        // venue countdown; only LiveSafetySession may pass disarm_after_kill.
        finish_live_shutdown(live_shutdown_disposition::preserve_dead_man_switch);
    }

    void quiesce_for_live_shutdown() override
    {
        quiesce_futures_live_resources(
            live_mutations_cancelled_, dms_, bridge_, bitget_private_ws_,
            transport_);
    }

    void finish_live_shutdown(live_shutdown_disposition disposition) override
    {
        const bool disarm_succeeded = finish_futures_live_resources(
            dms_, bridge_, bitget_private_ws_, transport_, disposition);
        state_ = lifecycle::closed;
        if (!disarm_succeeded)
        {
            std::cerr << "BitgetFuturesProvider: shutdown finish or "
                         "dead-man's-switch disarm failed; relying on the "
                         "countdown where available.\n";
            throw std::runtime_error(
                "BitgetFuturesProvider: live shutdown finish failed");
        }
        if (dms_ && disposition ==
                        live_shutdown_disposition::preserve_dead_man_switch)
        {
            std::cerr << "BitgetFuturesProvider: kill failed or was ambiguous; "
                         "leaving dead-man's-switch countdown armed.\n";
        }
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return executor_;
    }

    void on_mid_price(const std::string& /*symbol*/, double mid_price) override
    {
        if (hybrid_exec_)
            hybrid_exec_->on_mid_price(mid_price);
        else if (paper_exec_)
            paper_exec_->set_last_price(mid_price);
    }

    std::shared_ptr<IRiskCheck> get_risk_check() override { return risk_check_; }

    std::shared_ptr<IReconciler> get_reconciler() override { return reconciler_; }

    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_switch_; }

    std::shared_ptr<truetest::exits::IBracketAdapter>
    get_bracket_adapter() override
    {
        return bracket_adapter_;
    }

    std::vector<liveness_source> get_liveness_sources() override
    {
        std::vector<liveness_source> out;
        if (dms_)
        {
            // last_alive_ms points into dms_; WorkerWatchdog must stop
            // before provider close/destruction (engine shutdown order).
            // Deadline = 3 × heartbeat: tolerate one missed cycle before
            // halt. Matches Binance futures DMS wiring.
            liveness_source s;
            s.name = "bitget-futures-dms-heartbeat";
            s.last_alive_ms = &dms_->liveness_ts();
            s.deadline_ms = dms_->heartbeat_interval_ms() * 3;
            out.push_back(std::move(s));
        }
        return out;
    }

    std::optional<instrument_spec>
    get_instrument(const std::string& symbol) const override
    {
        if (!instrument_spec_) return std::nullopt;
        if (!symbol.empty() && upper(symbol) != upper(instrument_spec_->symbol)
            && upper(symbol) != upper(symbol_))
            return std::nullopt;
        return *instrument_spec_;
    }

    void set_halt_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        halt_cb_.store(std::move(cb));
        apply_halt_cb_to_transports();
    }

    ProviderFundingIngress* funding_ingress() noexcept override
    {
        return mode_ == engine_mode::live ? &funding_ingress_ : nullptr;
    }

    bool supports_event_stream() const override
    {
        return !depth_stream_.empty();
    }

    // BitgetCombinedParser::parse_records emits every publicTrade data[] tick.
    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        if (depth_stream_.empty()) return nullptr;
        return std::make_shared<BitgetCombinedParser>();
    }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }
    const std::string& category() const { return category_; }
    const std::string& api_passphrase() const { return api_passphrase_; }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;
    std::string host_;
    std::string port_;
    bitget::endpoints endpoints_;
    std::string category_ = "USDT-FUTURES";
    std::string api_surface_ = "uta";

    bool configured_ = false;
    engine_mode mode_ = engine_mode::backtest;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::shared_ptr<ILatencyModel> wire_latency_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    std::shared_ptr<IQueueModel> maker_queue_model_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;

    lifecycle state_ = lifecycle::closed;

    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<BitgetTransport> bitget_transport_;
    std::shared_ptr<BitgetCombinedTransport> bitget_combined_transport_;

    std::shared_ptr<BitgetPaperExecutor> paper_exec_;
    std::shared_ptr<BitgetHybridExecutor> hybrid_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::shared_ptr<ExecutionBridge> bridge_;
    std::shared_ptr<std::atomic<bool>> live_mutations_cancelled_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<IExecutionAdapter> executor_;

    // Live-only
    std::shared_ptr<BitgetRestClient> rest_;
    std::shared_ptr<bitget::ShortClientOidMinter> minter_;
    std::shared_ptr<TokenBucketRateLimiter> order_rate_limiter_;
    std::shared_ptr<BitgetFuturesReconciler> reconciler_;
    std::shared_ptr<BitgetFuturesKillSwitch> kill_switch_;
    std::shared_ptr<BitgetFuturesDeadMansSwitch> dms_;
    std::shared_ptr<BitgetFuturesBracketAdapter> bracket_adapter_;
    std::shared_ptr<BitgetPrivateWsTransport> bitget_private_ws_;
    std::optional<instrument_spec> instrument_spec_;

    std::uint64_t seed_ = 0;
    std::string depth_stream_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    std::string expected_margin_type_;
    double      liquidation_warn_pct_ = 0.05;
    bool        margin_type_strict_   = false;

    FuturesRiskCheck::config rc_cfg_{};
    std::shared_ptr<IRiskCheck> risk_check_;
    std::shared_ptr<truetest::risk::MaintenanceMarginTable> mm_table_;

    int64_t dead_man_countdown_ms_ = 0;
    int64_t dead_man_heartbeat_ms_ = 0;

    int backfill_bars_ = 0;
    std::string backfill_interval_;
    std::string backfill_host_override_;

    ThreadSafeCallback<void(std::string_view)> halt_cb_;
    ProviderFundingIngress funding_ingress_;

    // Live refuse checklist (plan Phase 2 / Task 9).
    // Returns false → caller sets state=error.
    bool open_live_path()
    {
        if (api_secret_.empty() || api_passphrase_.empty())
        {
            std::cerr << "BitgetFuturesProvider: refusing to go live — "
                         "api_secret and api_passphrase are required\n";
            return false;
        }

        rest_ = std::make_shared<BitgetRestClient>(
            api_key_, api_secret_, api_passphrase_,
            endpoints_.rest_host, endpoints_.rest_port,
            "/api/v2/public/time",
            /*paptrading=*/endpoints_.is_demo);

        // Bound shared REST I/O so DMS/kill cannot stall forever behind one hung call.
        // Kill-switch may tighten further per-call; this is the live default floor.
        rest_->set_per_call_timeout(std::chrono::milliseconds(3000));

        if (!rest_->resync_clock_now())
        {
            std::cerr << "BitgetFuturesProvider: refusing to go live — "
                         "clock resync failed\n";
            return false;
        }

        auto check = bitget::verify_clock_skew(*rest_);
        if (!check.ok)
        {
            std::cerr << "BitgetFuturesProvider: refusing to go live — "
                      << check.note << "\n";
            return false;
        }

        // Instruments probe — refuse if not ok OR tick/lot <= 0.
        {
            const std::string sym = upper(symbol_);
            auto info = rest_->get_unsigned(
                "/api/v3/market/instruments",
                bitget::instruments_query(category_, sym));
            if (info.status < 200 || info.status >= 300)
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                             "instruments HTTP " << info.status << ": "
                          << bitget::truncate_for_log(info.body) << "\n";
                return false;
            }
            auto probe = bitget::parse_instruments_response(info.body, sym);
            if (!probe.ok)
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                          << probe.note << "\n";
                return false;
            }
            if (probe.spec.tick_size <= 0.0 || probe.spec.lot_size <= 0.0)
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                             "instrument tick_size/lot_size must be > 0 "
                             "(tick=" << probe.spec.tick_size
                          << " lot=" << probe.spec.lot_size << ")\n";
                return false;
            }
            instrument_spec_ = probe.spec;
            if (instrument_spec_->symbol.empty())
                instrument_spec_->symbol = sym;
        }

        // One-way mode gate via account settings.
        {
            auto resp = rest_->get("/api/v3/account/settings", "");
            if (resp.status < 200 || resp.status >= 300)
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                             "/api/v3/account/settings HTTP " << resp.status
                          << ": " << bitget::truncate_for_log(resp.body)
                          << "\n";
                return false;
            }
            if (!resp.business_ok
                && !bitget::is_business_success(resp.status, resp.body))
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                             "/api/v3/account/settings business failure: "
                          << bitget::truncate_for_log(resp.body) << "\n";
                return false;
            }
            auto hold_err = bitget::check_one_way_hold_mode(resp.body);
            if (!hold_err.empty())
            {
                std::cerr << "BitgetFuturesProvider: refusing to go live — "
                          << hold_err << "\n";
                return false;
            }

            // Strict margin gate (fail-closed) via same settings body.
            // Missing symbolConfigList entry or mismatch → refuse open.
            if (margin_type_strict_ && !expected_margin_type_.empty())
            {
                auto mm_err = bitget::check_margin_type_strict(
                    resp.body, upper(symbol_), expected_margin_type_);
                if (!mm_err.empty())
                {
                    std::cerr << "BitgetFuturesProvider: refusing to go live — "
                              << mm_err << "\n";
                    return false;
                }
            }
        }

        // Phase 4: position advisories (margin mismatch / liq distance).
        // Non-strict path is warning-only; settings-based strict margin already
        // ran above. Flat maintenance_margin_pct remains the MM model unless
        // operator sets FuturesRiskCheck::maintenance_margin_pct (no UTA
        // leverageBracket endpoint equivalent to Binance fapi).
        {
            const std::string pos_q =
                "category=" + category_ + "&symbol=" + upper(symbol_);
            auto pr = rest_->get("/api/v3/position/current-position", pos_q);
            if (pr.status >= 200 && pr.status < 300
                && (pr.business_ok
                    || bitget::is_business_success(pr.status, pr.body)))
            {
                auto advisories = bitget::futures::compute_advisories(
                    pr.body, upper(symbol_), expected_margin_type_,
                    liquidation_warn_pct_);
                for (const auto& a : advisories)
                    std::cerr << "  [ADVISORY] " << a.note << "\n";
            }
            else
            {
                std::cerr << "BitgetFuturesProvider: position advisory probe "
                             "HTTP " << pr.status
                          << " — skipping margin/liq advisories "
                             "(startup refuses; operator reconciliation required)\n";
            }
        }

        minter_ = std::make_shared<bitget::ShortClientOidMinter>(seed_);

        // Conservative place-order rate: capacity 10, refill 5/s (~5–10/s).
        order_rate_limiter_ = std::make_shared<TokenBucketRateLimiter>(
            /*capacity=*/10.0, /*refill_per_sec=*/5.0);

        reconciler_ = std::make_shared<BitgetFuturesReconciler>(
            rest_, upper(symbol_), category_, endpoints_.is_demo);

        kill_switch_ = make_bitget_futures_kill_switch(
            rest_, category_, upper(symbol_));

        live_mutations_cancelled_->store(false, std::memory_order_release);
        bracket_adapter_ = make_bitget_futures_bracket_adapter(
            rest_, category_, live_mutations_cancelled_, upper(symbol_));

        ExecutionBridge::deps d;
        d.order_tx = make_bitget_rest_order_transport(
            rest_, live_mutations_cancelled_);
        bitget_private_ws_ = std::make_shared<BitgetPrivateWsTransport>(
            api_key_, api_secret_, api_passphrase_, endpoints_);
        bitget_private_ws_->set_time_offset_ms(rest_->clock_offset_ms());
        d.fill_tx = bitget_private_ws_;
        apply_halt_cb_to_transports();

        auto encoder = std::make_shared<BitgetFuturesOrderEncoder>(
            symbol_, category_);
        // Wire operator margin expectation into place-order body (limit).
        // Venue default is crossed; isolated accounts must send "isolated".
        if (!expected_margin_type_.empty())
        {
            const std::string canon =
                bitget::canonical_margin_mode(expected_margin_type_);
            if (canon == "ISOLATED")
                encoder->set_margin_mode("isolated");
            else if (canon == "CROSSED")
                encoder->set_margin_mode("crossed");
        }
        d.encoder = std::move(encoder);
        d.parser = std::make_shared<BitgetFuturesUserDataParser>();
        d.order_rate_limiter = order_rate_limiter_;
        d.client_id_fn = [m = minter_](uint64_t) { return m->next(); };

        const auto funding_symbol = upper(symbol_);
        d.funding_update_handler =
            [this, sym = funding_symbol](
                const parsed_funding_update& update) noexcept {
                return funding_ingress_.try_publish(
                    std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(update.event_time_ms)),
                    sym, update.cash_delta);
            };
        d.funding_failure_handler = [this]() noexcept {
            funding_ingress_.latch_failure();
            fail_funding_ingress();
        };

        // Non-funding position/account snapshots remain diagnostic only.
        d.position_snapshot_handler =
            [this, sym = funding_symbol](const parsed_position_snapshot& s) {
                log_position_snapshot(s, sym);
            };

        bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
        if (!bridge_->open())
        {
            std::cerr << "BitgetFuturesProvider: ExecutionBridge open "
                         "failed: " << bridge_->last_error() << "\n";
            return false;
        }
        executor_ = bridge_;

        // Dead-man's switch — last to arm, first to disarm. Venue countdown
        // brackets live order routing so a crash after bridge open is still
        // protected. Account-wide cancel caveat is logged inside DMS ctor.
        if (dead_man_countdown_ms_ > 0)
        {
            // Pass operator HB as-is (0 = DMS-internal default countdown/3).
            // DMS ctor owns floor (≥1000) and clamp vs countdown so large
            // Binance-style ms values cannot leave HB ≥ venue timer.
            const int64_t hb = dead_man_heartbeat_ms_;

            dms_ = make_bitget_futures_dead_mans_switch(
                rest_, dead_man_countdown_ms_, hb);
            if (!dms_->start())
            {
                std::cerr << "BitgetFuturesProvider: dead-man's switch "
                             "failed to arm — refusing to go live.\n";
                bridge_->close();
                return false;
            }
            std::cerr << "  BitgetFuturesProvider: dead-man's switch armed "
                         "(countdown=" << dms_->countdown_sec()
                      << "s, heartbeat=" << dms_->heartbeat_interval_ms()
                      << "ms)\n";
        }

        std::cerr << "  BitgetFuturesProvider: live path open "
                     "(kill-switch ready"
                  << (dms_ ? ", DMS armed" : ", DMS off") << ")\n";
        return true;
    }

    // Wire engine halt into transports when present. Matches Binance:
    // leave fatal_cb unset until set_halt_callback so public/private WS may
    // reconnect during the open→engine-wire window. A log-only provisional
    // would disable reconnect without actually halting (fill-blind live).
    // Paper/shadow: same — no fatal until engine wires halt.
    void apply_halt_cb_to_transports()
    {
        auto halt = halt_cb_.load();
        if (!halt)
            return;
        if (bitget_transport_)
            bitget_transport_->set_fatal_disconnect_callback(*halt);
        if (bitget_combined_transport_)
            bitget_combined_transport_->set_fatal_disconnect_callback(*halt);
        if (bitget_private_ws_)
            bitget_private_ws_->set_fatal_disconnect_callback(*halt);
        if (dms_)
        {
            wire_dms_failure_to_engine(dms_, *halt, transport_);
        }
    }

    void fail_funding_ingress() noexcept
    {
        if (auto halt = halt_cb_.load())
        {
            try { (*halt)("bitget funding ingress overflow or malformed update"); }
            catch (...) {}
        }
        if (transport_)
        {
            try { transport_->request_stop(); }
            catch (...) {}
        }
    }

    // Empty or "uta" (any case) → allowed. Everything else → refuse.
    static bool api_surface_is_uta(std::string_view surface)
    {
        if (surface.empty()) return true;
        if (surface.size() != 3) return false;
        return (surface[0] == 'u' || surface[0] == 'U')
            && (surface[1] == 't' || surface[1] == 'T')
            && (surface[2] == 'a' || surface[2] == 'A');
    }

    bool is_kline_stream() const
    {
        return stream_type_.rfind("kline", 0) == 0
            || stream_type_.rfind("candle", 0) == 0;
    }

    // CLI stream kline1m / candle4h → interval suffix (before normalize).
    std::string kline_interval_from_stream() const
    {
        if (stream_type_.rfind("kline", 0) == 0)
            return stream_type_.substr(5);
        if (stream_type_.rfind("candle", 0) == 0)
            return stream_type_.substr(6);
        return {};
    }

    static std::string upper(const std::string& s)
    {
        std::string out(s);
        for (auto& c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    static const char* reason_str(parsed_position_snapshot::reason r)
    {
        using R = parsed_position_snapshot::reason;
        switch (r)
        {
        case R::order:              return "ORDER";
        case R::funding_fee:        return "FUNDING_FEE";
        case R::adjustment:         return "ADJUSTMENT";
        case R::deposit:            return "DEPOSIT";
        case R::withdraw:           return "WITHDRAW";
        case R::margin_transfer:    return "MARGIN_TRANSFER";
        case R::margin_type_change: return "MARGIN_TYPE_CHANGE";
        case R::liquidation:        return "LIQUIDATION";
        case R::admin:              return "ADMIN";
        case R::unknown:            return "UNKNOWN";
        case R::other:              return "OTHER";
        }
        return "UNKNOWN";
    }

    static void log_position_snapshot(const parsed_position_snapshot& s,
                                      const std::string& provider_symbol)
    {
        for (const auto& p : s.positions)
        {
            if (p.symbol != provider_symbol) continue;
            if (s.r == parsed_position_snapshot::reason::order)
                continue;
            std::fprintf(stderr,
                "  [POSITION-SNAPSHOT] %s reason=%s qty=%.8f margin=%s side=%s\n",
                p.symbol.c_str(), reason_str(s.r), p.qty,
                p.margin_type.c_str(), p.position_side.c_str());
        }
        for (const auto& b : s.balances)
        {
            if (s.r == parsed_position_snapshot::reason::order) continue;
            std::fprintf(stderr,
                "  [POSITION-SNAPSHOT] %s reason=%s balance=%.8f delta=%.8f\n",
                b.asset.c_str(), reason_str(s.r),
                b.wallet_balance, b.balance_change);
        }
    }
};

#endif // HAS_BITGET
