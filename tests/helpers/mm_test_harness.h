#pragma once

// Shared fixtures for the inventory-aware market-making strategy tests.
//
// Everything here is cold-path test scaffolding: JSON parsing, decimal-string
// conversion, and default configuration builders. None of it is linked into a
// shipping binary.

#include "providers/footprint/decimal_ticks.h"
#include "strategy/market_making/inventory_aware_mm_strategy.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace truetest::mm::test
{

// ── exact decimal-string conversion ─────────────────────────────────────────
// Reuses the repository's integer decimal parser so a fixture value never
// round-trips through a double.

inline std::int64_t pow10_i64(int e)
{
    std::int64_t v = 1;
    for (int i = 0; i < e; ++i)
        v *= 10;
    return v;
}

inline Price price_from_decimal(std::string_view s)
{
    const auto dec = truetest::footprint::parse_decimal(s);
    if (!dec)
        throw std::runtime_error("mm fixture: malformed decimal price '" + std::string(s) + "'");
    constexpr int price_decimals = 4; // Price::SCALE == 10000
    if (dec->scale > price_decimals)
    {
        const std::int64_t div = pow10_i64(dec->scale - price_decimals);
        if (dec->mantissa % div != 0)
            throw std::runtime_error("mm fixture: price '" + std::string(s)
                                     + "' is finer than the Price grid");
        return Price(dec->mantissa / div);
    }
    return Price(dec->mantissa * pow10_i64(price_decimals - dec->scale));
}

inline qty_atoms atoms_from_decimal(std::string_view s)
{
    const auto dec = truetest::footprint::parse_decimal(s);
    if (!dec)
        throw std::runtime_error("mm fixture: malformed decimal quantity '" + std::string(s) + "'");
    constexpr int qty_decimals = 8; // canonical atoms == 1e8
    if (dec->scale > qty_decimals)
    {
        const std::int64_t div = pow10_i64(dec->scale - qty_decimals);
        if (dec->mantissa % div != 0)
            throw std::runtime_error("mm fixture: quantity '" + std::string(s)
                                     + "' is finer than the atom grid");
        return dec->mantissa / div;
    }
    return dec->mantissa * pow10_i64(qty_decimals - dec->scale);
}

inline std::string decimal_from_price(Price p)
{
    const std::int64_t raw = p.raw();
    const bool neg = raw < 0;
    const std::int64_t a = neg ? -raw : raw;
    std::string frac = std::to_string(a % 10000);
    frac.insert(0, static_cast<std::size_t>(4 - frac.size()), '0');
    return (neg ? "-" : "") + std::to_string(a / 10000) + "." + frac;
}

inline std::string decimal_from_atoms(qty_atoms q)
{
    const bool neg = q < 0;
    const std::int64_t a = neg ? -q : q;
    std::string frac = std::to_string(a % 100000000);
    frac.insert(0, static_cast<std::size_t>(8 - frac.size()), '0');
    return (neg ? "-" : "") + std::to_string(a / 100000000) + "." + frac;
}

// ── defaults ────────────────────────────────────────────────────────────────

// BTCUSDT-shaped venue metadata: 0.10 tick, 0.0001 lot, 1 bp maker fee.
inline mm_instrument default_instrument()
{
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.tick_size = 0.10;
    spec.lot_size = 0.0001;
    spec.maker_rate = 0.0001; // 1.0 bp
    mm_instrument out{};
    const auto status = make_mm_instrument(spec, out);
    if (status != instrument_status::ok)
        throw std::runtime_error(std::string("mm fixture instrument: ") + describe(status));
    return out;
}

// Reference configuration used by the unit, property, and golden suites.
// Every number is explicit so a test can state what it expects from the
// config rather than from a library default that might drift.
inline mm_config default_config()
{
    mm_config cfg;
    cfg.strategy_id = "inventory_aware_mm_ref";
    cfg.levels = 1;

    cfg.fair_value.microprice_weight = 0.5;
    cfg.fair_value.imbalance_weight = 0.0;
    cfg.fair_value.short_flow_weight = 0.0;

    cfg.inventory.hard_limit_base = atoms_from_decimal("1.0");
    cfg.inventory.soft_limit_ratio = 0.50;
    cfg.inventory.reducing_bias_ratio = 0.80;
    cfg.inventory.reservation_skew_bps_at_hard_limit = 4.0;
    cfg.inventory.soft_limit_skew_boost = 0.0;
    cfg.inventory.size_skew_strength = 0.50;
    cfg.inventory.min_size_multiplier = 0.0;
    cfg.inventory.max_size_multiplier = 2.0;
    cfg.inventory.reducing_size_factor = 0.25;

    cfg.spread.min_half_spread_bps = 6.0;
    cfg.spread.max_half_spread_bps = 50.0;
    cfg.spread.fee_buffer_bps = 0.5;
    cfg.spread.maker_fee_multiplier = 1.0;
    cfg.spread.volatility_multiplier = 1.0;
    cfg.spread.toxicity_multiplier = 1.0;
    cfg.spread.latency_buffer_bps = 0.0;
    cfg.spread.latency_multiplier = 1.0;

    cfg.quotes.base_size = atoms_from_decimal("0.10");
    cfg.quotes.level_spacing_bps = 2.0;
    cfg.quotes.post_only = true;
    cfg.quotes.minimum_refresh_ticks = 0;
    cfg.quotes.minimum_quote_lifetime_ms = 0;

    cfg.safety.max_market_data_age_ms = 250;
    cfg.safety.pause_on_sequence_gap = true;
    cfg.safety.require_authoritative_inventory = true;
    return cfg;
}

inline constexpr timestamp_ns base_event_time_ns = 1'770'000'000'000'000'000LL;

inline market_snapshot default_market()
{
    market_snapshot m;
    m.event_time_ns = base_event_time_ns;
    m.receive_time_ns = base_event_time_ns + 100'000;
    m.best_bid = price_from_decimal("60000.00");
    m.best_ask = price_from_decimal("60000.50");
    m.best_bid_qty = atoms_from_decimal("4.20");
    m.best_ask_qty = atoms_from_decimal("1.40");
    m.short_horizon_volatility_bps = 0.30;
    m.toxicity_risk_bps = 0.10;
    m.latency_risk_bps = 0.10;
    m.short_flow_signal = 0.0;
    m.sequence_valid = true;
    m.snapshot_id = 1;
    return m;
}

inline inventory_snapshot flat_inventory()
{
    inventory_snapshot inv;
    inv.signed_base_position = 0;
    inv.hard_limit = 0; // config limit applies
    inv.worst_case_position_if_all_buys_fill = 0;
    inv.worst_case_position_if_all_sells_fill = 0;
    inv.authoritative = true;
    return inv;
}

inline inventory_snapshot inventory_at(qty_atoms position)
{
    inventory_snapshot inv = flat_inventory();
    inv.signed_base_position = position;
    inv.worst_case_position_if_all_buys_fill = position;
    inv.worst_case_position_if_all_sells_fill = position;
    return inv;
}

inline strategy_context default_context(timestamp_ns decision_time_ns
                                        = base_event_time_ns + 150'000)
{
    strategy_context ctx;
    ctx.decision_time_ns = decision_time_ns;
    ctx.instrument = default_instrument();
    ctx.symbol_id = 1;
    return ctx;
}

inline InventoryAwareMarketMakingStrategy make_strategy(const mm_config& cfg)
{
    InventoryAwareMarketMakingStrategy s;
    const auto status = s.configure(cfg);
    if (!status.ok)
        throw std::runtime_error(std::string("mm fixture config rejected: ") + status.message);
    return s;
}

// ── decision inspection helpers ─────────────────────────────────────────────

inline const quote_intent* find_intent(const quote_decision& d, order_side side,
                                       unsigned level)
{
    for (std::size_t i = 0; i < d.intents.size(); ++i)
        if (d.intents[i].side == side && d.intents[i].level == level)
            return &d.intents[i];
    return nullptr;
}

inline std::size_t count_side(const quote_decision& d, order_side side)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < d.intents.size(); ++i)
        if (d.intents[i].side == side)
            ++n;
    return n;
}

