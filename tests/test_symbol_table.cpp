#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "itch/symbol_table.h"

namespace {

std::vector<std::byte> make_directory(std::uint16_t locate, const char* symbol,
                                      char market_category = 'Q',
                                      std::uint32_t round_lot = 100) {
    std::vector<std::byte> msg;
    msg.push_back(static_cast<std::byte>('R'));
    msg.push_back(static_cast<std::byte>(locate >> 8));
    msg.push_back(static_cast<std::byte>(locate & 0xFF));
    msg.resize(11, std::byte{0});

    int i = 0;
    for (; symbol[i] && i < 8; ++i) msg.push_back(static_cast<std::byte>(symbol[i]));
    for (; i < 8; ++i) msg.push_back(static_cast<std::byte>(' '));

    msg.push_back(static_cast<std::byte>(market_category));
    msg.push_back(static_cast<std::byte>('N'));
    for (int shift = 24; shift >= 0; shift -= 8) {
        msg.push_back(static_cast<std::byte>((round_lot >> shift) & 0xFF));
    }
    msg.resize(itch::expected_length('R'), std::byte{0});
    return msg;
}

}  // namespace

TEST_CASE("locates resolve to symbols after their directory message") {
    itch::SymbolTable table;
    CHECK(table.size() == 0);
    CHECK(table.contains(7) == false);
    CHECK(table.symbol(7).empty());

    const auto aapl = make_directory(7, "AAPL");
    table.add(itch::StockDirectory(aapl.data()));

    CHECK(table.size() == 1);
    CHECK(table.contains(7));
    CHECK(table.symbol(7) == "AAPL");
    CHECK(table.entry(7).round_lot_size == 100);
    CHECK(table.entry(7).market_category == 'Q');
}

TEST_CASE("padding is trimmed and short symbols keep their length") {
    itch::SymbolTable table;
    const auto single = make_directory(1, "A");
    table.add(itch::StockDirectory(single.data()));

    CHECK(table.symbol(1) == "A");
    CHECK(table.symbol(1).size() == 1);
}

TEST_CASE("symbols may contain punctuation and fill the field") {
    itch::SymbolTable table;
    const auto test_issue = make_directory(9000, "ATEST.A");
    const auto full = make_directory(9001, "ABCDEFGH");
    table.add(itch::StockDirectory(test_issue.data()));
    table.add(itch::StockDirectory(full.data()));

    CHECK(table.symbol(9000) == "ATEST.A");
    CHECK(table.symbol(9001) == "ABCDEFGH");
    CHECK(table.symbol(9001).size() == 8);
}

TEST_CASE("the whole locate domain is addressable without resizing") {
    itch::SymbolTable table;
    const auto highest = make_directory(65535, "ZZZZ");
    table.add(itch::StockDirectory(highest.data()));

    CHECK(table.symbol(65535) == "ZZZZ");
    CHECK(table.contains(0) == false);
    CHECK(table.size() == 1);
}

TEST_CASE("re-registering a locate overwrites rather than double counting") {
    itch::SymbolTable table;
    const auto first = make_directory(42, "OLD");
    const auto second = make_directory(42, "NEW", 'N', 1);
    table.add(itch::StockDirectory(first.data()));
    table.add(itch::StockDirectory(second.data()));

    CHECK(table.size() == 1);
    CHECK(table.symbol(42) == "NEW");
    CHECK(table.entry(42).round_lot_size == 1);
}
