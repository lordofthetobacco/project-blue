#include "scene/binary_stream.h"

#include <array>
#include <bit>

namespace blue::scene {
namespace {

constexpr std::array<uint32_t, 256> MakeCrcTable() {
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

constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

}  // namespace

uint32_t Crc32(std::span<const std::byte> data) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const std::byte b : data) {
    crc = kCrcTable[(crc ^ std::to_integer<uint32_t>(b)) & 0xFFU] ^ (crc >> 8);
  }
  return ~crc;
}

void BinaryWriter::WriteU32(uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void BinaryWriter::WriteF32(float value) {
  WriteU32(std::bit_cast<uint32_t>(value));
}

void BinaryWriter::WriteBytes(std::span<const std::byte> data) {
  bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void BinaryWriter::WriteString(std::string_view value) {
  WriteU32(static_cast<uint32_t>(value.size()));
  WriteBytes(std::as_bytes(std::span(value)));
}

size_t BinaryWriter::BeginChunk(uint32_t fourcc) {
  const size_t start = bytes_.size();
  WriteU32(fourcc);
  WriteU32(0);
  return start;
}

void BinaryWriter::EndChunk(size_t chunk_start) {
  constexpr size_t kChunkHeaderSize = 8;
  const auto size =
      static_cast<uint32_t>(bytes_.size() - chunk_start - kChunkHeaderSize);
  for (int i = 0; i < 4; ++i) {
    bytes_[chunk_start + 4 + static_cast<size_t>(i)] =
        static_cast<std::byte>((size >> (8 * i)) & 0xFFU);
  }
}

bool BinaryReader::Require(size_t count) {
  if (status_ != ReadStatus::kOk) {
    return false;
  }
  if (count > remaining()) {
    status_ = ReadStatus::kTruncated;
    return false;
  }
  return true;
}

uint32_t BinaryReader::ReadU32() {
  if (!Require(4)) {
    return 0;
  }
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<uint32_t>(data_[position_ + i]) << (8 * i);
  }
  position_ += 4;
  return value;
}

float BinaryReader::ReadF32() { return std::bit_cast<float>(ReadU32()); }

std::string BinaryReader::ReadString(uint32_t max_length) {
  const uint32_t length = ReadU32();
  if (status_ != ReadStatus::kOk) {
    return {};
  }
  if (length > max_length) {
    status_ = ReadStatus::kLimitExceeded;
    return {};
  }
  const std::span<const std::byte> body = ReadBytes(length);
  std::string text;
  text.reserve(body.size());
  for (const std::byte b : body) {
    text.push_back(static_cast<char>(b));
  }
  return text;
}

std::span<const std::byte> BinaryReader::ReadBytes(size_t count) {
  if (!Require(count)) {
    return {};
  }
  const std::span<const std::byte> result = data_.subspan(position_, count);
  position_ += count;
  return result;
}

}  // namespace blue::scene
