#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "platform.h"

namespace itch {

enum class MessageType : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    TradingAction = 'H',
    RegSHO = 'Y',
    MarketParticipant = 'L',
    MWCBLevel = 'V',
    MWCBStatus = 'W',
    IPOQuotingPeriod = 'K',
    LULDAuctionCollar = 'J',
    OperationalHalt = 'h',
    AddOrder = 'A',
    AddOrderMPID = 'F',
    OrderExecuted = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
    CrossTrade = 'Q',
    BrokenTrade = 'B',
    NOII = 'I',
    RPII = 'N',
};

enum class Side : char { Buy = 'B', Sell = 'S' };

// Prices are unsigned integers scaled by 10^4: 1500000 is $150.0000. They are kept
// integral end to end; scaling happens only where a value is printed.
using Price = std::uint32_t;
using Shares = std::uint32_t;
using OrderRef = std::uint64_t;
using StockLocate = std::uint16_t;

inline constexpr Price kPriceScale = 10000;

// Payload length for each type, or 0 if the type carries no fixed length we rely on.
// Every message type in ITCH 5.0 is fixed-width, so a length mismatch means the
// stream has desynchronised or the type byte was misread.
inline constexpr std::uint16_t expected_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        default:  return 0;
    }
}

// Symbols are 8 bytes, left-justified and space-padded rather than null-terminated.
inline std::string_view read_symbol(const std::byte* p) {
    const char* chars = reinterpret_cast<const char*>(p);
    std::size_t len = 8;
    while (len > 0 && chars[len - 1] == ' ') --len;
    return std::string_view(chars, len);
}

// Fields common to every message. Views hold a pointer into the mapping and decode
// on access, so a handler only pays for the fields it actually reads.
class MessageView {
public:
    explicit MessageView(const std::byte* p) : p_(p) {}

    char type() const { return static_cast<char>(p_[0]); }
    StockLocate locate() const { return load_be16(p_ + 1); }
    std::uint64_t timestamp() const { return load_be48(p_ + 5); }

    const std::byte* raw() const { return p_; }

protected:
    const std::byte* p_;
};

class SystemEvent : public MessageView {
public:
    using MessageView::MessageView;
    char event_code() const { return static_cast<char>(p_[11]); }
};

class StockDirectory : public MessageView {
public:
    using MessageView::MessageView;
    std::string_view symbol() const { return read_symbol(p_ + 11); }
    char market_category() const { return static_cast<char>(p_[19]); }
    char financial_status() const { return static_cast<char>(p_[20]); }
    Shares round_lot_size() const { return load_be32(p_ + 21); }
};

class AddOrder : public MessageView {
public:
    using MessageView::MessageView;
    OrderRef order_ref() const { return load_be64(p_ + 11); }
    Side side() const { return static_cast<Side>(p_[19]); }
    Shares shares() const { return load_be32(p_ + 20); }
    std::string_view symbol() const { return read_symbol(p_ + 24); }
    Price price() const { return load_be32(p_ + 32); }
};

// 'F' is 'A' with a trailing 4-byte market participant id; the shared fields are
// at identical offsets, so it reuses AddOrder's accessors.
class AddOrderMPID : public AddOrder {
public:
    using AddOrder::AddOrder;
    std::string_view mpid() const {
        return std::string_view(reinterpret_cast<const char*>(p_ + 36), 4);
    }
};

class OrderExecuted : public MessageView {
public:
    using MessageView::MessageView;
    OrderRef order_ref() const { return load_be64(p_ + 11); }
    Shares executed_shares() const { return load_be32(p_ + 19); }
    std::uint64_t match_number() const { return load_be64(p_ + 23); }
};

// 'C' reports an execution at a price other than the order's display price. The
// order still leaves the book at its own price; only the print differs.
class OrderExecutedPrice : public OrderExecuted {
public:
    using OrderExecuted::OrderExecuted;
    bool printable() const { return static_cast<char>(p_[31]) == 'Y'; }
    Price execution_price() const { return load_be32(p_ + 32); }
};

class OrderCancel : public MessageView {
public:
    using MessageView::MessageView;
    OrderRef order_ref() const { return load_be64(p_ + 11); }
    Shares cancelled_shares() const { return load_be32(p_ + 19); }
};

class OrderDelete : public MessageView {
public:
    using MessageView::MessageView;
    OrderRef order_ref() const { return load_be64(p_ + 11); }
};

// Replace removes the original order and adds a new one with a new reference. The
// new order loses the original's queue priority.
class OrderReplace : public MessageView {
public:
    using MessageView::MessageView;
    OrderRef original_order_ref() const { return load_be64(p_ + 11); }
    OrderRef new_order_ref() const { return load_be64(p_ + 19); }
    Shares shares() const { return load_be32(p_ + 27); }
    Price price() const { return load_be32(p_ + 31); }
};

// Non-cross trade of non-displayable interest. It never touched the visible book,
// so it must not be applied to it; it exists to complete the trade tape.
class Trade : public MessageView {
public:
    using MessageView::MessageView;
    Side side() const { return static_cast<Side>(p_[19]); }
    Shares shares() const { return load_be32(p_ + 20); }
    std::string_view symbol() const { return read_symbol(p_ + 24); }
    Price price() const { return load_be32(p_ + 32); }
    std::uint64_t match_number() const { return load_be64(p_ + 36); }
};

// Opening, closing and halt crosses. Share count is 8 bytes here, unlike every
// other message, where it is 4.
class CrossTrade : public MessageView {
public:
    using MessageView::MessageView;
    std::uint64_t shares() const { return load_be64(p_ + 11); }
    std::string_view symbol() const { return read_symbol(p_ + 19); }
    Price cross_price() const { return load_be32(p_ + 27); }
    std::uint64_t match_number() const { return load_be64(p_ + 31); }
    char cross_type() const { return static_cast<char>(p_[39]); }
};

}  // namespace itch
