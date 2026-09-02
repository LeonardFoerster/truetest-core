#pragma once

#include <cstddef>
#include <iostream>
#include <chrono>
#include <string>
#include <memory>
#include <stdexcept>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <string_view>

#ifdef HAS_DEBUG
#include "debug/copy_tracker.h"
#endif

enum class event_type
{
        market,
        signal,
        order,
        fill,
        tick,
        l2_snapshot,
        l2_update,
        cancel,
        amend,
        rejection,
        funding
};


class event 
{
public:
        event(event_type type, std::chrono::system_clock::time_point timestamp)
                : type_(type)
                , timestamp_ (timestamp)
        { }

        virtual ~event() = default;

        event_type get_type() const { return type_; }
        std::chrono::system_clock::time_point get_timestamp() const { return timestamp_; }

        int64_t get_recv_ns() const { return recv_ns_; }
        void set_recv_ns(int64_t ns) { recv_ns_ = ns; }

        int64_t get_latency_ns() const { return latency_ns_; }
        void set_latency_ns(int64_t ns) { latency_ns_ = ns; }

        virtual std::string to_string() const { return "Event[type=" + std::to_string(static_cast<int>(type_)) + "]"; }

protected:
        event_type type_;
        std::chrono::system_clock::time_point timestamp_;
        int64_t recv_ns_ = 0;
        int64_t latency_ns_ = 0;
};


using event_pointer = std::shared_ptr<event>;


class market_event : public event
#ifdef HAS_DEBUG
    , public debug::CopyTracker<market_event>
#endif
{
public:
        market_event
        (
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                double open,
                double high,
                double low,
                double close,
                int64_t volume = 0,
                std::uint64_t quantity_scale = 1
        )

                :event(event_type::market, timestamp)
                , symbol_ (symbol)
                , open_ (open)
                , high_ (high)
                , low_ (low)
                , close_ (close)
                , volume_ (volume)
                , quantity_scale_ (quantity_scale)
        { }

        const std::string& get_symbol() const { return symbol_; }
        double get_open() const { return open_; }
        double get_high() const { return high_; }
        double get_low() const { return low_; }
        double get_close() const { return close_; }
        int64_t get_volume() const { return volume_; }
        std::uint64_t get_quantity_scale() const { return quantity_scale_; }

        std::string to_string() const override 
        {
                return "MarketEvent[" + symbol_ + " OHLC=" +
                        std::to_string(open_) + "/" + std::to_string(high_) + "/" +
                        std::to_string(low_) + "/" + std::to_string(close_) + "]";
        }

private:
        std::string symbol_;
        double open_;
        double high_;
        double low_;
        double close_; 
        int64_t volume_;
        std::uint64_t quantity_scale_ = 1;

};

enum class signal_type
{
        buy,
        sell,
        hold
};


class signal_event : public event
{
public:
        signal_event
        (
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                signal_type signal,
                double strength = 1.0
        )
        : event(event_type::signal, timestamp)
        , symbol_(symbol)
        , signal_(signal)
        , strength_(strength)
        { }

        const std::string& get_symbol() const { return symbol_; }
        signal_type get_signal() const { return signal_; }
        double get_strength() const { return strength_; }

        std::string to_string() const override 
        {
                std::string signal_str;
                switch (signal_) 
                {
                        case signal_type::buy: signal_str = "BUY"; break;
                        case signal_type::sell: signal_str = "SELL"; break;
                        case signal_type::hold: signal_str = "HOLD"; break;
                }
                        return "SignalEvent[" + symbol_ + " " + signal_str + " strength=" + std::to_string(strength_) + "]";
        }

private:
        std::string symbol_;
        signal_type signal_;
        double strength_ = 0;
};


enum class order_type
{
        market,
        limit,
        stop,
        stop_limit
};

enum class time_in_force
{
        ioc,
        fok,
        gtc,
        day
};

enum class order_side
{
        buy,
        sell
};

