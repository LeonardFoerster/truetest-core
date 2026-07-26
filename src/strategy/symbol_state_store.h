#pragma once

#include "types/symbol_table.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Dense per-symbol state store for strategy hot paths.
//
// Before (#3 bottleneck): unordered_map<string, State> → hash string every bar.
// After: SymbolTable::intern_id once (hash), then vector[id] O(1) pointer chase.
//
// Also returns the interned symbol string so order_event / exit_intent can copy
// from a stable buffer without building temporary keys.
template <typename State>
class SymbolStateStore
{
public:
    using id_type = std::uint16_t;
    using factory_type = std::function<State()>;

    explicit SymbolStateStore(factory_type factory)
        : factory_(std::move(factory))
    {}

    struct ref
    {
        State&             state;
        const std::string& symbol; // interned, stable until clear()
        id_type            id;
    };

    // Create-or-return state for symbol.
    ref get(const std::string& symbol)
    {
        const id_type id = table_.intern_id(symbol);
        ensure_slot(id);
        if (!slots_[id].has_value())
            slots_[id].emplace(factory_());
        return ref{*slots_[id], table_.resolve(id), id};
    }

    // Non-creating lookup (diagnostics / const paths).
    const State* find(const std::string& symbol) const
    {
        const id_type id = table_.id_of(symbol);
        if (id == SymbolTable::kInvalidId ||
            static_cast<std::size_t>(id) >= slots_.size() ||
            !slots_[id].has_value())
            return nullptr;
        return &(*slots_[id]);
    }

    State* find_mut(const std::string& symbol)
    {
        const id_type id = table_.id_of(symbol);
        if (id == SymbolTable::kInvalidId ||
            static_cast<std::size_t>(id) >= slots_.size() ||
            !slots_[id].has_value())
            return nullptr;
        return &(*slots_[id]);
    }

    void clear()
    {
        slots_.clear();
        table_.clear();
    }

    std::size_t size() const { return table_.size(); }
    const SymbolTable& table() const { return table_; }

private:
    void ensure_slot(id_type id)
    {
        const std::size_t need = static_cast<std::size_t>(id) + 1;
        if (slots_.size() < need)
            slots_.resize(need);
    }

    SymbolTable                     table_;
    std::vector<std::optional<State>> slots_;
    factory_type                    factory_;
};
