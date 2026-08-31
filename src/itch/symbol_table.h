#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "itch/messages.h"

namespace itch {

// Only A, F, P and Q carry a symbol; every other message identifies its stock by
// the 2-byte locate in the common header. The Stock Directory ('R') messages sent
// before the trading session map one to the other.
class SymbolTable {
public:
    struct Entry {
        std::array<char, 8> symbol{};
        Shares round_lot_size = 0;
        char market_category = '\0';
        bool present = false;
    };

    // Locate is a uint16_t, so sizing to its full domain removes the bounds branch
    // and any resize logic from a path every message takes. Only the ~9k entries
    // actually used are ever faulted in, so the unused tail costs no physical memory.
    SymbolTable() : entries_(kLocateDomain) {}

    void add(const StockDirectory& directory) {
        Entry& entry = entries_[directory.locate()];
        if (!entry.present) ++count_;

        const std::string_view symbol = directory.symbol();
        entry.symbol.fill(' ');
        for (std::size_t i = 0; i < symbol.size() && i < entry.symbol.size(); ++i) {
            entry.symbol[i] = symbol[i];
        }
        entry.round_lot_size = directory.round_lot_size();
        entry.market_category = directory.market_category();
        entry.present = true;
    }

    std::string_view symbol(StockLocate locate) const {
        const Entry& entry = entries_[locate];
        if (!entry.present) return {};

        std::size_t length = entry.symbol.size();
        while (length > 0 && entry.symbol[length - 1] == ' ') --length;
        return std::string_view(entry.symbol.data(), length);
    }

    const Entry& entry(StockLocate locate) const { return entries_[locate]; }
    bool contains(StockLocate locate) const { return entries_[locate].present; }
    std::size_t size() const { return count_; }

    static constexpr std::size_t kLocateDomain = 65536;

private:
    std::vector<Entry> entries_;
    std::size_t count_ = 0;
};

}  // namespace itch
