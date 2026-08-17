#pragma once
#include "../core/event.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct position
{
    double qty = 0.0;
    double cost_basis = 0.0;
};

// Per-entry bookkeeping that lives alongside the netted `position` map.
// Strategies get independent attribution for each entry (including a long
// and a short on the same symbol - the venue still sees the netted balance,
// but the lot table keeps each leg's entry price, SL/TP origin, and owning
// strategy distinct for analytics and exit matching.
struct lot
{
    std::string symbol;
    order_side  side = order_side::buy;    // buy=long opener, sell=short opener
    double      qty_open = 0.0;             // remaining open qty (decreases as closers fill)
    double      entry_price = 0.0;          // weighted avg across opener fills
    double      entry_filled_qty = 0.0;     // cumulative opener fill qty (for avg math)
    std::string strategy_name;
    std::chrono::system_clock::time_point ts_open{};
};

class portfolio
{
public:
    portfolio();
    explicit portfolio(double initial_balance);

    // Legacy entry: treats `fill` as an opener keyed by fill.order_id.
    void on_fill(const fill_event& fill);

    // Preferred entry. `opener_order_id` identifies which lot this fill
    // belongs to: a closer passes the original opener's id, so the correct
    // lot is reduced; an opener passes its own order_id and a new lot is
    // created, tagged with `strategy_name`. Passing opener_order_id == 0
    // skips lot bookkeeping entirely (used by risk-unwind paths that flat
    // the netted book without caring which lots they close).
    void on_fill(const fill_event& fill, std::uint64_t opener_order_id,
                 const std::string& strategy_name = {});

    // Funding settlement (non-lot event). Updates cash and a separate P&L accumulator.
    // Does not affect lots (funding does not open or close positions in the bookkeeping sense).
    void on_funding(const funding_event& fe);

    bool position_open() const;
    bool position_open(const std::string& symbol) const;

    std::size_t get_total_trades() const { return total_trades_; }
    std::size_t get_total_fills() const { return total_fills_; }
    double get_cash() const { return cash_; }
    double get_initial_balance() const { return initial_balance_; }
    // Single-price mark (legacy / single-symbol). Prefer the marks overload
    // for multi-symbol portfolios.
    double get_equity(double last_price) const;
    // Per-symbol marks; symbols missing from `marks` fall back to `fallback_price`.
    double get_equity(const std::unordered_map<std::string, double>& marks,
                      double fallback_price = 0.0) const;
    double get_total_funding_pnl() const { return total_funding_pnl_; }

    const std::unordered_map<std::string, position>& get_positions() const { return positions_; }

    const std::unordered_map<std::uint64_t, lot>& get_lots() const { return lots_; }

    std::vector<std::uint64_t> open_lots_by_symbol(const std::string& symbol) const;

    void restore_state(double cash, std::size_t total_trades,
                       std::unordered_map<std::string, position> positions)
    {
        cash_ = cash;
        total_trades_ = total_trades;
        positions_ = std::move(positions);
    }

    // Phase A (MC object reuse): resets the portfolio to its initial state
    // (cash = initial_balance, no positions, no lots, counters zeroed).
    void reset();

private:
    double initial_balance_ = 10000.0;
    double cash_ = 10000.0;
    std::unordered_map<std::string, position> positions_;
    std::unordered_map<std::uint64_t, lot>    lots_;
    std::size_t total_trades_ = 0;
    std::size_t total_fills_ = 0;
    double total_funding_pnl_ = 0.0;   // separate accumulator for funding P&L

    void apply_netted_fill(const fill_event& fill);
    void apply_lot_fill(const fill_event& fill, std::uint64_t opener_order_id,
                        const std::string& strategy_name);
};