inline qty_atoms total_side_qty(const quote_decision& d, order_side side)
{
    qty_atoms total = 0;
    for (std::size_t i = 0; i < d.intents.size(); ++i)
        if (d.intents[i].side == side)
            total += d.intents[i].quantity;
    return total;
}

inline bool has_reason(const quote_decision& d, quote_reason r)
{
    return d.reasons.contains(r);
}

// ── JSON fixture I/O ────────────────────────────────────────────────────────

struct fixture_case
{
    std::string name;
    mm_config config;
    market_snapshot market;
    inventory_snapshot inventory;
    strategy_context context;
};

// Snapshot ids arrive either as a plain integer or as the documented
// "snap-000001" form. The hot path carries a uint64 id, so the string form is
// reduced to its trailing digits here, in cold test code.
inline std::uint64_t parse_snapshot_id(const nlohmann::json& j)
{
    if (j.is_number_unsigned())
        return j.get<std::uint64_t>();
    const std::string s = j.get<std::string>();
    std::size_t i = s.size();
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9')
        --i;
    if (i == s.size())
        throw std::runtime_error("mm fixture: snapshot_id '" + s + "' has no numeric suffix");
    return static_cast<std::uint64_t>(std::stoull(s.substr(i)));
}

inline void apply_config_json(const nlohmann::json& j, mm_config& cfg)
{
    if (j.contains("strategy_id")) cfg.strategy_id = j.at("strategy_id").get<std::string>();
    if (j.contains("levels")) cfg.levels = j.at("levels").get<unsigned>();

    if (const auto it = j.find("fair_value"); it != j.end())
    {
        const auto& f = *it;
        if (f.contains("microprice_weight")) cfg.fair_value.microprice_weight = f.at("microprice_weight").get<double>();
        if (f.contains("imbalance_weight")) cfg.fair_value.imbalance_weight = f.at("imbalance_weight").get<double>();
        if (f.contains("short_flow_weight")) cfg.fair_value.short_flow_weight = f.at("short_flow_weight").get<double>();
    }
    if (const auto it = j.find("inventory"); it != j.end())
    {
        const auto& v = *it;
        if (v.contains("hard_limit_base")) cfg.inventory.hard_limit_base = atoms_from_decimal(v.at("hard_limit_base").get<std::string>());
        if (v.contains("soft_limit_ratio")) cfg.inventory.soft_limit_ratio = v.at("soft_limit_ratio").get<double>();
        if (v.contains("reducing_bias_ratio")) cfg.inventory.reducing_bias_ratio = v.at("reducing_bias_ratio").get<double>();
        if (v.contains("reservation_skew_bps_at_hard_limit")) cfg.inventory.reservation_skew_bps_at_hard_limit = v.at("reservation_skew_bps_at_hard_limit").get<double>();
        if (v.contains("soft_limit_skew_boost")) cfg.inventory.soft_limit_skew_boost = v.at("soft_limit_skew_boost").get<double>();
        if (v.contains("size_skew_strength")) cfg.inventory.size_skew_strength = v.at("size_skew_strength").get<double>();
        if (v.contains("min_size_multiplier")) cfg.inventory.min_size_multiplier = v.at("min_size_multiplier").get<double>();
        if (v.contains("max_size_multiplier")) cfg.inventory.max_size_multiplier = v.at("max_size_multiplier").get<double>();
        if (v.contains("reducing_size_factor")) cfg.inventory.reducing_size_factor = v.at("reducing_size_factor").get<double>();
    }
    if (const auto it = j.find("spread"); it != j.end())
    {
        const auto& s = *it;
        if (s.contains("min_half_spread_bps")) cfg.spread.min_half_spread_bps = s.at("min_half_spread_bps").get<double>();
        if (s.contains("max_half_spread_bps")) cfg.spread.max_half_spread_bps = s.at("max_half_spread_bps").get<double>();
        if (s.contains("fee_buffer_bps")) cfg.spread.fee_buffer_bps = s.at("fee_buffer_bps").get<double>();
        if (s.contains("maker_fee_multiplier")) cfg.spread.maker_fee_multiplier = s.at("maker_fee_multiplier").get<double>();
        if (s.contains("volatility_multiplier")) cfg.spread.volatility_multiplier = s.at("volatility_multiplier").get<double>();
        if (s.contains("toxicity_multiplier")) cfg.spread.toxicity_multiplier = s.at("toxicity_multiplier").get<double>();
        if (s.contains("latency_buffer_bps")) cfg.spread.latency_buffer_bps = s.at("latency_buffer_bps").get<double>();
        if (s.contains("latency_multiplier")) cfg.spread.latency_multiplier = s.at("latency_multiplier").get<double>();
    }
    if (const auto it = j.find("quotes"); it != j.end())
    {
        const auto& q = *it;
        if (q.contains("base_size")) cfg.quotes.base_size = atoms_from_decimal(q.at("base_size").get<std::string>());
        if (q.contains("level_spacing_bps")) cfg.quotes.level_spacing_bps = q.at("level_spacing_bps").get<double>();
        if (q.contains("post_only")) cfg.quotes.post_only = q.at("post_only").get<bool>();
        if (q.contains("minimum_refresh_ticks")) cfg.quotes.minimum_refresh_ticks = q.at("minimum_refresh_ticks").get<std::uint32_t>();
        if (q.contains("minimum_quote_lifetime_ms")) cfg.quotes.minimum_quote_lifetime_ms = q.at("minimum_quote_lifetime_ms").get<std::int64_t>();
    }
    if (const auto it = j.find("safety"); it != j.end())
    {
        const auto& s = *it;
        if (s.contains("max_market_data_age_ms")) cfg.safety.max_market_data_age_ms = s.at("max_market_data_age_ms").get<std::int64_t>();
        if (s.contains("pause_on_sequence_gap")) cfg.safety.pause_on_sequence_gap = s.at("pause_on_sequence_gap").get<bool>();
        if (s.contains("require_authoritative_inventory")) cfg.safety.require_authoritative_inventory = s.at("require_authoritative_inventory").get<bool>();
    }
}

