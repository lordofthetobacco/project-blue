// Bit packing of the wire protocol: layout, primitive encodings and their
// rejection rules (docs/netcode/protocol.md sections 2 and 3).

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec3.hpp>

#include "net/bit_stream.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

// Returns the bytes `write` produces in a 32-byte buffer, padded.
template <typename WriteFn>
size_t WriteInto(std::array<uint8_t, 32>& buffer, WriteFn write) {
  buffer.fill(0xFF);
  BitWriter writer(buffer);
  write(writer);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  return size.value();
}

TEST_CASE("bits are written least significant first") {
  std::array<uint8_t, 32> buffer{};
  const size_t size = WriteInto(buffer, [](BitWriter& w) {
    w.WriteBits(5, 3);
    w.WriteBits(1, 1);
    w.WriteBits(0xAB, 8);
  });
  REQUIRE(size == 2);
  CHECK(buffer[0] == 0xBD);
  CHECK(buffer[1] == 0x0A);

  BitReader reader(std::span<const uint8_t>(buffer.data(), size));
  CHECK(reader.ReadBits(3) == 5);
  CHECK(reader.ReadBits(1) == 1);
  CHECK(reader.ReadBits(8) == 0xAB);
  CHECK(reader.Finish().ok());
}

TEST_CASE("BitsRequired matches 32 - clz") {
  CHECK(BitsRequired(0) == 0);
  CHECK(BitsRequired(1) == 1);
  CHECK(BitsRequired(63) == 6);
  CHECK(BitsRequired(64) == 7);
  CHECK(BitsRequired(UINT32_MAX) == 32);
}

TEST_CASE("primitive round trips at their boundaries") {
  std::array<uint8_t, 32> buffer{};
  const size_t size = WriteInto(buffer, [](BitWriter& w) {
    w.WriteBool(true);
    w.WriteU8(255);
    w.WriteU16(0xFFFF);
    w.WriteI16(-32768);
    w.WriteU32(0xFFFFFFFFu);
    w.WriteU64(0x0123456789ABCDEFULL);
    w.WriteBoundedU32(7, 0, 7);
    w.WriteBoundedI32(-1, -1, 7);
    w.WriteBoundedI32(INT32_MIN, INT32_MIN, INT32_MAX);
    w.WriteBoundedU32(5, 5, 5);  // zero bits
  });
  BitReader reader(std::span<const uint8_t>(buffer.data(), size));
  CHECK(reader.ReadBool());
  CHECK(reader.ReadU8() == 255);
  CHECK(reader.ReadU16() == 0xFFFF);
  CHECK(reader.ReadI16() == -32768);
  CHECK(reader.ReadU32() == 0xFFFFFFFFu);
  CHECK(reader.ReadU64() == 0x0123456789ABCDEFULL);
  CHECK(reader.ReadBoundedU32(0, 7) == 7);
  CHECK(reader.ReadBoundedI32(-1, 7) == -1);
  CHECK(reader.ReadBoundedI32(INT32_MIN, INT32_MAX) == INT32_MIN);
  CHECK(reader.ReadBoundedU32(5, 5) == 5);
  CHECK(reader.Finish().ok());
}

TEST_CASE("varuint and varint round trip and use minimal groups") {
  for (const uint32_t value :
       {0u, 1u, 63u, 64u, 4095u, 4096u, UINT32_MAX >> 1, UINT32_MAX}) {
    std::array<uint8_t, 32> buffer{};
    const size_t size =
        WriteInto(buffer, [value](BitWriter& w) { w.WriteVarUint(value); });
    CHECK(size == (VarUintBits(value) + 7) / 8);
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    CHECK(reader.ReadVarUint() == value);
    CHECK(reader.Finish().ok());
  }
  CHECK(VarUintBits(0) == 7);
  CHECK(VarUintBits(63) == 7);
  CHECK(VarUintBits(64) == 14);
  CHECK(VarUintBits(UINT32_MAX) == 42);
  for (const int32_t value : {0, -1, 1, -2, INT32_MIN, INT32_MAX}) {
    CHECK(ZigZagDecode(ZigZagEncode(value)) == value);
    std::array<uint8_t, 32> buffer{};
    const size_t size =
        WriteInto(buffer, [value](BitWriter& w) { w.WriteVarInt(value); });
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    CHECK(reader.ReadVarInt() == value);
  }
  CHECK(ZigZagEncode(-1) == 1);
  CHECK(ZigZagEncode(1) == 2);
}

