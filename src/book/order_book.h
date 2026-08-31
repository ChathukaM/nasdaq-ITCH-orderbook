#pragma once

#include <cstdint>
#include <map>

#include "itch/messages.h"

namespace itch {

// A price level aggregates the orders resting at one price. Individual orders are
// not stored here -- the order map already records each order's price, so a list
// per level would duplicate that. Step 9 adds one, since queue position needs it.
struct Level {
    std::uint64_t shares = 0;
    std::uint32_t order_count = 0;
};

// Bids and asks for a single symbol. Both sides are ordered ascending by price;
// the best bid is therefore the last entry and the best ask the first.
class OrderBook {
public:
    using Levels = std::map<Price, Level>;

    void add(Side side, Price price, Shares shares) {
        Level& level = levels(side)[price];
        level.shares += shares;
        ++level.order_count;
    }

    // Shares leaving a level without the order itself leaving: partial execution
    // or partial cancel.
    void reduce(Side side, Price price, Shares shares) {
        Levels& book = levels(side);
        const auto it = book.find(price);
        if (it == book.end()) return;

        it->second.shares -= shares;
    }

    void remove(Side side, Price price, Shares remaining_shares) {
        Levels& book = levels(side);
        const auto it = book.find(price);
        if (it == book.end()) return;

        it->second.shares -= remaining_shares;
        if (--it->second.order_count == 0) book.erase(it);
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }

    Price best_bid() const { return bids_.rbegin()->first; }
    Price best_ask() const { return asks_.begin()->first; }

    const Level& best_bid_level() const { return bids_.rbegin()->second; }
    const Level& best_ask_level() const { return asks_.begin()->second; }

    const Levels& bids() const { return bids_; }
    const Levels& asks() const { return asks_; }

    bool crossed() const { return has_bid() && has_ask() && best_bid() >= best_ask(); }

private:
    Levels& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }

    Levels bids_;
    Levels asks_;
};

}  // namespace itch