inline fixture_case parse_fixture(const nlohmann::json& j)
{
    fixture_case fc;
    fc.name = j.value("name", std::string("unnamed"));

    fc.config = default_config();
    if (const auto it = j.find("config"); it != j.end())
        apply_config_json(*it, fc.config);

    const auto& m = j.at("market");
    fc.market.event_time_ns = m.at("event_time_ns").get<timestamp_ns>();
    fc.market.receive_time_ns = m.at("receive_time_ns").get<timestamp_ns>();
    fc.market.best_bid = price_from_decimal(m.at("best_bid").at("price").get<std::string>());
    fc.market.best_ask = price_from_decimal(m.at("best_ask").at("price").get<std::string>());
    fc.market.best_bid_qty = atoms_from_decimal(m.at("best_bid").at("qty").get<std::string>());
    fc.market.best_ask_qty = atoms_from_decimal(m.at("best_ask").at("qty").get<std::string>());
    fc.market.short_horizon_volatility_bps = m.value("short_horizon_volatility_bps", 0.0);
    fc.market.toxicity_risk_bps = m.value("toxicity_risk_bps", 0.0);
    fc.market.latency_risk_bps = m.value("latency_risk_bps", 0.0);
    fc.market.short_flow_signal = m.value("short_flow_signal", 0.0);
    fc.market.sequence_valid = m.value("sequence_valid", true);
    fc.market.snapshot_id = parse_snapshot_id(m.at("snapshot_id"));

    const auto& inv = j.at("inventory");
    fc.inventory.signed_base_position = atoms_from_decimal(inv.at("signed_base_position").get<std::string>());
    fc.inventory.hard_limit = inv.contains("hard_limit")
        ? atoms_from_decimal(inv.at("hard_limit").get<std::string>())
        : 0;
    fc.inventory.worst_case_position_if_all_buys_fill =
        atoms_from_decimal(inv.at("worst_case_position_if_all_buys_fill").get<std::string>());
    fc.inventory.worst_case_position_if_all_sells_fill =
        atoms_from_decimal(inv.at("worst_case_position_if_all_sells_fill").get<std::string>());
    fc.inventory.authoritative = inv.value("authoritative", true);

    instrument_spec spec;
    const auto& is = j.at("instrument");
    spec.symbol = is.value("symbol", std::string("BTCUSDT"));
    spec.tick_size = std::stod(is.at("tick_size").get<std::string>());
    spec.lot_size = std::stod(is.at("lot_size").get<std::string>());
    if (is.contains("min_qty"))
        spec.min_qty = std::stod(is.at("min_qty").get<std::string>());
    spec.maker_rate = is.value("maker_fee_bps", 0.0) / 10000.0;

    mm_instrument ins{};
    const auto st = make_mm_instrument(spec, ins);
    if (st != instrument_status::ok)
        throw std::runtime_error(std::string("mm fixture instrument: ") + describe(st));

    fc.context.decision_time_ns = m.at("decision_time_ns").get<timestamp_ns>();
    fc.context.instrument = ins;
    fc.context.symbol_id = 1;
    return fc;
}

