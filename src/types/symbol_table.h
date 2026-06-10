#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Phase 4: canonical symbol strings (one heap alloc per distinct symbol).
// Events still carry std::string today; registry and hot-path lookups use
// interned references to avoid duplicate allocations across sources.
class SymbolTable
{
public:
    static constexpr std::uint16_t kInvalidId = 0xFFFF;
    static constexpr std::size_t kMaxSymbols = 256;

    const std::string& intern(const std::string& sym)
    {
        auto it = index_.find(sym);
        if (it != index_.end())
            return symbols_[it->second];
        if (symbols_.size() >= kMaxSymbols)
            throw std::runtime_error("symbol table full");
        const std::uint16_t id = static_cast<std::uint16_t>(symbols_.size());
        symbols_.push_back(sym);
        index_.emplace(symbols_.back(), id);
        return symbols_.back();
    }

    const std::string& resolve(std::uint16_t id) const
    {
        return symbols_.at(id);
    }

    std::uint16_t id_of(const std::string& sym) const
    {
        auto it = index_.find(sym);
        return (it != index_.end()) ? it->second : kInvalidId;
    }

    std::size_t size() const { return symbols_.size(); }

    void clear()
    {
        symbols_.clear();
        index_.clear();
    }

private:
    std::vector<std::string> symbols_;
    std::unordered_map<std::string, std::uint16_t> index_;
};