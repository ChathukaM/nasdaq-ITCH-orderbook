#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#error "Native Windows is not supported. Build under WSL2, or add a CreateFileMapping backend here."
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itch {

static_assert(std::endian::native == std::endian::little,
              "load_be* helpers assume a little-endian host");

inline std::uint16_t load_be16(const std::byte* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline std::uint32_t load_be32(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline std::uint64_t load_be64(const std::byte* p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

// ITCH timestamps are 6 bytes. Landing them in the top 6 bytes of a u64 means the
// same byte-reversal instruction yields the value, with no shifting afterwards.
inline std::uint64_t load_be48(const std::byte* p) {
    std::uint64_t v = 0;
    std::memcpy(reinterpret_cast<std::byte*>(&v) + 2, p, 6);
    return __builtin_bswap64(v);
}

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
        }

        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("cannot stat " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);

        void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (addr == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("cannot mmap " + path + ": " + std::strerror(errno));
        }
        data_ = static_cast<const std::byte*>(addr);

        // The replay is a single forward pass, so the kernel can read ahead and drop
        // pages once passed. Measured as no change on macOS/APFS (70.4 vs 69.6 ns per
        // message, within run-to-run noise); kept because it is free and Linux honours
        // MADV_SEQUENTIAL more aggressively.
#if defined(MADV_SEQUENTIAL)
        ::madvise(addr, size_, MADV_SEQUENTIAL);
#endif
#if defined(MADV_WILLNEED)
        ::madvise(addr, size_, MADV_WILLNEED);
#endif
    }

    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::byte* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

}  // namespace itch
