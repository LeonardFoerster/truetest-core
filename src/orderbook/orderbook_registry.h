#pragma once
#include "orderbook.h"
#include "types/symbol_table.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ControlBlockPool;

class OrderbookRegistry
{
public:
    void set_order_pool_config(ControlBlockPool* cb_pool,
                               std::size_t min_blocks,
                               bool forbid_runtime_grow)
    {
        cb_pool_ = cb_pool;
        order_blocks_ = min_blocks;
        forbid_runtime_grow_ = forbid_runtime_grow;
        for (auto& [_, ob] : books_)
            ob->configure_order_pool(cb_pool_, order_blocks_, forbid_runtime_grow_);
    }

    std::shared_ptr<orderbook> get_or_create(const std::string& symbol)
    {
        const std::string& key = symbols_.intern(symbol);
        auto it = books_.find(key);
        if (it != books_.end())
            return it->second;
        auto ob = std::make_shared<orderbook>();
        if (order_blocks_ > 0)
            ob->configure_order_pool(cb_pool_, order_blocks_, forbid_runtime_grow_);
        books_[key] = ob;
        return ob;
    }

    std::shared_ptr<orderbook> get(const std::string& symbol) const
    {
        const std::uint16_t id = symbols_.id_of(symbol);
        if (id == SymbolTable::kInvalidId)
            return nullptr;
        const std::string& key = symbols_.resolve(id);
        auto it = books_.find(key);
        return (it != books_.end()) ? it->second : nullptr;
    }

    std::vector<std::string> symbols() const
    {
        std::vector<std::string> result;
        result.reserve(books_.size());
        for (const auto& [sym, _] : books_)
            result.push_back(sym);
        return result;
    }

    std::size_t size() const { return books_.size(); }

    void clear()
    {
        for (auto& [_, ob] : books_)
            ob->clear();
        books_.clear();
        symbols_.clear();
    }

    SymbolTable& symbol_table() { return symbols_; }
    const SymbolTable& symbol_table() const { return symbols_; }

private:
    SymbolTable symbols_;
    ControlBlockPool* cb_pool_ = nullptr;
    std::size_t order_blocks_ = 0;
    bool forbid_runtime_grow_ = false;

    std::unordered_map<std::string, std::shared_ptr<orderbook>> books_;
};
