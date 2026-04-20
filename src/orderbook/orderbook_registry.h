#pragma once
#include "orderbook.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class OrderbookRegistry
{
public:
    std::shared_ptr<orderbook> get_or_create(const std::string& symbol)
    {
        auto it = books_.find(symbol);
        if (it != books_.end())
            return it->second;
        auto ob = std::make_shared<orderbook>();
        books_[symbol] = ob;
        return ob;
    }

    std::shared_ptr<orderbook> get(const std::string& symbol) const
    {
        auto it = books_.find(symbol);
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
    }

private:
    std::unordered_map<std::string, std::shared_ptr<orderbook>> books_;
};
