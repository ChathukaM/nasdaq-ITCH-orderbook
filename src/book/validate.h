#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "book/handler.h"

namespace itch {

struct Reconciliation {
    std::uint64_t levels_checked = 0;
    std::uint64_t level_share_mismatches = 0;
    std::uint64_t level_count_mismatches = 0;
    std::uint64_t orders_checked = 0;
    std::uint64_t orphaned_orders = 0;

    bool clean() const {
        return level_share_mismatches == 0 && level_count_mismatches == 0 && orphaned_orders == 0;
    }
};

// A 64-bit digest of complete book state. Two runs that process the same input must
// produce the same value, so any optimisation that changes it has changed behaviour.
// Iteration is ordered by locate then by price so the result does not depend on
// container layout or platform.
class Fingerprint {
public:
    void mix(std::uint64_t value) {
        hash_ ^= value;
        hash_ *= 0x100000001b3ull;
        hash_ = (hash_ << 31) | (hash_ >> 33);
    }

    std::uint64_t value() const { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
};

inline std::uint64_t fingerprint_books(const BookHandler& handler, std::size_t locate_domain) {
    Fingerprint fp;
    for (std::size_t locate = 0; locate < locate_domain; ++locate) {
        const OrderBook& book = handler.book(static_cast<StockLocate>(locate));
        if (book.bids().empty() && book.asks().empty()) continue;

        fp.mix(locate);
        for (const auto& [price, level] : book.bids()) {
            fp.mix(0x42);
            fp.mix(price);
            fp.mix(level.shares);
            fp.mix(level.order_count);
        }
        for (const auto& [price, level] : book.asks()) {
            fp.mix(0x53);
            fp.mix(price);
            fp.mix(level.shares);
            fp.mix(level.order_count);
        }
    }
    return fp.value();
}

// Rebuilds every price level independently from the order map and compares. If the
// incremental updates applied during replay have drifted at all, this finds it.
inline Reconciliation reconcile(const BookHandler& handler, std::size_t locate_domain) {
    struct Key {
        StockLocate locate;
        char side;
        Price price;
        bool operator==(const Key& other) const {
            return locate == other.locate && side == other.side && price == other.price;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (std::size_t(k.locate) << 40) ^ (std::size_t(k.price) << 1) ^
                   std::size_t(k.side);
        }
    };

    Reconciliation result;
    std::unordered_map<Key, Level, KeyHash> rebuilt;
    rebuilt.reserve(handler.live_orders());

    for (const auto& [ref, order] : handler.orders()) {
        (void)ref;
        Level& level = rebuilt[Key{order.locate, order.side == Side::Buy ? 'B' : 'S', order.price}];
        level.shares += order.shares;
        ++level.order_count;
        ++result.orders_checked;
    }

    for (std::size_t locate = 0; locate < locate_domain; ++locate) {
        const OrderBook& book = handler.book(static_cast<StockLocate>(locate));
        const auto check = [&](const OrderBook::Levels& levels, char side) {
            for (const auto& [price, level] : levels) {
                ++result.levels_checked;
                const auto it =
                    rebuilt.find(Key{static_cast<StockLocate>(locate), side, price});
                const std::uint64_t shares = it == rebuilt.end() ? 0 : it->second.shares;
                const std::uint32_t count = it == rebuilt.end() ? 0 : it->second.order_count;
                if (shares != level.shares) ++result.level_share_mismatches;
                if (count != level.order_count) ++result.level_count_mismatches;
                if (it != rebuilt.end()) rebuilt.erase(it);
            }
        };
        check(book.bids(), 'B');
        check(book.asks(), 'S');
    }

    // Anything left describes orders the book never recorded a level for.
    result.orphaned_orders = rebuilt.size();
    return result;
}

}  // namespace itch
