#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace {

constexpr std::byte b(unsigned v) { return static_cast<std::byte>(v); }

}  // namespace

TEST_CASE("big-endian loads decode wire byte order") {
    const std::byte wire[] = {b(0x01), b(0x2C), b(0xDE), b(0xAD),
                              b(0xBE), b(0xEF), b(0x12), b(0x34)};

    CHECK(itch::load_be16(wire) == 0x012Cu);
    CHECK(itch::load_be32(wire) == 0x012CDEADu);
    CHECK(itch::load_be64(wire) == 0x012CDEADBEEF1234ull);
}

TEST_CASE("load_be48 decodes a 6-byte ITCH timestamp") {
    // Timestamp of the first System Event message in 07302019 (03:02:50.642 ET).
    const std::byte timestamp[] = {b(0x09), b(0xFA), b(0x4D), b(0x3A), b(0xD2), b(0x6B)};

    CHECK(itch::load_be48(timestamp) == 10970642174571ull);
}

TEST_CASE("loads are correct at unaligned offsets") {
    alignas(8) std::byte buf[16] = {};

    buf[1] = b(0x01);
    buf[2] = b(0x2C);
    CHECK(itch::load_be16(buf + 1) == 0x012Cu);

    buf[3] = b(0xDE);
    buf[4] = b(0xAD);
    buf[5] = b(0xBE);
    buf[6] = b(0xEF);
    CHECK(itch::load_be32(buf + 3) == 0xDEADBEEFu);
}

TEST_CASE("prices are integers with four implied decimal places") {
    const std::byte on_the_wire[] = {b(0x00), b(0x16), b(0xE3), b(0x60)};

    CHECK(itch::load_be32(on_the_wire) == 1'500'000u);  // $150.0000
}

TEST_CASE("mapping a missing file throws rather than returning a null mapping") {
    CHECK_THROWS_AS(itch::MappedFile("/nonexistent/path/to/nothing"), std::runtime_error);
}