// Why an engine-owned protective close was fired.  This stays in the core
// event vocabulary rather than making core depend on the exits layer: the
// value is carried across the order, audit, and execution boundaries.
enum class order_exit_reason : std::uint8_t
{
        none,
        stop_loss,
        take_profit,
        trailing_stop,
        time_stop,
        slippage_flatten
};

class order_event : public event
#ifdef HAS_DEBUG
    , public debug::CopyTracker<order_event>
#endif
{
public:
        order_event
        (
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                order_type order_type,
                order_side side,
                double quantity,
                double price = 0.0,
                time_in_force tif = time_in_force::gtc,
                double stop_price = 0.0
        )
                : event(event_type::order, timestamp)
                , symbol_(symbol)
                , order_type_(order_type)
                , side_(side)
                , quantity_(quantity)
                , price_(price)
                , tif_(tif)
                , stop_price_(stop_price)
                , earliest_eligible_ts_(timestamp)
        {
                if (tif == time_in_force::gtc && order_type == order_type::market)
                        tif_ = time_in_force::ioc;
        }

        uint64_t get_order_id() const { return order_id_; }
        void set_order_id(uint64_t id) { order_id_ = id; }

        // One strategy decision can produce one routed command.  The engine
        // currently assigns this alongside order_id; keeping it distinct in
        // the event schema makes the lifecycle audit explicit and leaves room
        // for a future multi-command signal without changing persisted rows.
        uint64_t get_signal_id() const { return signal_id_; }
        void set_signal_id(uint64_t id) { signal_id_ = id; }

        // Non-zero only for an engine-owned protective exit.  The ticket is
        // allocated before the close reaches route(), letting every terminal
        // rejection path recognise it before a synchronous fill can occur.
        uint64_t get_protective_exit_ticket() const { return protective_exit_ticket_; }
        void set_protective_exit_ticket(uint64_t id) { protective_exit_ticket_ = id; }
        order_exit_reason get_exit_reason() const { return exit_reason_; }
        void set_exit_reason(order_exit_reason reason) { exit_reason_ = reason; }

        // Engine-stamped before the candidate enters the active lifecycle.
        // Worker-side risk rechecks use this immutable decision snapshot,
        // rather than racing the current global lifecycle count.
        std::size_t get_pretrade_open_order_count() const { return pretrade_open_order_count_; }
        void set_pretrade_open_order_count(std::size_t count) { pretrade_open_order_count_ = count; }

        const std::string& get_symbol() const { return symbol_; }
        order_type get_order_type() const { return order_type_; }
        order_side get_side() const { return side_; }
        double get_quantity() const { return quantity_; }
        double get_price() const { return price_; }
        void set_quantity(double q) { quantity_ = q; }
        void set_price(double p) { price_ = p; }
        void set_stop_price(double p) { stop_price_ = p; }
        time_in_force get_tif() const { return tif_; }
        double get_stop_price() const { return stop_price_; }

        const std::string& get_strategy_name() const { return strategy_name_; }
        void set_strategy_name(const std::string& name) { strategy_name_ = name; }

        // Non-zero when this order closes (or scales out of) an earlier
        // entry; it references that entry's order_id. Openers leave this 0.
        uint64_t get_opener_order_id() const { return opener_order_id_; }
        void set_opener_order_id(uint64_t id) { opener_order_id_ = id; }

        std::chrono::system_clock::time_point get_earliest_eligible_ts() const { return earliest_eligible_ts_; }
        void set_earliest_eligible_ts(std::chrono::system_clock::time_point ts) { earliest_eligible_ts_ = ts; }

        std::chrono::system_clock::time_point get_submit_ts() const
        {
                return submit_ts_.time_since_epoch().count() != 0
                    ? submit_ts_ : get_timestamp();
        }
        void set_submit_ts(std::chrono::system_clock::time_point ts) { submit_ts_ = ts; }
        bool has_submit_ts() const { return submit_ts_.time_since_epoch().count() != 0; }

        // F-08 (docs/todos/11-F-forensic-lifecycle-audit.md) — when the
        // information that produced this order actually existed.
        //
        // A bar's timestamp is its OPEN time, so an order derived from
        // close[N] inherits a timestamp one full bar interval BEFORE that
        // close existed: the audit traced an order stamped 03:07:00 that was
        // decided from the 03:08:00 close. This is not a price lookahead —
        // execution is still correctly deferred by execution_bar_delay — but
        // it mis-dates every order, and every time-windowed risk rule
        // (orders_per_minute, trades_per_hour, daily_loss) inherits the error.
        //
        // The market-data clock is deliberately NOT overwritten: get_timestamp()
        // stays the bar's open, which is what it means. This carries the
        // decision instant alongside it, and defaults to the timestamp so an
        // order nobody stamps behaves exactly as before.
        std::chrono::system_clock::time_point get_decision_ts() const
        {
                return decision_ts_.time_since_epoch().count() != 0
                        ? decision_ts_ : get_timestamp();
        }
        void set_decision_ts(std::chrono::system_clock::time_point ts) { decision_ts_ = ts; }
        bool has_decision_ts() const { return decision_ts_.time_since_epoch().count() != 0; }


        std::string to_string() const override
        {
                std::string side_str = (side_ == order_side::buy) ? "BUY" : "SELL";
                std::string type_str;
                switch (order_type_) {
                case order_type::market: type_str = "MARKET"; break;
                case order_type::limit: type_str = "LIMIT"; break;
                case order_type::stop: type_str = "STOP"; break;
                case order_type::stop_limit: type_str = "STOP_LIMIT"; break;
                }
                return "OrderEvent[id=" + std::to_string(order_id_) + " " + type_str + " " + side_str + " " +
                        std::to_string(quantity_) + " " + symbol_ +
                        " @ " + std::to_string(price_) + "]";
        }

private:
        uint64_t order_id_ = 0;
        uint64_t signal_id_ = 0;
        uint64_t protective_exit_ticket_ = 0;
        std::size_t pretrade_open_order_count_ = 0;
        std::string symbol_;
        order_type order_type_;
        order_side side_;
        double quantity_;
        double price_;
        time_in_force tif_;
        double stop_price_;
        std::chrono::system_clock::time_point earliest_eligible_ts_;
        std::chrono::system_clock::time_point submit_ts_{};
        std::chrono::system_clock::time_point decision_ts_{};   // F-08
        std::string strategy_name_;
        uint64_t opener_order_id_ = 0;
        order_exit_reason exit_reason_ = order_exit_reason::none;
};


