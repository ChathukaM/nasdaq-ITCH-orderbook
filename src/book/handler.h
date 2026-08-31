#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "book/order_book.h"
#include "itch/messages.h"
#include "itch/symbol_table.h"

namespace itch {

// Everything the book needs about a resting order. Execute, cancel and delete
// messages carry only a reference number, so this is the sole record of what an
// order was; without it those messages cannot be applied at all.
struct Order {
    Price price = 0;
    Shares shares = 0;
    StockLocate locate = 0;
    Side side = Side::Buy;
};

struct Stats {
    std::uint64_t orders_added = 0;
    std::uint64_t orders_deleted = 0;
    std::uint64_t orders_replaced = 0;
    std::uint64_t executions = 0;
    std::uint64_t cancels = 0;
    std::uint64_t executed_shares = 0;
    std::uint64_t hidden_trades = 0;
    std::uint64_t cross_trades = 0;

    // An E/X/D/U naming an order we never saw added means state was lost upstream.
    std::uint64_t unknown_order_refs = 0;
    std::uint64_t crossed_books = 0;
};

class BookHandler {
public:
    explicit BookHandler(const SymbolTable& symbols)
        : symbols_(symbols), books_(SymbolTable::kLocateDomain) {}

    void apply(const std::byte* payload) {
        switch (static_cast<char>(payload[0])) {
            case 'A':
            case 'F': on_add(AddOrder(payload)); break;
            case 'E': on_executed(OrderExecuted(payload)); break;
            case 'C': on_executed_price(OrderExecutedPrice(payload)); break;
            case 'X': on_cancel(OrderCancel(payload)); break;
            case 'D': on_delete(OrderDelete(payload)); break;
            case 'U': on_replace(OrderReplace(payload)); break;
            case 'P': ++stats_.hidden_trades; break;
            case 'Q': ++stats_.cross_trades; break;
            default: break;
        }
    }

    const OrderBook& book(StockLocate locate) const { return books_[locate]; }
    const SymbolTable& symbols() const { return symbols_; }
    const Stats& stats() const { return stats_; }
    std::size_t live_orders() const { return orders_.size(); }

private:
    void on_add(const AddOrder& msg) {
        const OrderRef ref = msg.order_ref();
        orders_[ref] = Order{msg.price(), msg.shares(), msg.locate(), msg.side()};
        books_[msg.locate()].add(msg.side(), msg.price(), msg.shares());
        ++stats_.orders_added;
    }

    // Executions reduce the order at its own resting price. For 'C' the print may
    // occur at a different price, but the book is unaffected by that difference.
    void on_executed(const OrderExecuted& msg) {
        const auto it = orders_.find(msg.order_ref());
        if (it == orders_.end()) {
            ++stats_.unknown_order_refs;
            return;
        }

        Order& order = it->second;
        const Shares executed = msg.executed_shares();
        ++stats_.executions;
        stats_.executed_shares += executed;

        if (executed >= order.shares) {
            books_[order.locate].remove(order.side, order.price, order.shares);
            orders_.erase(it);
        } else {
            order.shares -= executed;
            books_[order.locate].reduce(order.side, order.price, executed);
        }
    }

    void on_executed_price(const OrderExecutedPrice& msg) { on_executed(msg); }

    void on_cancel(const OrderCancel& msg) {
        const auto it = orders_.find(msg.order_ref());
        if (it == orders_.end()) {
            ++stats_.unknown_order_refs;
            return;
        }

        Order& order = it->second;
        const Shares cancelled = msg.cancelled_shares();
        ++stats_.cancels;

        if (cancelled >= order.shares) {
            books_[order.locate].remove(order.side, order.price, order.shares);
            orders_.erase(it);
        } else {
            order.shares -= cancelled;
            books_[order.locate].reduce(order.side, order.price, cancelled);
        }
    }

    void on_delete(const OrderDelete& msg) {
        const auto it = orders_.find(msg.order_ref());
        if (it == orders_.end()) {
            ++stats_.unknown_order_refs;
            return;
        }

        const Order& order = it->second;
        books_[order.locate].remove(order.side, order.price, order.shares);
        orders_.erase(it);
        ++stats_.orders_deleted;
    }

    // Replace carries no side or locate: the new order inherits both from the
    // original, which is another field only the order map can supply.
    void on_replace(const OrderReplace& msg) {
        const auto it = orders_.find(msg.original_order_ref());
        if (it == orders_.end()) {
            ++stats_.unknown_order_refs;
            return;
        }

        const Order original = it->second;
        books_[original.locate].remove(original.side, original.price, original.shares);
        orders_.erase(it);

        orders_[msg.new_order_ref()] =
            Order{msg.price(), msg.shares(), original.locate, original.side};
        books_[original.locate].add(original.side, msg.price(), msg.shares());
        ++stats_.orders_replaced;
    }

    const SymbolTable& symbols_;
    std::unordered_map<OrderRef, Order> orders_;
    std::vector<OrderBook> books_;
    Stats stats_;
};

}  // namespace itch
