#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "itch/messages.h"

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (unsigned v : vals) out.push_back(static_cast<std::byte>(v));
    return out;
}

// Header shared by every message: type, locate, tracking, 6-byte timestamp.
void append_header(std::vector<std::byte>& msg, char type, std::uint16_t locate,
                   std::uint64_t timestamp) {
    msg.push_back(static_cast<std::byte>(type));
    msg.push_back(static_cast<std::byte>(locate >> 8));
    msg.push_back(static_cast<std::byte>(locate & 0xFF));
    msg.push_back(std::byte{0});
    msg.push_back(std::byte{0});
    for (int shift = 40; shift >= 0; shift -= 8) {
        msg.push_back(static_cast<std::byte>((timestamp >> shift) & 0xFF));
    }
}

void append_be(std::vector<std::byte>& msg, std::uint64_t value, int width) {
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
        msg.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void append_symbol(std::vector<std::byte>& msg, const char* symbol) {
    int i = 0;
    for (; symbol[i] && i < 8; ++i) msg.push_back(static_cast<std::byte>(symbol[i]));
    for (; i < 8; ++i) msg.push_back(static_cast<std::byte>(' '));
}

}  // namespace

TEST_CASE("system event decodes the real first message of 07302019") {
    const auto msg = bytes_of({0x53, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFA, 0x4D, 0x3A, 0xD2, 0x6B,
                               0x4F});
    REQUIRE(msg.size() == itch::expected_length('S'));

    const itch::SystemEvent ev(msg.data());
    CHECK(ev.type() == 'S');
    CHECK(ev.locate() == 0);
    CHECK(ev.timestamp() == 10970642174571ull);
    CHECK(ev.event_code() == 'O');
}

TEST_CASE("add order decodes every field") {
    std::vector<std::byte> msg;
    append_header(msg, 'A', 494, 14400001000000ull);
    append_be(msg, 8617, 8);
    msg.push_back(static_cast<std::byte>('B'));
    append_be(msg, 600, 4);
    append_symbol(msg, "ARGX");
    append_be(msg, 1409900, 4);

    REQUIRE(msg.size() == itch::expected_length('A'));

    const itch::AddOrder add(msg.data());
    CHECK(add.locate() == 494);
    CHECK(add.timestamp() == 14400001000000ull);
    CHECK(add.order_ref() == 8617);
    CHECK(add.side() == itch::Side::Buy);
    CHECK(add.shares() == 600);
    CHECK(add.symbol() == "ARGX");
    CHECK(add.price() == 1409900);
}

TEST_CASE("symbols are space-padded, not null-terminated") {
    std::vector<std::byte> msg;
    append_header(msg, 'A', 1, 0);
    append_be(msg, 1, 8);
    msg.push_back(static_cast<std::byte>('S'));
    append_be(msg, 100, 4);
    append_symbol(msg, "A");
    append_be(msg, 10000, 4);

    const itch::AddOrder add(msg.data());
    CHECK(add.symbol() == "A");
    CHECK(add.symbol().size() == 1);
}

TEST_CASE("add order with MPID shares add order's offsets") {
    std::vector<std::byte> msg;
    append_header(msg, 'F', 7, 0);
    append_be(msg, 42, 8);
    msg.push_back(static_cast<std::byte>('S'));
    append_be(msg, 250, 4);
    append_symbol(msg, "AAPL");
    append_be(msg, 1500000, 4);
    for (char c : {'N', 'S', 'D', 'Q'}) msg.push_back(static_cast<std::byte>(c));

    REQUIRE(msg.size() == itch::expected_length('F'));

    const itch::AddOrderMPID add(msg.data());
    CHECK(add.order_ref() == 42);
    CHECK(add.symbol() == "AAPL");
    CHECK(add.price() == 1500000);
    CHECK(add.mpid() == "NSDQ");
}

TEST_CASE("execute, cancel and delete carry only a reference") {
    std::vector<std::byte> exec;
    append_header(exec, 'E', 7, 0);
    append_be(exec, 1001, 8);
    append_be(exec, 100, 4);
    append_be(exec, 555, 8);
    REQUIRE(exec.size() == itch::expected_length('E'));

    const itch::OrderExecuted e(exec.data());
    CHECK(e.order_ref() == 1001);
    CHECK(e.executed_shares() == 100);
    CHECK(e.match_number() == 555);

    std::vector<std::byte> cancel;
    append_header(cancel, 'X', 7, 0);
    append_be(cancel, 1002, 8);
    append_be(cancel, 50, 4);
    REQUIRE(cancel.size() == itch::expected_length('X'));

    const itch::OrderCancel x(cancel.data());
    CHECK(x.order_ref() == 1002);
    CHECK(x.cancelled_shares() == 50);

    std::vector<std::byte> del;
    append_header(del, 'D', 7, 0);
    append_be(del, 1003, 8);
    REQUIRE(del.size() == itch::expected_length('D'));

    CHECK(itch::OrderDelete(del.data()).order_ref() == 1003);
}

TEST_CASE("execute with price exposes both the order and the print price") {
    std::vector<std::byte> msg;
    append_header(msg, 'C', 7, 0);
    append_be(msg, 2001, 8);
    append_be(msg, 300, 4);
    append_be(msg, 999, 8);
    msg.push_back(static_cast<std::byte>('N'));
    append_be(msg, 1499900, 4);

    REQUIRE(msg.size() == itch::expected_length('C'));

    const itch::OrderExecutedPrice c(msg.data());
    CHECK(c.order_ref() == 2001);
    CHECK(c.executed_shares() == 300);
    CHECK(c.printable() == false);
    CHECK(c.execution_price() == 1499900);
}

TEST_CASE("replace carries both the old and new reference") {
    std::vector<std::byte> msg;
    append_header(msg, 'U', 7, 0);
    append_be(msg, 3001, 8);
    append_be(msg, 3002, 8);
    append_be(msg, 700, 4);
    append_be(msg, 1502500, 4);

    REQUIRE(msg.size() == itch::expected_length('U'));

    const itch::OrderReplace u(msg.data());
    CHECK(u.original_order_ref() == 3001);
    CHECK(u.new_order_ref() == 3002);
    CHECK(u.shares() == 700);
    CHECK(u.price() == 1502500);
}

TEST_CASE("cross trade carries an 8-byte share count") {
    std::vector<std::byte> msg;
    append_header(msg, 'Q', 7, 0);
    append_be(msg, 5'000'000'000ull, 8);
    append_symbol(msg, "AAPL");
    append_be(msg, 1500000, 4);
    append_be(msg, 777, 8);
    msg.push_back(static_cast<std::byte>('O'));

    REQUIRE(msg.size() == itch::expected_length('Q'));

    const itch::CrossTrade q(msg.data());
    CHECK(q.shares() == 5'000'000'000ull);
    CHECK(q.symbol() == "AAPL");
    CHECK(q.cross_price() == 1500000);
    CHECK(q.cross_type() == 'O');
}

TEST_CASE("prices are integers scaled by four decimal places") {
    CHECK(itch::kPriceScale == 10000);

    const itch::Price wire = 1500000;
    CHECK(wire / itch::kPriceScale == 150);
    CHECK(wire % itch::kPriceScale == 0);
}