enum class fill_source
{
        unknown,
        simulated,
        exchange
};

// Execution provenance is deliberately scalar/enum-only: it travels with a
// fill through the engine hot path without allocating. Names are rendered in
// cold reporting code only.
enum class fill_execution_model : std::uint8_t
{
        unclassified,
        synthetic_local_liquidity,
        l2_local_book,
        queue_aware_paper,
        recorded_trade_tape,
        venue_reported
};

enum class fill_execution_reason : std::uint8_t
{
        unknown,
        aggressive_ladder_match,
        market_maker_requote,
        bar_range_sweep,
        recorded_trade_print,
        venue_execution_report
};

inline constexpr std::string_view fill_execution_model_name(
    fill_execution_model model) noexcept
{
        switch (model)
        {
        case fill_execution_model::synthetic_local_liquidity: return "synthetic_local_liquidity";
        case fill_execution_model::l2_local_book: return "l2_local_book";
        case fill_execution_model::queue_aware_paper: return "queue_aware_paper";
        case fill_execution_model::recorded_trade_tape: return "recorded_trade_tape";
        case fill_execution_model::venue_reported: return "venue_reported";
        case fill_execution_model::unclassified: return "unclassified";
        }
        return "unclassified";
}

inline constexpr std::string_view fill_execution_reason_name(
    fill_execution_reason reason) noexcept
{
        switch (reason)
        {
        case fill_execution_reason::aggressive_ladder_match: return "aggressive_ladder_match";
        case fill_execution_reason::market_maker_requote: return "market_maker_requote";
        case fill_execution_reason::bar_range_sweep: return "bar_range_sweep";
        case fill_execution_reason::recorded_trade_print: return "recorded_trade_print";
        case fill_execution_reason::venue_execution_report: return "venue_execution_report";
        case fill_execution_reason::unknown: return "unknown";
        }
        return "unknown";
}

