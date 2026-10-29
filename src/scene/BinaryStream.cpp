#include "scene/BinaryStream.hpp"

#include <array>

namespace blue {

namespace {

constexpr std::array<uint32_t, 256> makeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? (value >> 1) ^ 0xEDB88320U : value >> 1;
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = makeCrcTable();

} // namespace

uint32_t crc32(std::span<const std::byte> data)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        crc = kCrcTable[(crc ^ std::to_integer<uint32_t>(b)) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

void BinaryWriter::writeU32(uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void BinaryWriter::writeF32(float value)
{
    writeU32(std::bit_cast<uint32_t>(value));
}

void BinaryWriter::writeBytes(std::span<const std::byte> data)
{
    bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void BinaryWriter::writeString(std::string_view value)
{
    writeU32(static_cast<uint32_t>(value.size()));
    writeBytes(std::as_bytes(std::span(value)));
}

size_t BinaryWriter::beginChunk(uint32_t fourcc)
{
    const size_t start = bytes_.size();
    writeU32(fourcc);
    writeU32(0);
    return start;
}

void BinaryWriter::endChunk(size_t chunkStart)
{
    constexpr size_t kChunkHeaderSize = 8;
    const auto size = static_cast<uint32_t>(bytes_.size() - chunkStart - kChunkHeaderSize);
    for (int i = 0; i < 4; ++i) {
        bytes_[chunkStart + 4 + static_cast<size_t>(i)] = static_cast<std::byte>((size >> (8 * i)) & 0xFFU);
    }
}

bool BinaryReader::require(size_t count)
{
    if (status_ != ReadStatus::Ok) {
        return false;
    }
    if (count > remaining()) {
        status_ = ReadStatus::Truncated;
        return false;
    }
    return true;
}

uint32_t BinaryReader::readU32()
{
    if (!require(4)) {
        return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<uint32_t>(data_[position_ + i]) << (8 * i);
    }
    position_ += 4;
    return value;
}

float BinaryReader::readF32()
{
    return std::bit_cast<float>(readU32());
}

std::string BinaryReader::readString(uint32_t maxLength)
{
    const uint32_t length = readU32();
    if (status_ != ReadStatus::Ok) {
        return {};
    }
    if (length > maxLength) {
        status_ = ReadStatus::LimitExceeded;
        return {};
    }
    const std::span<const std::byte> body = readBytes(length);
    return {reinterpret_cast<const char*>(body.data()), body.size()};
}

std::span<const std::byte> BinaryReader::readBytes(size_t count)
{
    if (!require(count)) {
        return {};
    }
    const std::span<const std::byte> result = data_.subspan(position_, count);
    position_ += count;
    return result;
}

} // namespace blue
