#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "analysis/queue.h"
#include "bench.h"
#include "book/handler.h"
#include "book/validate.h"
#include "itch/messages.h"
#include "itch/reader.h"
#include "itch/symbol_table.h"
#include "platform.h"
#include "replay.h"

namespace itch {

void format_price(char* out, std::size_t n, Price price) {
    std::snprintf(out, n, "%u.%04u", price / kPriceScale, price % kPriceScale);
}

void format_time(char* out, std::size_t n, std::uint64_t nanos) {
    const std::uint64_t seconds = nanos / kNanosPerSecond;
    std::snprintf(out, n, "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>((seconds / 60) % 60),
                  static_cast<unsigned long long>(seconds % 60),
                  static_cast<unsigned long long>(nanos % kNanosPerSecond));
}

}  // namespace itch

namespace {

using ull = unsigned long long;

int locate_of(const itch::SymbolTable& symbols, const std::string& name) {
    for (std::size_t locate = 1; locate < itch::SymbolTable::kLocateDomain; ++locate) {
        const auto code = static_cast<itch::StockLocate>(locate);
        if (symbols.contains(code) && symbols.symbol(code) == name) return int(locate);
    }
    return -1;
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
            itch::format_price(price, sizeof price, bid->first);
            std::printf("%12llu %10s", ull(bid->second.shares), price);
            ++bid;
        } else {
            std::printf("%12s %10s", "", "");
        }
        std::printf("  |  ");
        if (has_ask) {
            itch::format_price(price, sizeof price, ask->first);
            std::printf("%-10s %-12llu", price, ull(ask->second.shares));
            ++ask;
        }
        std::printf("\n");
    }

    if (book.has_bid() && book.has_ask()) {
        itch::format_price(price, sizeof price, book.best_ask() - book.best_bid());
        std::printf("spread: %s\n", price);
    }
}

int census(const itch::MappedFile& file) {
    itch::SymbolTable table;
    itch::MessageReader reader(file);

    std::array<std::uint64_t, 256> counts{};
    std::uint64_t total = 0, length_mismatches = 0, unknown_types = 0;
    std::uint64_t unknown_locates = 0, symbol_mismatches = 0, symbol_checks = 0;

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

    std::printf("size:      %.2f GB\n", double(file.size()) / 1e9);
    std::printf("messages:  %llu\n", ull(total));
    std::printf("symbols:   %zu\n\n", table.size());

    std::vector<std::pair<std::uint64_t, unsigned>> ranked;
    for (unsigned i = 0; i < counts.size(); ++i) {
        if (counts[i]) ranked.emplace_back(counts[i], i);
    }
    std::sort(ranked.rbegin(), ranked.rend());

    std::printf("%-4s %-30s %14s  %6s\n", "type", "name", "count", "share");
    for (const auto& [count, type] : ranked) {
        std::printf("%-4c %-30s %14llu  %5.2f%%\n", char(type),
                    itch::message_type_name(char(type)), ull(count),
                    100.0 * double(count) / double(total));
    }

    std::printf("\nlength mismatches: %llu\n", ull(length_mismatches));
    std::printf("unknown types:     %llu\n", ull(unknown_types));
    std::printf("unknown locates:   %llu\n", ull(unknown_locates));
    std::printf("symbol checks:     %llu  (%llu mismatched)\n", ull(symbol_checks),
                ull(symbol_mismatches));

    return (length_mismatches || unknown_types || unknown_locates || symbol_mismatches) ? 1 : 0;
}

int dump_symbols(const itch::MappedFile& file) {
    const itch::SymbolTable table = itch::build_symbol_table(file);
    std::printf("%-8s %-8s %-4s %s\n", "locate", "symbol", "cat", "round lot");
    for (std::size_t locate = 0; locate < itch::SymbolTable::kLocateDomain; ++locate) {
        const auto code = static_cast<itch::StockLocate>(locate);
        if (!table.contains(code)) continue;
        const auto& entry = table.entry(code);
        const auto symbol = table.symbol(code);
        std::printf("%-8zu %-8.*s %-4c %u\n", locate, static_cast<int>(symbol.size()),
                    symbol.data(), entry.market_category, entry.round_lot_size);
    }
    std::printf("\n%zu symbols\n", table.size());
    return 0;
}

int sample(const itch::MappedFile& file, long limit) {
    const itch::SymbolTable table = itch::build_symbol_table(file);
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
        char time[24], price[24];
        itch::format_time(time, sizeof time, add.timestamp());
        itch::format_price(price, sizeof price, add.price());
        std::printf("%-18s %-7u %-12llu %-4c %8u  %-8.*s %12s\n", time, add.locate(),
                    ull(add.order_ref()), static_cast<char>(add.side()), add.shares(),
                    static_cast<int>(symbol.size()), symbol.data(), price);
        ++shown;
    }
    return 0;
}

