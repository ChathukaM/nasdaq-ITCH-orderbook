#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace itch {

// Nasdaq's BinaryFILE framing: each message is preceded by its length as a
// 2-byte big-endian value. There is no other delimiter and no resynchronisation
// point, so a single bad length desynchronises the rest of the file.
class MessageReader {
public:
    MessageReader(const std::byte* data, std::size_t size)
        : cursor_(data), end_(data + size) {}

    explicit MessageReader(const MappedFile& file)
        : MessageReader(file.data(), file.size()) {}

    bool next(const std::byte*& payload, std::uint16_t& length) {
        if (static_cast<std::size_t>(end_ - cursor_) < 2) return false;

        length = load_be16(cursor_);
        if (length == 0) return false;
        if (static_cast<std::size_t>(end_ - cursor_) < std::size_t(2) + length) return false;

        payload = cursor_ + 2;
        cursor_ += 2 + length;
        return true;
    }

    std::size_t bytes_consumed(const std::byte* origin) const { return cursor_ - origin; }

private:
    const std::byte* cursor_;
    const std::byte* end_;
};

inline const char* message_type_name(char type) {
    switch (type) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order";
        case 'F': return "Add Order with MPID";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed with Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'N': return "RPII";
        default:  return "unknown";
    }
}

}  // namespace itch
