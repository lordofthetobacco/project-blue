#ifndef BLUE_NET_BIT_STREAM_H_
#define BLUE_NET_BIT_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec3.hpp>

#include "net/wire_error.h"

namespace blue::net {

// Bit packing for the wire protocol (sections 2 and 3 of
// docs/netcode/protocol.md). Stream bit i is bit (i % 8) of byte i / 8, and
// an n-bit value is written least significant bit first: writing 5 in 3
// bits, 1 in 1 bit and 0xAB in 8 bits yields the bytes BD 0A.
//
// Both streams have a sticky failure: the first error is recorded, every
// later write is ignored and every later read returns zero, so a codec can
// run straight through and check ok() (or Finish()) once at the end. Neither
// stream allocates, and neither touches memory outside its span.

// Returns the number of bits needed for every integer in 0..=range
// (BitsRequired(0) == 0).
int BitsRequired(uint32_t range);

// Returns the number of 7-bit groups times 7 that WriteVarUint uses for
// `value`.
size_t VarUintBits(uint32_t value);

// Zig-zag maps a signed integer onto an unsigned one (0, -1, 1, -2, ... ->
// 0, 1, 2, 3, ...), and back.
uint32_t ZigZagEncode(int32_t value);
int32_t ZigZagDecode(uint32_t value);

// Returns true if `bytes` is well-formed UTF-8 (no overlong forms, no
// surrogates, nothing above U+10FFFF), exactly as Rust's str::from_utf8.
bool IsValidUtf8(std::span<const uint8_t> bytes);

// Writes bits into a caller-provided buffer. The buffer must outlive the
// writer. Copyable: a copy writes to the same buffer.
class BitWriter {
 public:
  explicit BitWriter(std::span<uint8_t> buffer);
  BitWriter(const BitWriter&) = default;
  BitWriter& operator=(const BitWriter&) = default;

  // Writes the low `bits` bits (0..32) of `value`. Fails with kOutOfRange if
  // `value` has higher bits set, kOverflow if the limit would be passed.
  void WriteBits(uint32_t value, int bits);
  void WriteBool(bool value);
  void WriteU8(uint8_t value);
  void WriteU16(uint16_t value);
  // 16 bits, two's complement.
  void WriteI16(int16_t value);
  void WriteU32(uint32_t value);
  // The low 32 bits, then the high 32 bits.
  void WriteU64(uint64_t value);
  // `value - min` in BitsRequired(max - min) bits. kOutOfRange outside
  // min..=max, kUsage if min > max.
  void WriteBoundedU32(uint32_t value, uint32_t min, uint32_t max);
  void WriteBoundedI32(int32_t value, int32_t min, int32_t max);
  // 6-bit groups, least significant first, each followed by a continuation
  // bit; the minimum number of groups.
  void WriteVarUint(uint32_t value);
  // ZigZagEncode, then WriteVarUint.
  void WriteVarInt(int32_t value);
  // The binary32 bit pattern. kOutOfRange for NaN and infinities.
  void WriteF32(float value);
  void WriteVec3(const glm::vec3& value);
  // optional<u32>: a presence bit, then the value if present.
  void WriteOptionalU32(const std::optional<uint32_t>& value);
  // Byte length as bounded(0, max_bytes), then the bytes. kOutOfRange if
  // longer than `max_bytes`, kInvalidUtf8 if not UTF-8.
  void WriteString(std::string_view text, size_t max_bytes);

  // Records `error` unless a failure is already recorded.
  void Fail(WireError error);

  bool ok() const { return !error_.has_value(); }
  // The first recorded failure, or nullopt.
  std::optional<WireError> error() const { return error_; }
  // Bits written so far.
  size_t position() const { return position_; }
  // The bit position writes may not pass.
  size_t limit() const { return limit_; }
  // Moves the limit, clamped to the buffer size.
  void set_limit(size_t limit_bits);
  // Bits left before the limit.
  size_t bits_left() const;

  // Moves back to `position` (positions ahead are ignored) and clears a
  // recorded failure. Used to drop an append that did not fit.
  void Rewind(size_t position);

  // Pads with zero bits to the next byte boundary (ignoring the limit, not
  // the buffer) and returns the bytes written, or the recorded failure.
  WireResult<size_t> Finish();

 private:
  std::span<uint8_t> buffer_;
  size_t position_ = 0;
  size_t limit_ = 0;
  std::optional<WireError> error_;
};

// Reads bits from a byte span. Every read is bounds checked; reading past the
// end records kTruncated. The data must outlive the reader.
class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> data);
  BitReader(const BitReader&) = default;
  BitReader& operator=(const BitReader&) = default;

  // Reads `bits` bits (0..32), LSB first. Returns 0 after a failure.
  uint32_t ReadBits(int bits);
  bool ReadBool();
  uint8_t ReadU8();
  uint16_t ReadU16();
  int16_t ReadI16();
  uint32_t ReadU32();
  uint64_t ReadU64();
  // Inverse of BitWriter::WriteBounded*: a raw value above max - min
  // records kOutOfRange (and returns min).
  uint32_t ReadBoundedU32(uint32_t min, uint32_t max);
  int32_t ReadBoundedI32(int32_t min, int32_t max);
  // At most 6 groups; a 6th group above 3 or continuing records
  // kOutOfRange, a final zero group that is not the first kNonCanonical.
  uint32_t ReadVarUint();
  int32_t ReadVarInt();
  // A non-finite value records kOutOfRange (and returns 0).
  float ReadF32();
  glm::vec3 ReadVec3();
  std::optional<uint32_t> ReadOptionalU32();
  // Reads a string of at most out.size() bytes into `out` and returns its
  // length. Records kInvalidUtf8 for malformed UTF-8.
  size_t ReadString(std::span<char> out);

  // Records `error` unless a failure is already recorded.
  void Fail(WireError error);

  bool ok() const { return !error_.has_value(); }
  std::optional<WireError> error() const { return error_; }
  size_t position() const { return position_; }
  size_t bits_left() const { return data_.size() * 8 - position_; }

  // Returns the recorded failure, or checks that only zero padding of the
  // last byte remains (kTrailingData otherwise).
  WireResult<void> Finish();

 private:
  std::span<const uint8_t> data_;
  size_t position_ = 0;
  std::optional<WireError> error_;
};

}  // namespace blue::net

#endif  // BLUE_NET_BIT_STREAM_H_
