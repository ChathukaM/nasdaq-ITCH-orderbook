#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "book/handler.h"
#include "itch/symbol_table.h"

namespace {

struct MessageBuilder {
    std::vector<std::byte> bytes;

    explicit MessageBuilder(char type, std::uint16_t locate = 7) {
        bytes.push_back(static_cast<std::byte>(type));
        bytes.push_back(static_cast<std::byte>(locate >> 8));
        bytes.push_back(static_cast<std::byte>(locate & 0xFF));
        bytes.resize(11, std::byte{0});
    }

    MessageBuilder& be(std::uint64_t value, int width) {
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }
        return *this;
    }

    MessageBuilder& ch(char c) {
        bytes.push_back(static_cast<std::byte>(c));
        return *this;
    }

    MessageBuilder& symbol(const char* s) {
        int i = 0;
        for (; s[i] && i < 8; ++i) bytes.push_back(static_cast<std::byte>(s[i]));
        for (; i < 8; ++i) bytes.push_back(static_cast<std::byte>(' '));
        return *this;
    }

    const std::byte* data() const { return bytes.data(); }
};

std::vector<std::byte> add_order(std::uint64_t ref, char side, std::uint32_t shares,
                                 std::uint32_t price, std::uint16_t locate = 7) {
    MessageBuilder m('A', locate);
    m.be(ref, 8).ch(side).be(shares, 4).symbol("TEST").be(price, 4);
    return m.bytes;
}

std::vector<std::byte> execute(std::uint64_t ref, std::uint32_t shares) {
    MessageBuilder m('E');
    m.be(ref, 8).be(shares, 4).be(1, 8);
    return m.bytes;
}

std::vector<std::byte> cancel(std::uint64_t ref, std::uint32_t shares) {
    MessageBuilder m('X');
    m.be(ref, 8).be(shares, 4);
    return m.bytes;
}

std::vector<std::byte> del(std::uint64_t ref) {
    MessageBuilder m('D');
    m.be(ref, 8);
    return m.bytes;
}

std::vector<std::byte> replace(std::uint64_t old_ref, std::uint64_t new_ref,
                               std::uint32_t shares, std::uint32_t price) {
    MessageBuilder m('U');
    m.be(old_ref, 8).be(new_ref, 8).be(shares, 4).be(price, 4);
    return m.bytes;
}

const itch::SymbolTable kSymbols;

}  // namespace

TEST_CASE("adds accumulate into a single price level") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'B', 100, 1500000);
    const auto b = add_order(1002, 'B', 200, 1500000);
    h.apply(a.data());
    h.apply(b.data());

    const auto& book = h.book(7);
    REQUIRE(book.has_bid());
    CHECK(book.best_bid() == 1500000);
    CHECK(book.best_bid_level().shares == 300);
    CHECK(book.best_bid_level().order_count == 2);
    CHECK(h.live_orders() == 2);
}

TEST_CASE("best bid is the highest and best ask the lowest") {
    itch::BookHandler h(kSymbols);
    for (const auto& msg : {add_order(1, 'B', 100, 1499800), add_order(2, 'B', 100, 1500000),
                            add_order(3, 'S', 100, 1500500), add_order(4, 'S', 100, 1501000)}) {
        h.apply(msg.data());
    }

    const auto& book = h.book(7);
    CHECK(book.best_bid() == 1500000);
    CHECK(book.best_ask() == 1500500);
    CHECK(book.crossed() == false);
}

TEST_CASE("a full execution removes the order and empties the level") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'B', 100, 1500000);
    const auto e = execute(1001, 100);
    h.apply(a.data());
    h.apply(e.data());

    CHECK(h.book(7).has_bid() == false);
    CHECK(h.live_orders() == 0);
    CHECK(h.stats().executed_shares == 100);
}

TEST_CASE("a partial execution leaves the order resting with fewer shares") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'B', 500, 1500000);
    const auto e = execute(1001, 200);
    h.apply(a.data());
    h.apply(e.data());

    const auto& book = h.book(7);
    REQUIRE(book.has_bid());
    CHECK(book.best_bid_level().shares == 300);
    CHECK(book.best_bid_level().order_count == 1);
    CHECK(h.live_orders() == 1);
}

TEST_CASE("a partial cancel reduces the level without removing the order") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'S', 500, 1500000);
    const auto x = cancel(1001, 150);
    h.apply(a.data());
    h.apply(x.data());

    CHECK(h.book(7).best_ask_level().shares == 350);
    CHECK(h.book(7).best_ask_level().order_count == 1);
    CHECK(h.live_orders() == 1);
}

TEST_CASE("delete removes the whole order") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'B', 100, 1500000);
    const auto b = add_order(1002, 'B', 200, 1500000);
    const auto d = del(1001);
    h.apply(a.data());
    h.apply(b.data());
    h.apply(d.data());

    CHECK(h.book(7).best_bid_level().shares == 200);
    CHECK(h.book(7).best_bid_level().order_count == 1);
    CHECK(h.live_orders() == 1);
}

TEST_CASE("replace inherits side and locate from the original order") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'S', 100, 1500000, 42);
    const auto u = replace(1001, 1002, 300, 1502000);
    h.apply(a.data());
    h.apply(u.data());

    const auto& book = h.book(42);
    CHECK(book.has_ask());
    CHECK(book.best_ask() == 1502000);
    CHECK(book.best_ask_level().shares == 300);
    CHECK(book.has_bid() == false);
    CHECK(h.live_orders() == 1);
    CHECK(h.stats().orders_replaced == 1);
}

TEST_CASE("messages naming an unknown order are counted, not applied") {
    itch::BookHandler h(kSymbols);
    const auto e = execute(9999, 100);
    const auto d = del(8888);
    h.apply(e.data());
    h.apply(d.data());

    CHECK(h.stats().unknown_order_refs == 2);
    CHECK(h.live_orders() == 0);
    CHECK(h.book(7).has_bid() == false);
}

TEST_CASE("hidden and cross trades are tallied but never touch the book") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1001, 'B', 100, 1500000);
    h.apply(a.data());

    MessageBuilder trade('P');
    trade.be(0, 8).ch('B').be(500, 4).symbol("TEST").be(1500000, 4).be(1, 8);
    h.apply(trade.data());

    MessageBuilder cross('Q');
    cross.be(1000, 8).symbol("TEST").be(1500000, 4).be(1, 8).ch('O');
    h.apply(cross.data());

    CHECK(h.stats().hidden_trades == 1);
    CHECK(h.stats().cross_trades == 1);
    CHECK(h.book(7).best_bid_level().shares == 100);
    CHECK(h.book(7).best_bid_level().order_count == 1);
}

TEST_CASE("a level reappears correctly after being emptied") {
    itch::BookHandler h(kSymbols);
    const auto a = add_order(1, 'B', 100, 1500000);
    const auto d = del(1);
    const auto b = add_order(2, 'B', 400, 1500000);
    h.apply(a.data());
    h.apply(d.data());
    CHECK(h.book(7).has_bid() == false);

    h.apply(b.data());
    CHECK(h.book(7).best_bid_level().shares == 400);
    CHECK(h.book(7).best_bid_level().order_count == 1);
}
