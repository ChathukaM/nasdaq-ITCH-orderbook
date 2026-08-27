#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <vector>

#include "itch/reader.hpp"
#include "platform.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 1;
    }

    try {
        itch::MappedFile file(argv[1]);
        itch::MessageReader reader(file);

        std::array<std::uint64_t, 256> counts{};
        std::uint64_t total = 0;

        const auto start = std::chrono::steady_clock::now();

        const std::byte* payload = nullptr;
        std::uint16_t length = 0;
        while (reader.next(payload, length)) {
            counts[static_cast<unsigned char>(payload[0])]++;
            ++total;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const std::size_t consumed = reader.bytes_consumed(file.data());

        std::printf("file:      %s\n", argv[1]);
        std::printf("size:      %.2f GB\n", double(file.size()) / 1e9);
        std::printf("messages:  %llu\n", static_cast<unsigned long long>(total));
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

        std::printf("\nelapsed:   %.2f s\n", seconds);
        std::printf("rate:      %.2f M msg/s   %.2f GB/s\n",
                    double(total) / seconds / 1e6, double(consumed) / seconds / 1e9);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
