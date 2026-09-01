#pragma once

#include <cstdint>

#include "book/price_levels.h"
#include "itch/messages.h"

namespace itch {

// A price level aggregates the orders resting at one price. Individual orders are not
// stored here -- the order map already records each order's price, so a list per level
// would duplicate that. The queue analyser keeps its own, since queue position needs
// the ordering within a level.
using Level = PriceLevel;

class OrderBook {
public:
    void add(Side side, Price price, Shares shares) {
        if (side == Side::Buy) {
            bids_.add(price, shares);
        } else {
            asks_.add(price, shares);
        }
    }

    // Shares leaving a level without the order itself leaving: partial execution or
    // partial cancel.
    void reduce(Side side, Price price, Shares shares) {
        if (side == Side::Buy) {
            bids_.reduce(price, shares);
        } else {
            asks_.reduce(price, shares);
        }
    }

    void remove(Side side, Price price, Shares remaining_shares) {
        if (side == Side::Buy) {
            bids_.remove(price, remaining_shares);
        } else {
            asks_.remove(price, remaining_shares);
        }
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }

    Price best_bid() const { return bids_.best().price; }
    Price best_ask() const { return asks_.best().price; }

    const Level& best_bid_level() const { return bids_.best(); }
    const Level& best_ask_level() const { return asks_.best(); }

    const BidLevels& bids() const { return bids_; }
    const AskLevels& asks() const { return asks_; }

    bool crossed() const { return has_bid() && has_ask() && best_bid() >= best_ask(); }

private:
    BidLevels bids_;
    AskLevels asks_;
};

}  // namespace itch
