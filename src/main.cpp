#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <vector>

#include "book/handler.h"
#include "itch/messages.h"
#include "itch/reader.h"
#include "itch/symbol_table.h"
#include "platform.h"

namespace {

void print_price(char* out, std::size_t n, itch::Price price) {
    std::snprintf(out, n, "%u.%04u", price / itch::kPriceScale, price % itch::kPriceScale);
}

void print_time(char* out, std::size_t n, std::uint64_t nanos) {
    const std::uint64_t seconds = nanos / 1'000'000'000ull;
    std::snprintf(out, n, "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>((seconds / 60) % 60),
                  static_cast<unsigned long long>(seconds % 60),
                  static_cast<unsigned long long>(nanos % 1'000'000'000ull));
}

// The directory is broadcast in full before the session opens, so a single pass
// over the leading R messages is enough to resolve every locate seen afterwards.
itch::SymbolTable build_symbol_table(const itch::MappedFile& file) {
    itch::SymbolTable table;
    itch::MessageReader reader(file);

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        const char type = static_cast<char>(payload[0]);
        if (type == 'R') {
            table.add(itch::StockDirectory(payload));
        } else if (type == 'A' || type == 'F') {
            break;
        }
    }
    return table;
}

int dump_symbols(const itch::MappedFile& file) {
    const itch::SymbolTable table = build_symbol_table(file);

    std::printf("%-8s %-8s %-4s %s\n", "locate", "symbol", "cat", "round lot");
    for (std::size_t locate = 0; locate < itch::SymbolTable::kLocateDomain; ++locate) {
        if (!table.contains(static_cast<itch::StockLocate>(locate))) continue;
        const auto& entry = table.entry(static_cast<itch::StockLocate>(locate));
        const auto symbol = table.symbol(static_cast<itch::StockLocate>(locate));
        std::printf("%-8zu %-8.*s %-4c %u\n", locate, static_cast<int>(symbol.size()),
                    symbol.data(), entry.market_category, entry.round_lot_size);
    }
    std::printf("\n%zu symbols\n", table.size());
    return 0;
}

int sample(const itch::MappedFile& file, long limit) {
    const itch::SymbolTable table = build_symbol_table(file);
    itch::MessageReader reader(file);

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    long shown = 0;

    std::printf("%-18s %-7s %-12s %-4s %8s  %-8s %12s\n", "time", "locate", "order ref", "side",
                "shares", "symbol", "price");

    while (shown < limit && reader.next(payload, length)) {
        if (static_cast<char>(payload[0]) != 'A') continue;

        const itch::AddOrder add(payload);
        const auto symbol = table.symbol(add.locate());
        char time[24];
        char price[24];
        print_time(time, sizeof time, add.timestamp());
        print_price(price, sizeof price, add.price());

        std::printf("%-18s %-7u %-12llu %-4c %8u  %-8.*s %12s\n", time, add.locate(),
                    static_cast<unsigned long long>(add.order_ref()),
                    static_cast<char>(add.side()), add.shares(),
                    static_cast<int>(symbol.size()), symbol.data(), price);
        ++shown;
    }
    return 0;
}

int census(const itch::MappedFile& file) {
    itch::SymbolTable table;
    itch::MessageReader reader(file);

    std::array<std::uint64_t, 256> counts{};
    std::uint64_t total = 0;
    std::uint64_t length_mismatches = 0;
    std::uint64_t unknown_types = 0;
    std::uint64_t unknown_locates = 0;
    std::uint64_t symbol_mismatches = 0;
    std::uint64_t symbol_checks = 0;

    const auto start = std::chrono::steady_clock::now();

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        const char type = static_cast<char>(payload[0]);
        counts[static_cast<unsigned char>(type)]++;
        ++total;

        const std::uint16_t expected = itch::expected_length(type);
        if (expected == 0) {
            ++unknown_types;
        } else if (expected != length) {
            ++length_mismatches;
        }

        if (type == 'R') {
            table.add(itch::StockDirectory(payload));
        } else if (type == 'A' || type == 'F') {
            const itch::AddOrder add(payload);
            if (!table.contains(add.locate())) {
                ++unknown_locates;
            } else {
                ++symbol_checks;
                if (table.symbol(add.locate()) != add.symbol()) ++symbol_mismatches;
            }
        }
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::size_t consumed = reader.bytes_consumed(file.data());

    std::printf("size:      %.2f GB\n", double(file.size()) / 1e9);
    std::printf("messages:  %llu\n", static_cast<unsigned long long>(total));
    std::printf("symbols:   %zu\n", table.size());
    std::printf("consumed:  %.2f GB", double(consumed) / 1e9);
    if (consumed != file.size()) {
        std::printf("   (%zu bytes trailing)", file.size() - consumed);
    }
    std::printf("\n\n");

    std::vector<std::pair<std::uint64_t, unsigned>> ranked;
    for (unsigned i = 0; i < counts.size(); ++i) {
        if (counts[i]) ranked.emplace_back(counts[i], i);
    }
    std::sort(ranked.rbegin(), ranked.rend());

    std::printf("%-4s %-30s %14s  %6s\n", "type", "name", "count", "share");
    for (const auto& [count, type] : ranked) {
        std::printf("%-4c %-30s %14llu  %5.2f%%\n", char(type),
                    itch::message_type_name(char(type)),
                    static_cast<unsigned long long>(count),
                    100.0 * double(count) / double(total));
    }

    std::printf("\nlength mismatches: %llu\n", static_cast<unsigned long long>(length_mismatches));
    std::printf("unknown types:     %llu\n", static_cast<unsigned long long>(unknown_types));
    std::printf("unknown locates:   %llu\n", static_cast<unsigned long long>(unknown_locates));
    std::printf("symbol checks:     %llu  (%llu mismatched)\n",
                static_cast<unsigned long long>(symbol_checks),
                static_cast<unsigned long long>(symbol_mismatches));
    std::printf("elapsed:           %.2f s   (%.2f M msg/s)\n", seconds,
                double(total) / seconds / 1e6);

    const bool clean = length_mismatches == 0 && unknown_types == 0 && unknown_locates == 0 &&
                       symbol_mismatches == 0;
    return clean ? 0 : 1;
}

void print_book(const itch::BookHandler& handler, itch::StockLocate locate, int depth) {
    const auto& book = handler.book(locate);
    const auto symbol = handler.symbols().symbol(locate);
    char price[24];

    std::printf("\n%.*s  (locate %u)\n", static_cast<int>(symbol.size()), symbol.data(), locate);
    std::printf("%12s %10s  |  %-10s %-12s\n", "bid size", "bid", "ask", "ask size");

    auto bid = book.bids().rbegin();
    auto ask = book.asks().begin();
    for (int i = 0; i < depth; ++i) {
        const bool has_bid = bid != book.bids().rend();
        const bool has_ask = ask != book.asks().end();
        if (!has_bid && !has_ask) break;

        if (has_bid) {
            print_price(price, sizeof price, bid->first);
            std::printf("%12llu %10s", static_cast<unsigned long long>(bid->second.shares), price);
            ++bid;
        } else {
            std::printf("%12s %10s", "", "");
        }

        std::printf("  |  ");

        if (has_ask) {
            print_price(price, sizeof price, ask->first);
            std::printf("%-10s %-12llu", price, static_cast<unsigned long long>(ask->second.shares));
            ++ask;
        }
        std::printf("\n");
    }

    if (book.has_bid() && book.has_ask()) {
        print_price(price, sizeof price, book.best_ask() - book.best_bid());
        std::printf("spread: %s\n", price);
    }
}

int replay(const itch::MappedFile& file, const std::vector<std::string>& watch, long limit) {
    const itch::SymbolTable symbols = build_symbol_table(file);
    itch::BookHandler handler(symbols);
    itch::MessageReader reader(file);

    std::uint64_t total = 0;
    std::uint64_t locked_observations = 0;
    std::uint64_t crossed_observations = 0;
    std::vector<std::string> crossed_examples;
    std::uint64_t last_timestamp = 0;

    const auto start = std::chrono::steady_clock::now();

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        if (limit > 0 && total >= static_cast<std::uint64_t>(limit)) break;
        handler.apply(payload);
        ++total;

        const char type = static_cast<char>(payload[0]);
        if (type == 'A' || type == 'F' || type == 'U') {
            const itch::MessageView view(payload);
            last_timestamp = view.timestamp();
            const auto& b = handler.book(view.locate());
            if (b.has_bid() && b.has_ask() && b.best_bid() >= b.best_ask()) {
                if (b.best_bid() == b.best_ask()) {
                    ++locked_observations;
                } else {
                    ++crossed_observations;
                    if (crossed_examples.size() < 12) {
                        char buf[256], t[24], bid[24], ask[24];
                        print_time(t, sizeof t, view.timestamp());
                        print_price(bid, sizeof bid, b.best_bid());
                        print_price(ask, sizeof ask, b.best_ask());
                        const auto sym = symbols.symbol(view.locate());
                        std::snprintf(buf, sizeof buf, "  %s  %-8.*s bid %s > ask %s",
                                      t, static_cast<int>(sym.size()), sym.data(), bid, ask);
                        crossed_examples.emplace_back(buf);
                    }
                }
            }
        }
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto& stats = handler.stats();

    char clock[24];
    print_time(clock, sizeof clock, last_timestamp);

    std::printf("messages replayed: %llu\n", static_cast<unsigned long long>(total));
    std::printf("last timestamp:    %s\n\n", clock);
    std::printf("orders added:      %llu\n", static_cast<unsigned long long>(stats.orders_added));
    std::printf("orders deleted:    %llu\n", static_cast<unsigned long long>(stats.orders_deleted));
    std::printf("orders replaced:   %llu\n", static_cast<unsigned long long>(stats.orders_replaced));
    std::printf("executions:        %llu  (%llu shares)\n",
                static_cast<unsigned long long>(stats.executions),
                static_cast<unsigned long long>(stats.executed_shares));
    std::printf("cancels:           %llu\n", static_cast<unsigned long long>(stats.cancels));
    std::printf("hidden trades:     %llu\n", static_cast<unsigned long long>(stats.hidden_trades));
    std::printf("cross trades:      %llu\n\n", static_cast<unsigned long long>(stats.cross_trades));
    std::printf("live orders left:  %zu\n", handler.live_orders());
    std::printf("unknown refs:      %llu\n",
                static_cast<unsigned long long>(stats.unknown_order_refs));
    std::printf("locked books:      %llu\n", static_cast<unsigned long long>(locked_observations));
    std::printf("crossed books:     %llu\n", static_cast<unsigned long long>(crossed_observations));
    for (const auto& example : crossed_examples) std::printf("%s\n", example.c_str());
    std::printf("elapsed:           %.2f s   (%.2f M msg/s)\n", seconds,
                double(total) / seconds / 1e6);

    for (const std::string& name : watch) {
        for (std::size_t locate = 1; locate < itch::SymbolTable::kLocateDomain; ++locate) {
            const auto code = static_cast<itch::StockLocate>(locate);
            if (symbols.contains(code) && symbols.symbol(code) == name) {
                print_book(handler, code, 5);
                break;
            }
        }
    }

    return stats.unknown_order_refs == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--sample N | --symbols | --replay [--limit N] "
                     "[--watch SYM,SYM]]\n",
                     argv[0]);
        return 1;
    }

    std::vector<std::string> args(argv + 1, argv + argc);
    const bool want_symbols = args.size() > 1 && args[1] == "--symbols";
    const bool want_replay = args.size() > 1 && args[1] == "--replay";

    long limit = 0;
    long sample_limit = 0;
    std::vector<std::string> watch;
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--sample") sample_limit = std::strtol(args[i + 1].c_str(), nullptr, 10);
        if (args[i] == "--limit") limit = std::strtol(args[i + 1].c_str(), nullptr, 10);
        if (args[i] == "--watch") {
            std::string current;
            for (char c : args[i + 1]) {
                if (c == ',') {
                    if (!current.empty()) watch.push_back(current);
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            if (!current.empty()) watch.push_back(current);
        }
    }

    try {
        itch::MappedFile file(argv[1]);
        if (want_symbols) return dump_symbols(file);
        if (want_replay) return replay(file, watch, limit);
        return sample_limit > 0 ? sample(file, sample_limit) : census(file);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
