#include <cstdio>
#include <exception>

#include "platform.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 1;
    }

    try {
        itch::MappedFile file(argv[1]);

        std::printf("file:  %s\n", argv[1]);
        std::printf("size:  %zu bytes\n", file.size());

        std::printf("head: ");
        for (std::size_t i = 0; i < 16 && i < file.size(); ++i) {
            std::printf(" %02x", static_cast<unsigned>(file.data()[i]));
        }
        std::printf("\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
