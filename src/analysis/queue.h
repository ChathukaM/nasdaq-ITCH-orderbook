#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>

#include "itch/messages.h"

namespace itch {

// Queue position cannot be derived from aggregated depth: two feeds showing the same
// 300 shares at $150.00 say nothing about whether an order joining now sits behind
// one order or thirty. Order-by-order data is the only way to know, which is the
// reason this project needs ITCH rather than a cheaper depth feed.
struct QueueStats {
    static constexpr std::size_t kBuckets = 8;

    std::uint64_t orders_tracked = 0;
    std::uint64_t orders_filled = 0;
    std::uint64_t orders_cancelled = 0;

    std::array<std::uint64_t, kBuckets> joined_at{};
    std::array<std::uint64_t, kBuckets> filled_from{};
    std::array<std::uint64_t, kBuckets> wait_nanos_total{};

    std::uint64_t ahead_shares_total = 0;
    std::uint64_t fill_wait_nanos_total = 0;

    // Buckets are 0, 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+.
    static std::size_t bucket_for(std::uint32_t position) {
        std::size_t bucket = 0;
        std::uint32_t bound = 1;
        while (bucket + 1 < kBuckets && position >= bound) {
            bound *= 2;
            ++bucket;
        }
        return bucket;
    }

    static const char* bucket_label(std::size_t bucket) {
        static const char* labels[kBuckets] = {"0", "1", "2-3", "4-7",
                                               "8-15", "16-31", "32-63", "64+"};
        return labels[bucket];
    }
};

// Tracks one symbol at full order-by-order resolution, including the ordering of
// orders within each price level, which the production book deliberately omits.
class QueueAnalyser {
public:
    QueueAnalyser(StockLocate locate, std::uint64_t session_open, std::uint64_t session_close)
        : locate_(locate), open_(session_open), close_(session_close) {}

    void apply(const std::byte* payload) {
        switch (static_cast<char>(payload[0])) {
            case 'A':
            case 'F': {
                const AddOrder m(payload);
                if (m.locate() != locate_) return;
                join(m.order_ref(), m.side(), m.price(), m.shares(), m.timestamp());
                break;
            }
            case 'E':
            case 'C': {
                const OrderExecuted m(payload);
                consume(m.order_ref(), m.executed_shares(), m.timestamp(), true);
                break;
            }
            case 'X': {
                const OrderCancel m(payload);
                consume(m.order_ref(), m.cancelled_shares(), m.timestamp(), false);
                break;
            }
            case 'D': {
                const OrderDelete m(payload);
                remove(m.order_ref(), m.timestamp(), false);
                break;
            }
            case 'U': {
                const OrderReplace m(payload);
                const auto it = orders_.find(m.original_order_ref());
                if (it == orders_.end()) return;
                const Side side = it->second.side;
                remove(m.original_order_ref(), m.timestamp(), false);
                join(m.new_order_ref(), side, m.price(), m.shares(), m.timestamp());
                break;
            }
            default: break;
        }
    }

    const QueueStats& stats() const { return stats_; }

private:
    struct Resting {
        Price price = 0;
        Shares shares = 0;
        Side side = Side::Buy;
        std::uint64_t joined_at = 0;
        std::uint32_t position_on_join = 0;
        std::uint64_t shares_ahead_on_join = 0;
        bool in_session = false;
    };

    struct QueueLevel {
        std::deque<OrderRef> order_refs;
        std::uint64_t shares = 0;
    };

    using Levels = std::map<Price, QueueLevel>;

    Levels& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }

    void join(OrderRef ref, Side side, Price price, Shares shares, std::uint64_t timestamp) {
        QueueLevel& level = levels(side)[price];

        Resting resting;
        resting.price = price;
        resting.shares = shares;
        resting.side = side;
        resting.joined_at = timestamp;
        resting.position_on_join = static_cast<std::uint32_t>(level.order_refs.size());
        resting.shares_ahead_on_join = level.shares;
        resting.in_session = timestamp >= open_ && timestamp <= close_;

        level.order_refs.push_back(ref);
        level.shares += shares;
        orders_[ref] = resting;

        if (resting.in_session) {
            ++stats_.orders_tracked;
            stats_.ahead_shares_total += resting.shares_ahead_on_join;
            ++stats_.joined_at[QueueStats::bucket_for(resting.position_on_join)];
        }
    }

    void consume(OrderRef ref, Shares amount, std::uint64_t timestamp, bool executed) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return;

        if (amount >= it->second.shares) {
            remove(ref, timestamp, executed);
            return;
        }

        it->second.shares -= amount;
        levels(it->second.side)[it->second.price].shares -= amount;
    }

    void remove(OrderRef ref, std::uint64_t timestamp, bool executed) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return;

        const Resting resting = it->second;
        Levels& book = levels(resting.side);
        const auto level_it = book.find(resting.price);
        if (level_it != book.end()) {
            QueueLevel& level = level_it->second;
            level.shares -= resting.shares;
            for (auto q = level.order_refs.begin(); q != level.order_refs.end(); ++q) {
                if (*q == ref) {
                    level.order_refs.erase(q);
                    break;
                }
            }
            if (level.order_refs.empty()) book.erase(level_it);
        }
        orders_.erase(it);

        if (!resting.in_session) return;

        const std::size_t bucket = QueueStats::bucket_for(resting.position_on_join);
        if (executed) {
            ++stats_.orders_filled;
            ++stats_.filled_from[bucket];
            const std::uint64_t wait = timestamp - resting.joined_at;
            stats_.fill_wait_nanos_total += wait;
            stats_.wait_nanos_total[bucket] += wait;
        } else {
            ++stats_.orders_cancelled;
        }
    }

    StockLocate locate_;
    std::uint64_t open_;
    std::uint64_t close_;
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderRef, Resting> orders_;
    QueueStats stats_;
};

}  // namespace itch