inline nlohmann::json serialize_decision(const quote_decision& d)
{
    nlohmann::json out;
    out["state"] = to_string(d.state);
    out["market_snapshot_id"] = d.market_snapshot_id;
    out["fair_value"] = decimal_from_price(d.fair_value);
    out["reservation_price"] = decimal_from_price(d.reservation_price);
    out["inventory_utilization"] = d.inventory_utilization;
    out["target_half_spread_bps"] = d.target_half_spread_bps;
    out["bid_size"] = decimal_from_atoms(d.bid_size);
    out["ask_size"] = decimal_from_atoms(d.ask_size);
    out["requote"] = d.requote;
    out["cancel_resting_quotes"] = d.cancel_resting_quotes;

    out["quotes"] = nlohmann::json::array();
    for (std::size_t i = 0; i < d.intents.size(); ++i)
    {
        const auto& q = d.intents[i];
        out["quotes"].push_back({
            {"side", q.side == order_side::buy ? "BUY" : "SELL"},
            {"price", decimal_from_price(q.price)},
            {"quantity", decimal_from_atoms(q.quantity)},
            {"level", q.level},
            {"post_only", q.post_only},
        });
    }

    out["reason_codes"] = nlohmann::json::array();
    for (std::size_t i = 0; i < d.reasons.size(); ++i)
        out["reason_codes"].push_back(to_string(d.reasons[i]));

    return out;
}

} // namespace truetest::mm::test
