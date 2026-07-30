#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Canonical symbol strings + dense uint16 ids (one heap alloc per distinct symbol).
// Hot-path state should index by id (vector slot) after a single intern_id() call,
// not re-hash std::string on every bar.
class SymbolTable
{
public:
    static constexpr std::uint16_t kInvalidId = 0xFFFF;
    static constexpr std::size_t kMaxSymbols = 256;

    // Intern and return dense id. O(1) average; allocates only on first sighting.
    std::uint16_t intern_id(const std::string& sym)
    {
        auto it = index_.find(sym);
        if (it != index_.end())
            return it->second;
        if (symbols_.size() >= kMaxSymbols)
            throw std::runtime_error("symbol table full");
        const std::uint16_t id = static_cast<std::uint16_t>(symbols_.size());
        symbols_.push_back(sym);
        index_.emplace(symbols_.back(), id);
        return id;
    }

    // Intern and return stable reference into the table (valid until clear()).
    const std::string& intern(const std::string& sym)
    {
        return symbols_[intern_id(sym)];
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
