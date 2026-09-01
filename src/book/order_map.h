#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "itch/messages.h"

namespace itch {

// std::unordered_map allocates every entry as a separate node, so a lookup follows a
// bucket pointer to a node that is almost certainly not in cache. With one lookup on
// roughly half of all messages, that miss dominates the replay.
//
// This is a flat open-addressed table: entries live in one contiguous array and a
// probe walks consecutive slots, so a collision usually stays within the cache line
// already fetched. Capacity is a power of two and fixed at construction, so there is
// no rehash to stall on mid-session.
template <typename Value>
class OrderMap {
public:
    explicit OrderMap(unsigned capacity_log2)
        : slots_(std::size_t(1) << capacity_log2), mask_(slots_.size() - 1) {}

    Value* find(OrderRef key) {
        std::size_t index = hash(key) & mask_;
        while (true) {
            Slot& slot = slots_[index];
            if (slot.key == key) return &slot.value;
            if (slot.key == kEmpty) return nullptr;
            index = (index + 1) & mask_;
        }
    }

    void insert(OrderRef key, const Value& value) {
        std::size_t index = hash(key) & mask_;
        while (true) {
            Slot& slot = slots_[index];
            if (slot.key == key) {
                slot.value = value;
                return;
            }
            if (slot.key == kEmpty) {
                slot.key = key;
                slot.value = value;
                ++size_;
                return;
            }
            index = (index + 1) & mask_;
        }
    }

    // Deleting from a linear-probe table cannot simply blank the slot: keys that
    // probed past it would become unreachable. Tombstones are the usual fix, but a
    // full session churns 125M orders through 8M slots, so they accumulate until no
    // empty slot remains and every miss degenerates into a full-table scan. Instead
    // the chain is repaired in place (Knuth 6.4 R), which keeps probe lengths bounded
    // no matter how much the table churns.
    void erase(OrderRef key) {
        std::size_t hole = hash(key) & mask_;
        while (true) {
            if (slots_[hole].key == key) break;
            if (slots_[hole].key == kEmpty) return;
            hole = (hole + 1) & mask_;
        }

        --size_;
        std::size_t probe = hole;
        while (true) {
            probe = (probe + 1) & mask_;
            if (slots_[probe].key == kEmpty) break;

            const std::size_t ideal = hash(slots_[probe].key) & mask_;
            if (((probe - ideal) & mask_) >= ((probe - hole) & mask_)) {
                slots_[hole] = slots_[probe];
                hole = probe;
            }
        }
        slots_[hole].key = kEmpty;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const Slot& slot : slots_) {
            if (slot.key != kEmpty) fn(slot.key, slot.value);
        }
    }

private:
    // Reference 0 never names a live order in the feed, so it is free to mean "empty".
    static constexpr OrderRef kEmpty = 0;

    struct Slot {
        OrderRef key = kEmpty;
        Value value{};
    };

    // Order references are dense and roughly sequential, so their low bits alone would
    // cluster badly. Mixing spreads them across the table.
    static std::size_t hash(OrderRef key) {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 29;
        return static_cast<std::size_t>(key);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace itch