struct fill_provenance
{
        fill_execution_model model = fill_execution_model::unclassified;
        fill_execution_reason reason = fill_execution_reason::unknown;
        bool exploratory = false;
        double intended_price = 0.0;
        double reference_price = 0.0;
        std::chrono::system_clock::time_point reference_timestamp{};
        double modeled_spread_bps = 0.0;
        double modeled_impact_bps = 0.0;
        double fill_probability = 1.0;
        std::chrono::nanoseconds modeled_latency{0};
};

enum class fill_cumulative_source : std::uint8_t
{
        absent,
        venue_reported,
        engine_accumulated,
        simulated
};

// Fixed-capacity wire identity used on the execution hot path.  Assignment
// is explicit and fails closed on oversize input; it never allocates.
template <std::size_t Capacity>
class bounded_event_text
{
public:
        [[nodiscard]] bool assign(std::string_view value) noexcept
        {
                if (value.empty() || value.size() > Capacity)
                        return false;
                std::copy(value.begin(), value.end(), storage_.begin());
                size_ = static_cast<std::uint16_t>(value.size());
                return true;
        }

        void clear() noexcept { size_ = 0; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] std::string_view view() const noexcept
        {
                return {storage_.data(), size_};
        }

private:
        static_assert(Capacity <= static_cast<std::size_t>(UINT16_MAX));
        std::array<char, Capacity> storage_{};
        std::uint16_t size_ = 0;
};

class fill_event : public event
#ifdef HAS_DEBUG
    , public debug::CopyTracker<fill_event>
