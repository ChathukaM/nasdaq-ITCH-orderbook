#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "book/handler.h"
#include "book/validate.h"
#include "itch/messages.h"
#include "itch/reader.h"
#include "itch/symbol_table.h"
#include "platform.h"

namespace itch {

inline constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ull;

// Book state is snapshotted mid-session rather than at the end: by 20:00 every order
// has been cancelled and the book is empty, which would make the fingerprint trivial.
inline constexpr std::uint64_t kSnapshotTime = 15ull * 3600 * kNanosPerSecond;

struct ReplayResult {
    std::uint64_t messages = 0;
    std::uint64_t last_timestamp = 0;
    std::uint64_t locked_books = 0;
    std::uint64_t crossed_books = 0;
    std::uint64_t crossed_symbols = 0;
    std::uint64_t snapshot_fingerprint = 0;
    std::uint64_t snapshot_orders = 0;
    Reconciliation snapshot_reconciliation;
    Stats stats;
    std::size_t live_orders_at_end = 0;
    double seconds = 0.0;
    std::vector<std::string> crossed_examples;
};

// The directory is broadcast in full before the session opens, so scanning until the
// first order message is enough to resolve every locate seen afterwards.
inline SymbolTable build_symbol_table(const MappedFile& file) {
    SymbolTable table;
    MessageReader reader(file);

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        const char type = static_cast<char>(payload[0]);
        if (type == 'R') {
            table.add(StockDirectory(payload));
        } else if (type == 'A' || type == 'F') {
            break;
        }
    }
    return table;
}

void format_time(char* out, std::size_t n, std::uint64_t nanos);
void format_price(char* out, std::size_t n, Price price);

inline ReplayResult run_replay(const MappedFile& file, const SymbolTable& symbols,
                               BookHandler& handler, std::uint64_t limit, bool track_crossings) {
    ReplayResult result;
    MessageReader reader(file);

    std::vector<bool> seen_crossed(SymbolTable::kLocateDomain, false);
    bool snapshotted = false;

    const auto start = std::chrono::steady_clock::now();

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        if (limit && result.messages >= limit) break;
        handler.apply(payload);
        ++result.messages;

        const char type = static_cast<char>(payload[0]);
        if (type != 'A' && type != 'F' && type != 'U') continue;

        const MessageView view(payload);
        result.last_timestamp = view.timestamp();

        if (!snapshotted && view.timestamp() >= kSnapshotTime) {
            snapshotted = true;
            result.snapshot_fingerprint = fingerprint_books(handler, SymbolTable::kLocateDomain);
            result.snapshot_orders = handler.live_orders();
            result.snapshot_reconciliation = reconcile(handler, SymbolTable::kLocateDomain);
        }

        if (!track_crossings) continue;

        const OrderBook& book = handler.book(view.locate());
        if (!book.has_bid() || !book.has_ask()) continue;
        if (book.best_bid() < book.best_ask()) continue;

        if (book.best_bid() == book.best_ask()) {
            ++result.locked_books;
            continue;
        }

        ++result.crossed_books;
        if (!seen_crossed[view.locate()]) {
            seen_crossed[view.locate()] = true;
            ++result.crossed_symbols;

            if (result.crossed_examples.size() < 10) {
                char buf[256], time[24], bid[24], ask[24];
                format_time(time, sizeof time, view.timestamp());
                format_price(bid, sizeof bid, book.best_bid());
                format_price(ask, sizeof ask, book.best_ask());
                const auto symbol = symbols.symbol(view.locate());
                std::snprintf(buf, sizeof buf, "  %s  %-8.*s bid %s > ask %s", time,
                              static_cast<int>(symbol.size()), symbol.data(), bid, ask);
                result.crossed_examples.emplace_back(buf);
            }
        }
    }

    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.stats = handler.stats();
    result.live_orders_at_end = handler.live_orders();
    return result;
}

}  // namespace itch