TEST_CASE("varuint decoding rejects overlong and oversized forms") {
  SUBCASE("a trailing zero group is non-canonical") {
    std::array<uint8_t, 32> buffer{};
    const size_t size = WriteInto(buffer, [](BitWriter& w) {
      w.WriteBits(1, 6);
      w.WriteBool(true);
      w.WriteBits(0, 6);
      w.WriteBool(false);
    });
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    reader.ReadVarUint();
    CHECK(reader.error() == WireError::kNonCanonical);
  }
  SUBCASE("a sixth group above 3 is out of range") {
    std::array<uint8_t, 32> buffer{};
    const size_t size = WriteInto(buffer, [](BitWriter& w) {
      for (int i = 0; i < 5; ++i) {
        w.WriteBits(63, 6);
        w.WriteBool(true);
      }
      w.WriteBits(4, 6);
      w.WriteBool(false);
    });
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    reader.ReadVarUint();
    CHECK(reader.error() == WireError::kOutOfRange);
  }
  SUBCASE("a sixth group that continues is out of range") {
    std::array<uint8_t, 32> buffer{};
    const size_t size = WriteInto(buffer, [](BitWriter& w) {
      for (int i = 0; i < 6; ++i) {
        w.WriteBits(1, 6);
        w.WriteBool(true);
      }
    });
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    reader.ReadVarUint();
    CHECK(reader.error() == WireError::kOutOfRange);
  }
}

TEST_CASE("bounded values outside their range are refused and rejected") {
  std::array<uint8_t, 32> buffer{};
  BitWriter writer(buffer);
  writer.WriteBoundedU32(8, 0, 7);
  CHECK(writer.error() == WireError::kOutOfRange);

  BitWriter usage(buffer);
  usage.WriteBoundedI32(0, 5, 4);
  CHECK(usage.error() == WireError::kUsage);

  // bounded(0, 1022) uses 10 bits; 1023 is a raw value above the range.
  const size_t size =
      WriteInto(buffer, [](BitWriter& w) { w.WriteBits(1023, 10); });
  BitReader reader(std::span<const uint8_t>(buffer.data(), size));
  reader.ReadBoundedU32(0, 1022);
  CHECK(reader.error() == WireError::kOutOfRange);
}

TEST_CASE("floats must be finite in both directions") {
  std::array<uint8_t, 32> buffer{};
  for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
    BitWriter writer(buffer);
    writer.WriteF32(bad);
    CHECK(writer.error() == WireError::kOutOfRange);

    const size_t size = WriteInto(buffer, [bad](BitWriter& w) {
      w.WriteU32(std::bit_cast<uint32_t>(bad));
    });
    BitReader reader(std::span<const uint8_t>(buffer.data(), size));
    reader.ReadF32();
    CHECK(reader.error() == WireError::kOutOfRange);
  }
  const size_t size = WriteInto(buffer, [](BitWriter& w) {
    w.WriteVec3(glm::vec3(-0.0f, std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::denorm_min()));
  });
  BitReader reader(std::span<const uint8_t>(buffer.data(), size));
  const glm::vec3 v = reader.ReadVec3();
  CHECK(std::signbit(v.x));
  CHECK(v.y == std::numeric_limits<float>::max());
  CHECK(v.z == std::numeric_limits<float>::denorm_min());
}

