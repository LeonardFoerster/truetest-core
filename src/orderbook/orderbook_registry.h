#pragma once
#include "orderbook.h"
#include "types/symbol_table.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class ControlBlockPool;

// Per-symbol orderbooks indexed by dense SymbolTable id (not string-hash).
// get_or_create: one intern_id hash, then O(1) vector slot.
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
        for (auto& ob : books_)
        {
            if (ob)
                ob->configure_order_pool(cb_pool_, order_blocks_, forbid_runtime_grow_);
        }
    }

    std::shared_ptr<orderbook> get_or_create(const std::string& symbol)
    {
        const std::uint16_t id = symbols_.intern_id(symbol);
        ensure_slot(id);
        if (!books_[id])
        {
            auto ob = std::make_shared<orderbook>();
            if (order_blocks_ > 0)
                ob->configure_order_pool(cb_pool_, order_blocks_, forbid_runtime_grow_);
            books_[id] = std::move(ob);
        }
        return books_[id];
    }

    std::shared_ptr<orderbook> get(const std::string& symbol) const
    {
        const std::uint16_t id = symbols_.id_of(symbol);
        if (id == SymbolTable::kInvalidId ||
            static_cast<std::size_t>(id) >= books_.size())
            return nullptr;
        return books_[id];
    }

    std::vector<std::string> symbols() const
    {
        std::vector<std::string> result;
        result.reserve(symbols_.size());
        for (std::size_t i = 0; i < books_.size(); ++i)
        {
            if (books_[i])
                result.push_back(symbols_.resolve(static_cast<std::uint16_t>(i)));
        }
        return result;
    }

    template <typename Fn>
    void for_each_book(Fn&& fn) const
    {
        for (std::size_t i = 0; i < books_.size(); ++i)
        {
            if (books_[i])
                fn(symbols_.resolve(static_cast<std::uint16_t>(i)), *books_[i]);
        }
    }

    std::size_t size() const
    {
        std::size_t n = 0;
        for (const auto& ob : books_)
            if (ob) ++n;
        return n;
    }

    void clear()
    {
        for (auto& ob : books_)
        {
            if (ob)
                ob->clear();
        }
        books_.clear();
        symbols_.clear();
    }

    SymbolTable& symbol_table() { return symbols_; }
    const SymbolTable& symbol_table() const { return symbols_; }

private:
    void ensure_slot(std::uint16_t id)
    {
        const std::size_t need = static_cast<std::size_t>(id) + 1;
        if (books_.size() < need)
            books_.resize(need);
    }

    SymbolTable symbols_;
    std::vector<std::shared_ptr<orderbook>> books_;
    ControlBlockPool* cb_pool_ = nullptr;
    std::size_t order_blocks_ = 0;
    bool forbid_runtime_grow_ = false;
};