#endif
{
public:
        fill_event
        (
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                uint64_t order_id,
                order_side side,
                double filled_quantity,
                double fill_price,
                double commission = 0.0,
                double remaining_qty = 0.0,
                uint64_t fill_id = 0,
                const std::string& strategy_name = {},
                uint64_t opener_order_id = 0
        )
                : event(event_type::fill, timestamp)
                , order_id_(order_id)
                , symbol_(symbol)
                , side_(side)
                , filled_quantity_(filled_quantity)
                , fill_price_(fill_price)
                , commission_(commission)
                , remaining_qty_(remaining_qty)
                , fill_id_(fill_id)
                , strategy_name_(strategy_name)
                , opener_order_id_(opener_order_id)
        {
        }

        uint64_t get_order_id() const { return order_id_; }
        const std::string& get_symbol() const { return symbol_; }
        order_side get_side() const { return side_; }
        double get_filled_quantity() const
        {
                return has_economic_quantity_
                        ? economic_quantity_ : filled_quantity_;
        }
        double get_reported_filled_quantity() const
        {
                return filled_quantity_;
        }
        // Preserve the raw venue slice for durable identity/reconnect replay,
        // while every economic consumer observes the exact delta implied by
        // the admitted cumulative cursor.
        void set_economic_quantity(double value) noexcept
        {
                economic_quantity_ = value;
                has_economic_quantity_ = true;
        }
        double get_fill_price() const { return fill_price_; }
        double get_commission() const { return commission_; }
        double get_remaining_qty() const { return remaining_qty_; }
        uint64_t get_fill_id() const { return fill_id_; }
        // Composite execution adapters merge child adapters whose diagnostic
        // counters are independent. They must assign one adapter-level ID
        // before exposing the fill to the canonical ingress gate.
        void set_fill_id(uint64_t value) noexcept { fill_id_ = value; }
        bool is_partial() const { return remaining_qty_ > 0.0; }

        [[nodiscard]] bool set_venue_execution_id(std::string_view value) noexcept
        {
                return venue_execution_id_.assign(value);
        }
        [[nodiscard]] std::string_view get_venue_execution_id() const noexcept
        {
                return venue_execution_id_.view();
        }

        [[nodiscard]] bool set_commission_currency(std::string_view value) noexcept
        {
                return commission_currency_.assign(value);
        }
        [[nodiscard]] std::string_view get_commission_currency() const noexcept
        {
                return commission_currency_.view();
        }

        void set_cumulative_filled_qty(double quantity,
                                       fill_cumulative_source source) noexcept
        {
                cumulative_filled_qty_ = quantity;
                cumulative_source_ = source;
        }
        [[nodiscard]] bool has_cumulative_filled_qty() const noexcept
        {
                return cumulative_source_ != fill_cumulative_source::absent;
        }
        [[nodiscard]] double get_cumulative_filled_qty() const noexcept
        {
                return cumulative_filled_qty_;
        }
        [[nodiscard]] fill_cumulative_source get_cumulative_source() const noexcept
        {
                return cumulative_source_;
        }

        fill_source get_source() const { return source_; }
        void set_source(fill_source s) { source_ = s; }

        const fill_provenance& get_provenance() const { return provenance_; }
        void set_provenance(const fill_provenance& provenance) { provenance_ = provenance; }

        // Per-lot / attribution (populated by engine at fill synthesis time for
        // both simulated and exchange fills; mirrors order_event fields for
        // clean propagation through rings to workers, QuestDB, shadow duals,
        // ExitManager, etc.). Openers typically have opener_order_id_ == 0.
        const std::string& get_strategy_name() const { return strategy_name_; }
        void set_strategy_name(const std::string& name) { strategy_name_ = name; }

        uint64_t get_opener_order_id() const { return opener_order_id_; }
        void set_opener_order_id(uint64_t id) { opener_order_id_ = id; }

        double get_total_cost() const
        {
                double base = get_filled_quantity() * fill_price_;
                return (side_ == order_side::buy) ? base + commission_ : base - commission_;
        }

        std::string to_string() const override
        {
                std::string side_str = (side_ == order_side::buy) ? "BOUGHT" : "SOLD";
                std::string s = "FillEvent[order_id=" + std::to_string(order_id_) +
                        " fill_id=" + std::to_string(fill_id_) + " " + side_str + " " +
                        std::to_string(filled_quantity_) + " " + symbol_ +
                        " @ " + std::to_string(fill_price_) +
                        " remaining=" + std::to_string(remaining_qty_) +
                        " commission=" + std::to_string(commission_);
                if (!strategy_name_.empty())
                        s += " strategy=" + strategy_name_;
                if (opener_order_id_ != 0)
                        s += " opener=" + std::to_string(opener_order_id_);
                s += "]";
                return s;
        }

private:
        uint64_t order_id_ = 0;
        std::string symbol_;
        order_side side_;
        double filled_quantity_;
        double economic_quantity_ = 0.0;
        bool has_economic_quantity_ = false;
        double fill_price_;
        double commission_;
        double remaining_qty_ = 0.0;
        uint64_t fill_id_ = 0;
        fill_source source_ = fill_source::unknown;
        fill_provenance provenance_{};
        bounded_event_text<96> venue_execution_id_{};
        bounded_event_text<24> commission_currency_{};
        double cumulative_filled_qty_ = 0.0;
        fill_cumulative_source cumulative_source_ = fill_cumulative_source::absent;

        // Per-lot attribution (enriched during deepdive refactor for consistent
        // propagation; default empty/0 for legacy compatibility).
        std::string strategy_name_;
        uint64_t opener_order_id_ = 0;
};


enum class tick_side
{
        bid,
        ask,
        unknown
};

class tick_event : public event
#ifdef HAS_DEBUG
    , public debug::CopyTracker<tick_event>
#endif
{
public:
        tick_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                double price,
                int64_t quantity,
                tick_side side = tick_side::unknown,
                std::uint64_t quantity_scale = 1
        )
                : event(event_type::tick, timestamp)
                , symbol_(symbol)
                , price_(price)
                , quantity_(quantity)
                , side_(side)
                , quantity_scale_(quantity_scale)
        { }

        const std::string& get_symbol() const { return symbol_; }
        double get_price() const { return price_; }
        int64_t get_quantity() const { return quantity_; }
        tick_side get_side() const { return side_; }
        std::uint64_t get_quantity_scale() const { return quantity_scale_; }

        std::string to_string() const override
        {
                std::string side_str;
                switch (side_) {
                case tick_side::bid: side_str = "BID"; break;
                case tick_side::ask: side_str = "ASK"; break;
                case tick_side::unknown: side_str = "UNK"; break;
                }
                return "TickEvent[" + symbol_ + " " + std::to_string(price_) +
                        " x" + std::to_string(quantity_) + " " + side_str + "]";
        }