int replay(const itch::MappedFile& file, const std::vector<std::string>& watch,
           std::uint64_t limit) {
    const itch::SymbolTable symbols = itch::build_symbol_table(file);
    itch::BookHandler handler(symbols);
    const itch::ReplayResult r = itch::run_replay(file, symbols, handler, limit, true);
    const auto& s = r.stats;

    char clock[24];
    itch::format_time(clock, sizeof clock, r.last_timestamp);

    std::printf("messages replayed: %llu\n", ull(r.messages));
    std::printf("last timestamp:    %s\n\n", clock);
    std::printf("orders added:      %llu\n", ull(s.orders_added));
    std::printf("orders deleted:    %llu\n", ull(s.orders_deleted));
    std::printf("orders replaced:   %llu\n", ull(s.orders_replaced));
    std::printf("executions:        %llu  (%llu shares)\n", ull(s.executions),
                ull(s.executed_shares));
    std::printf("cancels:           %llu\n", ull(s.cancels));
    std::printf("hidden trades:     %llu\n", ull(s.hidden_trades));
    std::printf("cross trades:      %llu\n", ull(s.cross_trades));

    std::printf("\n--- invariants ---\n");
    std::printf("unknown order refs:   %llu\n", ull(s.unknown_order_refs));
    std::printf("orders left at close: %zu\n", r.live_orders_at_end);
    std::printf("locked books:         %llu\n", ull(r.locked_books));
    std::printf("crossed books:        %llu   (%llu distinct symbols)\n", ull(r.crossed_books),
                ull(r.crossed_symbols));
    for (const auto& example : r.crossed_examples) std::printf("%s\n", example.c_str());

    const auto& rec = r.snapshot_reconciliation;
    std::printf("\n--- 15:00 snapshot ---\n");
    std::printf("live orders:          %llu\n", ull(r.snapshot_orders));
    std::printf("levels checked:       %llu\n", ull(rec.levels_checked));
    std::printf("share mismatches:     %llu\n", ull(rec.level_share_mismatches));
    std::printf("count mismatches:     %llu\n", ull(rec.level_count_mismatches));
    std::printf("orphaned orders:      %llu\n", ull(rec.orphaned_orders));
    std::printf("book fingerprint:     0x%016llx\n", ull(r.snapshot_fingerprint));

    std::printf("\nelapsed: %.2f s   (%.2f M msg/s)\n", r.seconds,
                double(r.messages) / r.seconds / 1e6);

    for (const std::string& name : watch) {
        const int locate = locate_of(symbols, name);
        if (locate > 0) print_book(handler, static_cast<itch::StockLocate>(locate), 5);
    }

    return (s.unknown_order_refs == 0 && rec.clean()) ? 0 : 1;
}

int bench(const itch::MappedFile& file, std::uint64_t limit, bool latency) {
    const itch::SymbolTable symbols = itch::build_symbol_table(file);

    // Each phase is measured on a warm mapping so page-fault cost is not charged to
    // whichever phase happens to run first.
    itch::warm_pages(file, limit ? limit * 40 : 0);

    std::uint64_t sink = 0;
    const itch::PhaseResult framing = itch::bench_framing(file, limit);
    const itch::PhaseResult decode = itch::bench_decode(file, limit, sink);
    const itch::PhaseResult book = itch::bench_book(file, symbols, limit, nullptr);

    std::printf("%-26s %14s %12s %10s %10s\n", "phase", "messages", "M msg/s", "ns/msg", "GB/s");
    for (const auto& phase : {framing, decode, book}) {
        std::printf("%-26s %14llu %12.2f %10.1f %10.2f\n", phase.name, ull(phase.messages),
                    phase.messages_per_second() / 1e6, phase.nanos_per_message(),
                    phase.gigabytes_per_second());
    }

    std::printf("\nmarginal cost of decoding:      %6.1f ns/msg\n",
                decode.nanos_per_message() - framing.nanos_per_message());
    std::printf("marginal cost of book updates:  %6.1f ns/msg\n",
                book.nanos_per_message() - decode.nanos_per_message());
    std::printf("(sink %llu)\n", ull(sink));

    if (latency) {
        itch::LatencyHistogram histogram;
        const itch::PhaseResult timed = itch::bench_book(file, symbols, limit, &histogram);
        histogram.finalise();

        std::printf("\n--- per-message latency (%zu samples) ---\n", histogram.size());
        std::printf("clock resolution on this host is 41 ns, so low percentiles are quantised\n");
        for (const double p : {0.50, 0.90, 0.99, 0.999, 0.9999}) {
            std::printf("p%-8.4g %8llu ns\n", p * 100, ull(histogram.percentile(p)));
        }
        std::printf("max      %8llu ns\n", ull(histogram.max()));
        std::printf("(timed pass %.2f s, instrumentation overhead included)\n", timed.seconds);
    }

    return 0;
}

