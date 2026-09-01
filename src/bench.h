#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "book/handler.h"
#include "itch/messages.h"
#include "itch/reader.h"
#include "itch/symbol_table.h"
#include "platform.h"

namespace itch {

// This machine's steady_clock ticks at 24 MHz, so a single message falls below its
// 41 ns resolution. Averages are therefore taken over batches, where quantisation
// error divides away; per-message timing is kept only for the tail, whose outliers
// are microseconds and comfortably above the noise floor.
inline constexpr std::size_t kBatchSize = 1024;

struct PhaseResult {
    const char* name = "";
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    double seconds = 0.0;

    double messages_per_second() const { return double(messages) / seconds; }
    double nanos_per_message() const { return seconds * 1e9 / double(messages); }
    double gigabytes_per_second() const { return double(bytes) / seconds / 1e9; }
};

class LatencyHistogram {
public:
    void record(std::uint64_t nanos) {
        samples_.push_back(nanos);
        if (nanos > max_) max_ = nanos;
    }

    void finalise() { std::sort(samples_.begin(), samples_.end()); }

    std::uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        const std::size_t index =
            std::min(samples_.size() - 1, std::size_t(p * double(samples_.size())));
        return samples_[index];
    }

    std::uint64_t max() const { return max_; }
    std::size_t size() const { return samples_.size(); }
    const std::vector<std::uint64_t>& samples() const { return samples_; }

private:
    std::vector<std::uint64_t> samples_;
    std::uint64_t max_ = 0;
};

// Walk the framing only: read each length prefix and advance. Establishes the floor
// that decoding and book maintenance are measured against.
inline PhaseResult bench_framing(const MappedFile& file, std::uint64_t limit) {
    MessageReader reader(file);
    PhaseResult result{"framing", 0, 0, 0.0};

    const auto start = std::chrono::steady_clock::now();
    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        if (limit && result.messages >= limit) break;
        result.bytes += 2 + length;
        ++result.messages;
    }
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

// Framing plus field extraction, with no book state. The accumulator exists solely
// to stop the optimiser deleting the decode it is supposed to be measuring.
inline PhaseResult bench_decode(const MappedFile& file, std::uint64_t limit,
                                std::uint64_t& sink) {
    MessageReader reader(file);
    PhaseResult result{"framing + decode", 0, 0, 0.0};
    std::uint64_t accumulator = 0;

    const auto start = std::chrono::steady_clock::now();
    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        if (limit && result.messages >= limit) break;
        result.bytes += 2 + length;
        ++result.messages;

        switch (static_cast<char>(payload[0])) {
            case 'A':
            case 'F': {
                const AddOrder m(payload);
                accumulator += m.order_ref() + m.price() + m.shares() + m.locate();
                break;
            }
            case 'E': {
                const OrderExecuted m(payload);
                accumulator += m.order_ref() + m.executed_shares();
                break;
            }
            case 'C': {
                const OrderExecutedPrice m(payload);
                accumulator += m.order_ref() + m.execution_price();
                break;
            }
            case 'X': {
                const OrderCancel m(payload);
                accumulator += m.order_ref() + m.cancelled_shares();
                break;
            }
            case 'D': {
                accumulator += OrderDelete(payload).order_ref();
                break;
            }
            case 'U': {
                const OrderReplace m(payload);
                accumulator += m.original_order_ref() + m.new_order_ref() + m.price();
                break;
            }
            default: break;
        }
    }
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    sink = accumulator;
    return result;
}

inline PhaseResult bench_book(const MappedFile& file, const SymbolTable& symbols,
                              std::uint64_t limit, LatencyHistogram* histogram) {
    BookHandler handler(symbols);
    MessageReader reader(file);
    PhaseResult result{"framing + decode + book", 0, 0, 0.0};

    const auto start = std::chrono::steady_clock::now();
    auto batch_start = start;
    std::size_t in_batch = 0;

    const std::byte* payload = nullptr;
    std::uint16_t length = 0;
    while (reader.next(payload, length)) {
        if (limit && result.messages >= limit) break;

        if (histogram) {
            const auto before = std::chrono::steady_clock::now();
            handler.apply(payload);
            const auto after = std::chrono::steady_clock::now();
            histogram->record(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
        } else {
            handler.apply(payload);
        }

        result.bytes += 2 + length;
        ++result.messages;

        if (++in_batch == kBatchSize) {
            in_batch = 0;
            batch_start = std::chrono::steady_clock::now();
        }
    }
    (void)batch_start;

    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace itch