private:
        std::string symbol_;
        double price_;
        int64_t quantity_;
        tick_side side_;
        std::uint64_t quantity_scale_ = 1;
};


struct l2_level
{
        double price;
        int64_t quantity;
};

// Binance depth20 and engine L2 paths cap at 20 levels per side (Phase 4).
inline constexpr std::size_t kL2SnapshotMaxLevels = 20;

class l2_snapshot_event : public event
{
public:
        l2_snapshot_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                const l2_level* bids,
                std::size_t bid_count,
                const l2_level* asks,
                std::size_t ask_count,
                std::uint64_t quantity_scale = 1
        )
                : event(event_type::l2_snapshot, timestamp)
                , symbol_(symbol)
                , quantity_scale_(quantity_scale)
        {
                bid_count_ = static_cast<std::uint8_t>(
                    std::min(bid_count, kL2SnapshotMaxLevels));
                ask_count_ = static_cast<std::uint8_t>(
                    std::min(ask_count, kL2SnapshotMaxLevels));
                if (bids && bid_count_ > 0)
                    std::copy_n(bids, bid_count_, bids_.begin());
                if (asks && ask_count_ > 0)
                    std::copy_n(asks, ask_count_, asks_.begin());
        }

        const std::string& get_symbol() const { return symbol_; }
        std::size_t bid_count() const { return bid_count_; }
        std::size_t ask_count() const { return ask_count_; }
        const l2_level& bid(std::size_t i) const { return bids_[i]; }
        const l2_level& ask(std::size_t i) const { return asks_[i]; }
        std::uint64_t get_quantity_scale() const { return quantity_scale_; }

        std::string to_string() const override
        {
                return "L2SnapshotEvent[" + symbol_ +
                        " bids=" + std::to_string(bid_count_) +
                        " asks=" + std::to_string(ask_count_) + "]";
        }

private:
        std::string symbol_;
        std::array<l2_level, kL2SnapshotMaxLevels> bids_{};
        std::array<l2_level, kL2SnapshotMaxLevels> asks_{};
        std::uint8_t bid_count_ = 0;
        std::uint8_t ask_count_ = 0;
        std::uint64_t quantity_scale_ = 1;
};


class l2_update_event : public event
{
public:
        l2_update_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                tick_side side,
                double price,
                int64_t new_quantity,
                std::uint64_t quantity_scale = 1
        )
                : event(event_type::l2_update, timestamp)
                , symbol_(symbol)
                , side_(side)
                , price_(price)
                , new_quantity_(new_quantity)
                , quantity_scale_(quantity_scale)
        { }

        const std::string& get_symbol() const { return symbol_; }
        tick_side get_side() const { return side_; }
        double get_price() const { return price_; }
        int64_t get_new_quantity() const { return new_quantity_; }
        std::uint64_t get_quantity_scale() const { return quantity_scale_; }

        std::string to_string() const override
        {
                std::string side_str = (side_ == tick_side::bid) ? "BID" : "ASK";
                return "L2UpdateEvent[" + symbol_ + " " + side_str +
                        " " + std::to_string(price_) +
                        " qty=" + std::to_string(new_quantity_) + "]";
        }

private:
        std::string symbol_;
        tick_side side_;
        double price_;
        int64_t new_quantity_;
        std::uint64_t quantity_scale_ = 1;
};


class cancel_event : public event
{
public:
        cancel_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                uint64_t order_id,
                const std::string& reason = ""
        )
                : event(event_type::cancel, timestamp)
                , symbol_(symbol)
                , order_id_(order_id)
                , reason_(reason)
        { }

        const std::string& get_symbol() const { return symbol_; }
        uint64_t get_order_id() const { return order_id_; }
        const std::string& get_reason() const { return reason_; }

        std::string to_string() const override
        {
                return "CancelEvent[order_id=" + std::to_string(order_id_) +
                        " " + symbol_ + " reason=" + reason_ + "]";
        }

