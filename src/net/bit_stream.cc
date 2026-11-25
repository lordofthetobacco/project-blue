#include "net/bit_stream.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec3.hpp>

#include "net/wire_error.h"

namespace blue::net {
namespace {

constexpr int kVarUintGroupBits = 6;
constexpr uint32_t kVarUintGroupMask = 0x3F;
constexpr int kVarUintMaxGroups = 6;
// The 6th group holds bits 30 and 31 only.
constexpr uint32_t kVarUintLastGroupMax = 0x3;

// Returns true if `value` fits in `bits` bits (0..32).
bool FitsInBits(uint32_t value, int bits) {
  return bits >= 32 || (value >> bits) == 0;
}

// Returns the length of the UTF-8 sequence starting with `lead`, or 0 for a
// byte that cannot start one.
int Utf8SequenceLength(uint8_t lead) {
  if (lead < 0x80) {
    return 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    return 2;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    return 4;
  }
  return 0;
}

// Returns true if `second` may follow `lead` (the extra restrictions on the
// second byte rule out overlong forms, surrogates and code points above
// U+10FFFF).
bool ValidSecondByte(uint8_t lead, uint8_t second) {
  switch (lead) {
    case 0xE0:
      return second >= 0xA0 && second <= 0xBF;
    case 0xED:
      return second >= 0x80 && second <= 0x9F;
    case 0xF0:
      return second >= 0x90 && second <= 0xBF;
    case 0xF4:
      return second >= 0x80 && second <= 0x8F;
    default:
      return second >= 0x80 && second <= 0xBF;
  }
}

}  // namespace

int BitsRequired(uint32_t range) { return std::bit_width(range); }

size_t VarUintBits(uint32_t value) {
  const int groups =
      value == 0
          ? 1
          : (BitsRequired(value) + kVarUintGroupBits - 1) / kVarUintGroupBits;
  return static_cast<size_t>(groups) * (kVarUintGroupBits + 1);
}

uint32_t ZigZagEncode(int32_t value) {
  return (static_cast<uint32_t>(value) << 1) ^
         static_cast<uint32_t>(value >> 31);
}

int32_t ZigZagDecode(uint32_t value) {
  return static_cast<int32_t>(value >> 1) ^ -static_cast<int32_t>(value & 1);
}

bool IsValidUtf8(std::span<const uint8_t> bytes) {
  size_t i = 0;
  while (i < bytes.size()) {
    const uint8_t lead = bytes[i];
    const int length = Utf8SequenceLength(lead);
    if (length == 0 || bytes.size() - i < static_cast<size_t>(length)) {
      return false;
    }
    if (length > 1 && !ValidSecondByte(lead, bytes[i + 1])) {
      return false;
    }
    for (int k = 2; k < length; ++k) {
      const uint8_t continuation = bytes[i + static_cast<size_t>(k)];
      if (continuation < 0x80 || continuation > 0xBF) {
        return false;
      }
    }
    i += static_cast<size_t>(length);
  }
  return true;
}

// --- BitWriter ---------------------------------------------------------------

BitWriter::BitWriter(std::span<uint8_t> buffer)
    : buffer_(buffer), limit_(buffer.size() * 8) {}

void BitWriter::WriteBits(uint32_t value, int bits) {
  if (!ok()) {
    return;
  }
  if (bits < 0 || bits > 32 || !FitsInBits(value, bits)) {
    Fail(WireError::kOutOfRange);
    return;
  }
  if (position_ + static_cast<size_t>(bits) > limit_) {
    Fail(WireError::kOverflow);
    return;
  }
  uint64_t rest = value;
  int remaining = bits;
  while (remaining > 0) {
    const size_t index = position_ / 8;
    const int offset = static_cast<int>(position_ % 8);
    const int take = std::min(8 - offset, remaining);
    const uint32_t mask = (1u << take) - 1;
    const uint32_t chunk = static_cast<uint32_t>(rest) & mask;
    const uint32_t old = buffer_[index];
    buffer_[index] =
        static_cast<uint8_t>((old & ~(mask << offset)) | (chunk << offset));
    rest >>= take;
    remaining -= take;
    position_ += static_cast<size_t>(take);
  }
}

void BitWriter::WriteBool(bool value) { WriteBits(value ? 1 : 0, 1); }

void BitWriter::WriteU8(uint8_t value) { WriteBits(value, 8); }

void BitWriter::WriteU16(uint16_t value) { WriteBits(value, 16); }

void BitWriter::WriteI16(int16_t value) {
  WriteU16(static_cast<uint16_t>(value));
}

void BitWriter::WriteU32(uint32_t value) { WriteBits(value, 32); }

void BitWriter::WriteU64(uint64_t value) {
  WriteU32(static_cast<uint32_t>(value & 0xFFFFFFFFu));
  WriteU32(static_cast<uint32_t>(value >> 32));
}

void BitWriter::WriteBoundedU32(uint32_t value, uint32_t min, uint32_t max) {
  if (min > max) {
    Fail(WireError::kUsage);
    return;
  }
  if (value < min || value > max) {
    Fail(WireError::kOutOfRange);
    return;
  }
  WriteBits(value - min, BitsRequired(max - min));
}

void BitWriter::WriteBoundedI32(int32_t value, int32_t min, int32_t max) {
  if (min > max) {
    Fail(WireError::kUsage);
    return;
  }
  if (value < min || value > max) {
    Fail(WireError::kOutOfRange);
    return;
  }
  const auto range = static_cast<uint32_t>(int64_t{max} - int64_t{min});
  const auto raw = static_cast<uint32_t>(int64_t{value} - int64_t{min});
  WriteBits(raw, BitsRequired(range));
}

void BitWriter::WriteVarUint(uint32_t value) {
  uint32_t rest = value;
  while (true) {
    const uint32_t group = rest & kVarUintGroupMask;
    rest >>= kVarUintGroupBits;
    const bool more = rest != 0;
    WriteBits(group, kVarUintGroupBits);
    WriteBool(more);
    if (!more) {
      return;
    }
  }
}

void BitWriter::WriteVarInt(int32_t value) {
  WriteVarUint(ZigZagEncode(value));
}

void BitWriter::WriteF32(float value) {
  if (!std::isfinite(value)) {
    Fail(WireError::kOutOfRange);
    return;
  }
  WriteU32(std::bit_cast<uint32_t>(value));
}

void BitWriter::WriteVec3(const glm::vec3& value) {
  WriteF32(value.x);
  WriteF32(value.y);
  WriteF32(value.z);
}

void BitWriter::WriteOptionalU32(const std::optional<uint32_t>& value) {
  WriteBool(value.has_value());
  if (value.has_value()) {
    WriteU32(*value);
  }
}

void BitWriter::WriteString(std::string_view text, size_t max_bytes) {
  if (text.size() > max_bytes || max_bytes > UINT32_MAX) {
    Fail(WireError::kOutOfRange);
    return;
  }
  const std::span<const uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  if (!IsValidUtf8(bytes)) {
    Fail(WireError::kInvalidUtf8);
    return;
  }
  WriteBoundedU32(static_cast<uint32_t>(text.size()), 0,
                  static_cast<uint32_t>(max_bytes));
  for (const uint8_t byte : bytes) {
    WriteU8(byte);
  }
}

void BitWriter::Fail(WireError error) {
  if (!error_.has_value()) {
    error_ = error;
  }
}

void BitWriter::set_limit(size_t limit_bits) {
  limit_ = std::min(limit_bits, buffer_.size() * 8);
}

size_t BitWriter::bits_left() const {
  return limit_ > position_ ? limit_ - position_ : 0;
}

void BitWriter::Rewind(size_t position) {
  position_ = std::min(position_, position);
  error_.reset();
}

WireResult<size_t> BitWriter::Finish() {
  if (error_.has_value()) {
    return WireResult<size_t>::Error(*error_);
  }
  const int padding = static_cast<int>((8 - position_ % 8) % 8);
  limit_ = buffer_.size() * 8;
  WriteBits(0, padding);
  if (error_.has_value()) {
    return WireResult<size_t>::Error(*error_);
  }
  return WireResult<size_t>::Ok(position_ / 8);
}

// --- BitReader ---------------------------------------------------------------

BitReader::BitReader(std::span<const uint8_t> data) : data_(data) {}

uint32_t BitReader::ReadBits(int bits) {
  if (!ok()) {
    return 0;
  }
  if (bits < 0 || bits > 32) {
    Fail(WireError::kUsage);
    return 0;
  }
  if (static_cast<size_t>(bits) > bits_left()) {
    Fail(WireError::kTruncated);
    return 0;
  }
  uint64_t value = 0;
  int shift = 0;
  int remaining = bits;
  while (remaining > 0) {
    const size_t index = position_ / 8;
    const int offset = static_cast<int>(position_ % 8);
    const int take = std::min(8 - offset, remaining);
    const uint32_t mask = (1u << take) - 1;
    const uint32_t chunk = (uint32_t{data_[index]} >> offset) & mask;
    value |= uint64_t{chunk} << shift;
    shift += take;
    remaining -= take;
    position_ += static_cast<size_t>(take);
  }
  return static_cast<uint32_t>(value);
}

bool BitReader::ReadBool() { return ReadBits(1) != 0; }

uint8_t BitReader::ReadU8() { return static_cast<uint8_t>(ReadBits(8)); }

uint16_t BitReader::ReadU16() { return static_cast<uint16_t>(ReadBits(16)); }

int16_t BitReader::ReadI16() { return static_cast<int16_t>(ReadU16()); }

uint32_t BitReader::ReadU32() { return ReadBits(32); }

uint64_t BitReader::ReadU64() {
  const uint64_t low = ReadU32();
  const uint64_t high = ReadU32();
  return low | (high << 32);
}

uint32_t BitReader::ReadBoundedU32(uint32_t min, uint32_t max) {
  if (min > max) {
    Fail(WireError::kUsage);
    return min;
  }
  const uint32_t range = max - min;
  const uint32_t raw = ReadBits(BitsRequired(range));
  if (raw > range) {
    Fail(WireError::kOutOfRange);
    return min;
  }
  return min + raw;
}

int32_t BitReader::ReadBoundedI32(int32_t min, int32_t max) {
  if (min > max) {
    Fail(WireError::kUsage);
    return min;
  }
  const auto range = static_cast<uint32_t>(int64_t{max} - int64_t{min});
  const uint32_t raw = ReadBits(BitsRequired(range));
  if (raw > range) {
    Fail(WireError::kOutOfRange);
    return min;
  }
  return static_cast<int32_t>(int64_t{min} + int64_t{raw});
}

uint32_t BitReader::ReadVarUint() {
  uint32_t result = 0;
  for (int index = 0; index < kVarUintMaxGroups; ++index) {
    const uint32_t group = ReadBits(kVarUintGroupBits);
    const bool more = ReadBool();
    if (!ok()) {
      return 0;
    }
    if (index == kVarUintMaxGroups - 1 &&
        (group > kVarUintLastGroupMax || more)) {
      Fail(WireError::kOutOfRange);
      return 0;
    }
    result |= group << (kVarUintGroupBits * index);
    if (!more) {
      if (index > 0 && group == 0) {
        Fail(WireError::kNonCanonical);
        return 0;
      }
      return result;
    }
  }
  // Unreachable: the 6th group either ends the value or fails above.
  Fail(WireError::kOutOfRange);
  return 0;
}

int32_t BitReader::ReadVarInt() { return ZigZagDecode(ReadVarUint()); }

float BitReader::ReadF32() {
  const float value = std::bit_cast<float>(ReadU32());
  if (!std::isfinite(value)) {
    Fail(WireError::kOutOfRange);
    return 0.0f;
  }
  return value;
}

glm::vec3 BitReader::ReadVec3() {
  const float x = ReadF32();
  const float y = ReadF32();
  const float z = ReadF32();
  return glm::vec3(x, y, z);
}

std::optional<uint32_t> BitReader::ReadOptionalU32() {
  if (!ReadBool()) {
    return std::nullopt;
  }
  return ReadU32();
}

size_t BitReader::ReadString(std::span<char> out) {
  if (out.size() > UINT32_MAX) {
    Fail(WireError::kUsage);
    return 0;
  }
  const uint32_t length = ReadBoundedU32(0, static_cast<uint32_t>(out.size()));
  for (uint32_t i = 0; i < length; ++i) {
    out[i] = static_cast<char>(ReadU8());
  }
  if (!ok()) {
    return 0;
  }
  const std::span<const uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(out.data()), length);
  if (!IsValidUtf8(bytes)) {
    Fail(WireError::kInvalidUtf8);
    return 0;
  }
  return length;
}

void BitReader::Fail(WireError error) {
  if (!error_.has_value()) {
    error_ = error;
  }
}

WireResult<void> BitReader::Finish() {
  if (error_.has_value()) {
    return WireResult<void>::Error(*error_);
  }
  const size_t left = bits_left();
  if (left >= 8) {
    return WireResult<void>::Error(WireError::kTrailingData);
  }
  if (ReadBits(static_cast<int>(left)) != 0) {
    return WireResult<void>::Error(WireError::kTrailingData);
  }
  return WireResult<void>::Ok();
}

}  // namespace blue::net