// Regular session only: 09:30 to 16:00 ET. Pre- and post-market queues behave
// differently enough that mixing them would blur the result.
int queue(const itch::MappedFile& file, const std::string& name) {
    const itch::SymbolTable symbols = itch::build_symbol_table(file);
    const int locate = locate_of(symbols, name);
    if (locate < 0) {
        std::fprintf(stderr, "error: symbol %s not in the stock directory\n", name.c_str());
        return 1;
    }

    const std::uint64_t open = 9 * 3600ull * itch::kNanosPerSecond + 1800ull * itch::kNanosPerSecond;
    const std::uint64_t close = 16 * 3600ull * itch::kNanosPerSecond;

    itch::QueueAnalyser analyser(static_cast<itch::StockLocate>(locate), open, close);
    itch::MessageReader reader(file);

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) analyser.apply(payload);

    const auto& q = analyser.stats();
    const std::uint64_t resolved = q.orders_filled + q.orders_cancelled;

    std::printf("symbol:            %s (locate %d)\n", name.c_str(), locate);
    std::printf("orders joined:     %llu\n", ull(q.orders_tracked));
    std::printf("  filled:          %llu\n", ull(q.orders_filled));
    std::printf("  cancelled:       %llu\n", ull(q.orders_cancelled));
    if (resolved) {
        std::printf("fill rate:         %.2f%%\n", 100.0 * double(q.orders_filled) / double(resolved));
    }
    if (q.orders_tracked) {
        std::printf("mean shares ahead: %.0f\n",
                    double(q.ahead_shares_total) / double(q.orders_tracked));
    }
    if (q.orders_filled) {
        std::printf("mean wait to fill: %.2f s\n",
                    double(q.fill_wait_nanos_total) / double(q.orders_filled) / 1e9);
    }

    std::printf("\n%-8s %12s %12s %10s %14s\n", "queue", "joined", "filled", "fill %",
                "mean wait (s)");
    for (std::size_t b = 0; b < itch::QueueStats::kBuckets; ++b) {
        if (q.joined_at[b] == 0) continue;
        const double rate = 100.0 * double(q.filled_from[b]) / double(q.joined_at[b]);
        const double wait = q.filled_from[b]
                                ? double(q.wait_nanos_total[b]) / double(q.filled_from[b]) / 1e9
                                : 0.0;
        std::printf("%-8s %12llu %12llu %9.2f%% %14.2f\n", itch::QueueStats::bucket_label(b),
                    ull(q.joined_at[b]), ull(q.filled_from[b]), rate, wait);
    }

    std::printf("\ncsv,queue_bucket,joined,filled,fill_rate,mean_wait_s\n");
    for (std::size_t b = 0; b < itch::QueueStats::kBuckets; ++b) {
        if (q.joined_at[b] == 0) continue;
        const double rate = double(q.filled_from[b]) / double(q.joined_at[b]);
        const double wait = q.filled_from[b]
                                ? double(q.wait_nanos_total[b]) / double(q.filled_from[b]) / 1e9
                                : 0.0;
        std::printf("csv,%s,%llu,%llu,%.6f,%.6f\n", itch::QueueStats::bucket_label(b),
                    ull(q.joined_at[b]), ull(q.filled_from[b]), rate, wait);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [mode] [options]\n\n"
                     "modes:\n"
                     "  (none)            message type census and format checks\n"
                     "  --symbols         dump the stock locate table\n"
                     "  --sample N        print the first N decoded add-order messages\n"
                     "  --replay          rebuild the book, report invariants and fingerprint\n"
                     "  --bench           phase-by-phase throughput breakdown\n"
                     "  --queue SYM       queue position and fill probability for one symbol\n\n"
                     "options:\n"
                     "  --limit N         stop after N messages\n"
                     "  --watch SYM,SYM   print books for these symbols after a replay\n"
                     "  --latency         add a per-message latency histogram to --bench\n",
                     argv[0]);
        return 1;
    }

    const std::vector<std::string> args(argv + 1, argv + argc);
    const auto has = [&](const char* flag) {
        return std::find(args.begin(), args.end(), flag) != args.end();
    };
    const auto value_of = [&](const char* flag) -> std::string {
        const auto it = std::find(args.begin(), args.end(), flag);
        return (it != args.end() && it + 1 != args.end()) ? *(it + 1) : std::string();
    };

    const std::uint64_t limit = std::strtoull(value_of("--limit").c_str(), nullptr, 10);
    const long sample_limit = std::strtol(value_of("--sample").c_str(), nullptr, 10);

    std::vector<std::string> watch;
    {
        std::string current;
        for (char c : value_of("--watch")) {
            if (c == ',') {
                if (!current.empty()) watch.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) watch.push_back(current);
    }

    try {
        const itch::MappedFile file(args[0]);
        if (has("--symbols")) return dump_symbols(file);
        if (has("--bench")) return bench(file, limit, has("--latency"));
        if (!value_of("--queue").empty()) return queue(file, value_of("--queue"));
        if (has("--replay")) return replay(file, watch, limit);
        if (sample_limit > 0) return sample(file, sample_limit);
        return census(file);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
