#pragma once
#include <iostream>
#include <chrono>
#include <string>
#include <memory>
#include <vector>

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
        rejection
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

        // Hot-path latency stamp (set on fill_event when the book matches,
        // representing now - originating market/tick recv_ns). Zero on other
        // event types.
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
                int64_t volume = 0
        )

                :event(event_type::market, timestamp)
                , symbol_ (symbol)
                , open_ (open)
                , high_ (high)
                , low_ (low)
                , close_ (close)
                , volume_ (volume)
        { }

        const std::string& get_symbol() const { return symbol_; }
        double get_open() const { return open_; }
        double get_high() const { return high_; }
        double get_low() const { return low_; }
        double get_close() const { return close_; }
        int64_t get_volume() const { return volume_; }

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
        ioc,    // immediate-or-cancel: fill what you can, cancel rest
        fok,    // fill-or-kill: all or nothing
        gtc,    // good-till-cancel: stays on book
        day     // good-till-end-of-session
};

enum class order_side
{
        buy,
        sell
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
                // Default TIF based on order type if caller used gtc (the default)
                if (tif == time_in_force::gtc && order_type == order_type::market)
                        tif_ = time_in_force::ioc;
        }

        uint64_t get_order_id() const { return order_id_; }
        void set_order_id(uint64_t id) { order_id_ = id; }

        const std::string& get_symbol() const { return symbol_; }
        order_type get_order_type() const { return order_type_; }
        order_side get_side() const { return side_; }
        double get_quantity() const { return quantity_; }
        double get_price() const { return price_; }
        time_in_force get_tif() const { return tif_; }
        double get_stop_price() const { return stop_price_; }

        const std::string& get_strategy_name() const { return strategy_name_; }
        void set_strategy_name(const std::string& name) { strategy_name_ = name; }

        std::chrono::system_clock::time_point get_earliest_eligible_ts() const { return earliest_eligible_ts_; }
        void set_earliest_eligible_ts(std::chrono::system_clock::time_point ts) { earliest_eligible_ts_ = ts; }

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
        std::string symbol_;
        order_type order_type_;
        order_side side_;
        double quantity_;
        double price_;
        time_in_force tif_;
        double stop_price_;
        std::chrono::system_clock::time_point earliest_eligible_ts_;
        std::string strategy_name_;
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
                uint64_t fill_id = 0
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
        {
        }

        uint64_t get_order_id() const { return order_id_; }
        const std::string& get_symbol() const { return symbol_; }
        order_side get_side() const { return side_; }
        double get_filled_quantity() const { return filled_quantity_; }
        double get_fill_price() const { return fill_price_; }
        double get_commission() const { return commission_; }
        double get_remaining_qty() const { return remaining_qty_; }
        uint64_t get_fill_id() const { return fill_id_; }
        bool is_partial() const { return remaining_qty_ > 0.0; }


        double get_total_cost() const
        {
                double base = filled_quantity_ * fill_price_;
                return (side_ == order_side::buy) ? base + commission_ : base - commission_;
        }

        std::string to_string() const override
        {
                std::string side_str = (side_ == order_side::buy) ? "BOUGHT" : "SOLD";
                return "FillEvent[order_id=" + std::to_string(order_id_) +
                        " fill_id=" + std::to_string(fill_id_) + " " + side_str + " " +
                        std::to_string(filled_quantity_) + " " + symbol_ +
                        " @ " + std::to_string(fill_price_) +
                        " remaining=" + std::to_string(remaining_qty_) +
                        " commission=" + std::to_string(commission_) + "]";
        }

private:
        uint64_t order_id_ = 0;
        std::string symbol_;
        order_side side_;
        double filled_quantity_;
        double fill_price_;
        double commission_;
        double remaining_qty_ = 0.0;
        uint64_t fill_id_ = 0;
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
                tick_side side = tick_side::unknown
        )
                : event(event_type::tick, timestamp)
                , symbol_(symbol)
                , price_(price)
                , quantity_(quantity)
                , side_(side)
        { }

        const std::string& get_symbol() const { return symbol_; }
        double get_price() const { return price_; }
        int64_t get_quantity() const { return quantity_; }
        tick_side get_side() const { return side_; }

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
};


struct l2_level
{
        double price;
        int64_t quantity;
};

class l2_snapshot_event : public event
{
public:
        l2_snapshot_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                std::vector<l2_level> bids,
                std::vector<l2_level> asks
        )
                : event(event_type::l2_snapshot, timestamp)
                , symbol_(symbol)
                , bids_(std::move(bids))
                , asks_(std::move(asks))
        { }

        const std::string& get_symbol() const { return symbol_; }
        const std::vector<l2_level>& get_bids() const { return bids_; }
        const std::vector<l2_level>& get_asks() const { return asks_; }

        std::string to_string() const override
        {
                return "L2SnapshotEvent[" + symbol_ +
                        " bids=" + std::to_string(bids_.size()) +
                        " asks=" + std::to_string(asks_.size()) + "]";
        }

private:
        std::string symbol_;
        std::vector<l2_level> bids_;
        std::vector<l2_level> asks_;
};


class l2_update_event : public event
{
public:
        l2_update_event(
                std::chrono::system_clock::time_point timestamp,
                const std::string& symbol,
                tick_side side,
                double price,
                int64_t new_quantity
        )
                : event(event_type::l2_update, timestamp)
                , symbol_(symbol)
                , side_(side)
                , price_(price)
                , new_quantity_(new_quantity)
        { }

        const std::string& get_symbol() const { return symbol_; }
        tick_side get_side() const { return side_; }
        double get_price() const { return price_; }
        int64_t get_new_quantity() const { return new_quantity_; }

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
