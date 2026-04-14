#pragma once
#include "../core/event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct position
{
    double qty = 0.0;
    double cost_basis = 0.0;
};

class portfolio
{
public:
    portfolio();
    explicit portfolio(double initial_balance);
    void on_fill(const fill_event& fill);
    void print_summary() const;

    bool position_open() const;
    bool position_open(const std::string& symbol) const;

    // Can we afford this order? Checks cash for buys, position for sells.
    bool can_afford(order_side side, double quantity, double price) const;
    bool can_afford(const std::string& symbol, order_side side, double quantity, double price) const;

    // Compute position size based on risk fraction of current equity
    double compute_quantity(double price, double risk_fraction) const;

    std::size_t get_total_trades() const { return total_trades_; }
    double get_cash() const { return cash_; }
    double get_initial_balance() const { return initial_balance_; }
    double get_equity(double last_price) const;

    const std::unordered_map<std::string, position>& get_positions() const { return positions_; }

    // K3: restore portfolio state from a checkpoint. Overwrites cash, positions,
    // and total_trades wholesale. initial_balance is preserved (it represents the
    // session start balance, not the restored cash).
    void restore_state(double cash, std::size_t total_trades,
                       std::unordered_map<std::string, position> positions)
    {
        cash_ = cash;
        total_trades_ = total_trades;
        positions_ = std::move(positions);
    }

private:
    double initial_balance_ = 10000.0;
    double cash_ = 10000.0;
    std::unordered_map<std::string, position> positions_;
    std::size_t total_trades_ = 0;
};