private:
        std::string symbol_;
        uint64_t order_id_;
        std::string reason_;
};


class amend_event : public event
{
public:
        amend_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                uint64_t order_id,
                double new_price,
                double new_quantity
        )
                : event(event_type::amend, timestamp)
                , symbol_(symbol)
                , order_id_(order_id)
                , new_price_(new_price)
                , new_quantity_(new_quantity)
        { }

        const std::string& get_symbol() const { return symbol_; }
        uint64_t get_order_id() const { return order_id_; }
        double get_new_price() const { return new_price_; }
        double get_new_quantity() const { return new_quantity_; }

        std::string to_string() const override
        {
                return "AmendEvent[order_id=" + std::to_string(order_id_) +
                        " " + symbol_ +
                        " new_price=" + std::to_string(new_price_) +
                        " new_qty=" + std::to_string(new_quantity_) + "]";
        }

private:
        std::string symbol_;
        uint64_t order_id_;
        double new_price_;
        double new_quantity_;
};


class rejection_event : public event
{
public:
        rejection_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                uint64_t order_id,
                const std::string& reason = ""
        )
                : event(event_type::rejection, timestamp)
                , symbol_(symbol)
                , order_id_(order_id)
                , reason_(reason)
        { }

        const std::string& get_symbol() const { return symbol_; }
        uint64_t get_order_id() const { return order_id_; }
        const std::string& get_reason() const { return reason_; }

        std::string to_string() const override
        {
                return "RejectionEvent[order_id=" + std::to_string(order_id_) +
                        " " + symbol_ + " reason=" + reason_ + "]";
        }

private:
        std::string symbol_;
        uint64_t order_id_;
        std::string reason_;
};

// Funding settlement from the exchange (e.g. ACCOUNT_UPDATE with reason
// FUNDING_FEE). Construction is allocation-free because provider ingress is
// drained at the event-loop boundary. It updates cash/equity but does not
// close lots (unlike fill_event). See Phase 2 of prod.md.
class funding_event : public event
{
public:
    static constexpr std::size_t text_capacity = 31;

    funding_event(std::chrono::system_clock::time_point ts,
                  std::string_view symbol,
                  double qty_change,          // signed asset quantity (usually 0 for USDT-M)
                  double cash_delta,          // the actual USDT funding credit/debit
                  std::string_view reason = "FUNDING_FEE")
        : event(event_type::funding, ts)
        , qty_change_(qty_change)
        , cash_delta_(cash_delta)
    {
        symbol_size_ = assign_text(symbol_, symbol);
        reason_size_ = assign_text(reason_, reason);
    }

    std::string_view get_symbol() const
    {
        return {symbol_.data(), symbol_size_};
    }
    double get_qty_change() const { return qty_change_; }
    double get_cash_delta() const { return cash_delta_; }
    std::string_view get_reason() const
    {
        return {reason_.data(), reason_size_};
    }

    std::string to_string() const override
    {
        return "FundingEvent[" + std::string(get_symbol()) +
               " qty=" + std::to_string(qty_change_) +
               " cash=" + std::to_string(cash_delta_) +
               " reason=" + std::string(get_reason()) + "]";
    }

private:
    template<std::size_t N>
    static std::uint8_t assign_text(std::array<char, N>& target,
                                    std::string_view value)
    {
        if (value.empty() || value.size() >= N)
            throw std::length_error("funding event text is empty or too long");
        std::copy(value.begin(), value.end(), target.begin());
        target[value.size()] = '\0';
        return static_cast<std::uint8_t>(value.size());
    }

    std::array<char, text_capacity + 1> symbol_{};
    std::array<char, text_capacity + 1> reason_{};
    std::uint8_t symbol_size_ = 0;
    std::uint8_t reason_size_ = 0;
    double qty_change_;
    double cash_delta_;
};
