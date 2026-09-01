#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "itch/messages.h"

namespace itch {

struct PriceLevel {
    Price price = 0;
    std::uint64_t shares = 0;
    std::uint32_t order_count = 0;
};

// std::map is a red-black tree: each level sits in its own heap node, and finding one
// walks a chain of pointers with a likely cache miss at every step. A sorted array
// searches contiguous memory instead, which is far friendlier to the cache.
//
// The catch is insertion cost. Book depth is heavily skewed -- median 45 levels but a
// tail past 3500, concentrated in exactly the symbols that carry the most traffic --
// so an insert at the wrong end memmoves tens of kilobytes on the hottest path.
//
// Both sides therefore keep the *best* price at the back: bids ascending, asks
// descending. New levels at or near the touch, which is where activity concentrates,
// then append or shift only a short tail. Deep levels are still O(n) to insert, but
// those are rare.
template <bool BestIsHighest>
class PriceLevels {
public:
    using Container = std::vector<PriceLevel>;

    void add(Price price, Shares shares) {
        const auto it = find_slot(price);
        if (it != levels_.end() && it->price == price) {
            it->shares += shares;
            ++it->order_count;
            return;
        }
        levels_.insert(it, PriceLevel{price, shares, 1});
    }

    void reduce(Price price, Shares shares) {
        const auto it = find_slot(price);
        if (it == levels_.end() || it->price != price) return;
        it->shares -= shares;
    }

    void remove(Price price, Shares remaining_shares) {
        const auto it = find_slot(price);
        if (it == levels_.end() || it->price != price) return;

        it->shares -= remaining_shares;
        if (--it->order_count == 0) levels_.erase(it);
    }

    bool empty() const { return levels_.empty(); }
    std::size_t size() const { return levels_.size(); }

    const PriceLevel& best() const { return levels_.back(); }
    const Container& all() const { return levels_; }

private:
    static bool precedes(Price a, Price b) { return BestIsHighest ? a < b : a > b; }

    typename Container::iterator find_slot(Price price) {
        return std::lower_bound(
            levels_.begin(), levels_.end(), price,
            [](const PriceLevel& level, Price target) { return precedes(level.price, target); });
    }

    Container levels_;
};

using BidLevels = PriceLevels<true>;
using AskLevels = PriceLevels<false>;

}  // namespace itch