TEST_CASE("strings are bounded and UTF-8") {
  std::array<uint8_t, 64> buffer{};
  BitWriter writer(buffer);
  writer.WriteString("Zo\xC3\xAB", 32);
  writer.WriteString("", 32);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());

  BitReader reader(std::span<const uint8_t>(buffer.data(), size.value()));
  std::array<char, 32> text{};
  const size_t length = reader.ReadString(text);
  CHECK(std::string_view(text.data(), length) == "Zo\xC3\xAB");
  CHECK(reader.ReadString(text) == 0);
  CHECK(reader.Finish().ok());

  BitWriter too_long(buffer);
  too_long.WriteString("abcd", 3);
  CHECK(too_long.error() == WireError::kOutOfRange);

  BitWriter invalid(buffer);
  invalid.WriteString("\xC3", 32);
  CHECK(invalid.error() == WireError::kInvalidUtf8);

  CHECK(IsValidUtf8(std::span<const uint8_t>()));
  const std::array<uint8_t, 2> overlong = {0xC0, 0x80};
  CHECK_FALSE(IsValidUtf8(overlong));
  const std::array<uint8_t, 3> surrogate = {0xED, 0xA0, 0x80};
  CHECK_FALSE(IsValidUtf8(surrogate));
  const std::array<uint8_t, 4> too_high = {0xF4, 0x90, 0x80, 0x80};
  CHECK_FALSE(IsValidUtf8(too_high));
  const std::array<uint8_t, 4> emoji = {0xF0, 0x9F, 0x98, 0x80};
  CHECK(IsValidUtf8(emoji));
}

TEST_CASE("reading past the end is truncation and the failure is sticky") {
  const std::array<uint8_t, 1> data = {0xFF};
  BitReader reader(data);
  CHECK(reader.ReadBits(4) == 0xF);
  CHECK(reader.ReadBits(5) == 0);
  CHECK(reader.error() == WireError::kTruncated);
  CHECK(reader.ReadBits(1) == 0);
  CHECK(reader.error() == WireError::kTruncated);
  CHECK(reader.Finish().error() == WireError::kTruncated);
}

TEST_CASE("the end check rejects whole bytes and set padding bits") {
  const std::array<uint8_t, 2> two = {0x01, 0x00};
  BitReader whole_byte(two);
  whole_byte.ReadBits(1);
  CHECK(whole_byte.Finish().error() == WireError::kTrailingData);

  const std::array<uint8_t, 1> padded = {0x81};
  BitReader set_padding(padded);
  set_padding.ReadBits(1);
  CHECK(set_padding.Finish().error() == WireError::kTrailingData);

  const std::array<uint8_t, 1> clean = {0x01};
  BitReader ok(clean);
  ok.ReadBits(1);
  CHECK(ok.Finish().ok());
}

TEST_CASE("the writer respects its limit and can rewind") {
  std::array<uint8_t, 2> buffer{};
  BitWriter writer(buffer);
  writer.set_limit(10);
  writer.WriteBits(0x3FF, 10);
  CHECK(writer.ok());
  writer.WriteBits(1, 1);
  CHECK(writer.error() == WireError::kOverflow);
  writer.Rewind(4);
  CHECK(writer.ok());
  CHECK(writer.position() == 4);
  writer.WriteBits(0, 4);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  CHECK(size.value() == 1);
  CHECK(buffer[0] == 0x0F);

  BitWriter bad_value(buffer);
  bad_value.WriteBits(4, 2);
  CHECK(bad_value.error() == WireError::kOutOfRange);
}

TEST_CASE("optional u32 round trips") {
  std::array<uint8_t, 32> buffer{};
  const size_t size = WriteInto(buffer, [](BitWriter& w) {
    w.WriteOptionalU32(std::nullopt);
    w.WriteOptionalU32(0xDEADBEEFu);
  });
  BitReader reader(std::span<const uint8_t>(buffer.data(), size));
  CHECK(reader.ReadOptionalU32() == std::nullopt);
  CHECK(reader.ReadOptionalU32() == 0xDEADBEEFu);
  CHECK(reader.Finish().ok());
}

}  // namespace
}  // namespace blue::net
