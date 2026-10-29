#ifndef BLUE_SCENE_BINARY_STREAM_H_
#define BLUE_SCENE_BINARY_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Little-endian primitives assembled byte by byte, so the on-disk layout is
// independent of host endianness and struct padding.

namespace blue::scene {

// Returns the four characters packed into a little-endian chunk tag.
[[nodiscard]] constexpr uint32_t MakeFourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

// Returns the CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) of `data`.
[[nodiscard]] uint32_t Crc32(std::span<const std::byte> data);

// Appends little-endian values to an in-memory buffer. Copyable and movable;
// not thread-safe.
class BinaryWriter {
 public:
  BinaryWriter() = default;
  BinaryWriter(const BinaryWriter&) = default;
  BinaryWriter& operator=(const BinaryWriter&) = default;
  BinaryWriter(BinaryWriter&&) = default;
  BinaryWriter& operator=(BinaryWriter&&) = default;

  void WriteU32(uint32_t value);
  void WriteF32(float value);
  void WriteBytes(std::span<const std::byte> data);
  // Writes a u32 byte length followed by the bytes, no terminator.
  void WriteString(std::string_view value);

  // Starts a length-prefixed chunk tagged `fourcc` and returns its start
  // offset, to be passed to EndChunk() once the body is written.
  [[nodiscard]] size_t BeginChunk(uint32_t fourcc);
  // Patches the size of the chunk begun at `chunk_start` to cover everything
  // written since.
  void EndChunk(size_t chunk_start);

  // Returns the bytes written so far.
  [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

enum class ReadStatus {
  kOk,
  kTruncated,      // A read ran past the end of the data.
  kLimitExceeded,  // A declared length was larger than the caller allows.
};

// Bounds-checked cursor over a byte span that is not owned: the data must
// outlive the reader. The first failure is sticky: later reads return zero
// values, so callers can read a whole record and check status() once.
class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::byte> data) : data_(data) {}
  BinaryReader(const BinaryReader&) = default;
  BinaryReader& operator=(const BinaryReader&) = default;
  BinaryReader(BinaryReader&&) = default;
  BinaryReader& operator=(BinaryReader&&) = default;

  [[nodiscard]] uint32_t ReadU32();
  [[nodiscard]] float ReadF32();
  // Reads a length-prefixed string. Fails with kLimitExceeded when the
  // declared length exceeds `max_length`.
  [[nodiscard]] std::string ReadString(uint32_t max_length);
  // Returns a view of the next `count` bytes into the underlying data, or an
  // empty span (and a failed status) if fewer remain.
  [[nodiscard]] std::span<const std::byte> ReadBytes(size_t count);

  // Returns the number of unread bytes.
  [[nodiscard]] size_t remaining() const { return data_.size() - position_; }
  [[nodiscard]] ReadStatus status() const { return status_; }

 private:
  // Returns true if `count` more bytes can be read; otherwise records
  // kTruncated (unless already failed) and returns false.
  [[nodiscard]] bool Require(size_t count);

  std::span<const std::byte> data_;
  size_t position_ = 0;
  ReadStatus status_ = ReadStatus::kOk;
};

}  // namespace blue::scene

#endif  // BLUE_SCENE_BINARY_STREAM_H_
