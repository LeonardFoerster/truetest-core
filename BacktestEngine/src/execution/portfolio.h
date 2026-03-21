#pragma once
#include "../core/event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct position
{
    int qty = 0;
    double cost_basis = 0.0;
};

class portfolio
{
public:
    portfolio();
    void on_fill(const fill_event& fill);
    void print_summary() const;

    bool position_open() const;
    bool position_open(const std::string& symbol) const;

    std::size_t get_total_trades() const { return total_trades_; }
    double get_cash() const { return cash_; }

    const std::unordered_map<std::string, position>& get_positions() const { return positions_; }

private:
    double cash_ = 0.0;
    std::unordered_map<std::string, position> positions_;
    std::size_t total_trades_ = 0;
};
