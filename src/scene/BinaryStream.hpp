#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blue {

// Little-endian primitives assembled byte by byte, so the on-disk layout is
// independent of host endianness and struct padding.

[[nodiscard]] constexpr uint32_t makeFourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
[[nodiscard]] uint32_t crc32(std::span<const std::byte> data);

class BinaryWriter {
public:
    void writeU32(uint32_t value);
    void writeF32(float value);
    void writeBytes(std::span<const std::byte> data);
    // u32 byte length followed by the bytes, no terminator.
    void writeString(std::string_view value);

    // Chunks are length-prefixed; the size is patched once the body is written.
    [[nodiscard]] size_t beginChunk(uint32_t fourcc);
    void endChunk(size_t chunkStart);

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

enum class ReadStatus { Ok, Truncated, LimitExceeded };

// Bounds-checked cursor. The first failure is sticky: later reads return zero
// values, so callers can read a whole record and check status() once.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] uint32_t readU32();
    [[nodiscard]] float readF32();
    // Fails with LimitExceeded when the declared length exceeds maxLength.
    [[nodiscard]] std::string readString(uint32_t maxLength);
    [[nodiscard]] std::span<const std::byte> readBytes(size_t count);

    [[nodiscard]] size_t remaining() const { return data_.size() - position_; }
    [[nodiscard]] ReadStatus status() const { return status_; }

private:
    [[nodiscard]] bool require(size_t count);

    std::span<const std::byte> data_;
    size_t position_ = 0;
    ReadStatus status_ = ReadStatus::Ok;
};

} // namespace blue
