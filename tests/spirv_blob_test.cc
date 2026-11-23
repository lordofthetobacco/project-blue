#include "shader/spirv_blob.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "platform/file_system.h"

namespace blue::shader {
namespace {

constexpr uint32_t kMagic = 0x07230203;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpTypeVoid = 19;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kDecorationInvariant = 18;
constexpr uint32_t kDecorationLocation = 30;

constexpr uint32_t Op(uint32_t opcode, uint32_t words) {
  return (words << 16) | opcode;
}

// A minimal module: the header, `annotations`, then one type instruction.
std::vector<uint32_t> Module(std::span<const uint32_t> annotations) {
  std::vector<uint32_t> spirv = {kMagic, 0x00010600, 0, 10, 0};
  spirv.insert(spirv.end(), annotations.begin(), annotations.end());
  spirv.push_back(Op(kOpTypeVoid, 2));
  spirv.push_back(1);
  return spirv;
}

// Counts the instructions of `spirv` equal to `instruction`.
int Count(const std::vector<uint32_t>& spirv,
          std::span<const uint32_t> instruction) {
  int count = 0;
  size_t at = 5;
  while (at < spirv.size()) {
    const uint32_t words = spirv[at] >> 16;
    if (words == 0) {
      break;
    }
    if (words == instruction.size() &&
        std::memcmp(&spirv[at], instruction.data(),
                    words * sizeof(uint32_t)) == 0) {
      ++count;
    }
    at += words;
  }
  return count;
}

TEST_CASE("ToSpirvWords converts whole words only") {
  const std::array<std::byte, 8> bytes = {
      std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  const std::vector<uint32_t> words = ToSpirvWords(bytes);
  REQUIRE(words.size() == 2);
  CHECK(words[0] == kMagic);
  CHECK(words[1] == 1);
  CHECK(ToSpirvWords(std::span(bytes).first(7)).empty());
  CHECK(ToSpirvWords({}).empty());
}

TEST_CASE("MarkPositionInvariant decorates the position variable once") {
  const std::array<uint32_t, 8> annotations = {
      Op(kOpDecorate, 4), 7, kDecorationLocation, 0,
      Op(kOpDecorate, 4), 9, kDecorationBuiltIn,  0 /* Position */};
  std::vector<uint32_t> spirv = Module(annotations);
  const size_t size = spirv.size();
  const std::array<uint32_t, 3> invariant = {Op(kOpDecorate, 3), 9,
                                             kDecorationInvariant};

  REQUIRE(MarkPositionInvariant(spirv));
  CHECK(spirv.size() == size + 3);
  CHECK(Count(spirv, invariant) == 1);
  // Right after the decoration it mirrors, before the types.
  CHECK(spirv[5 + 8] == invariant[0]);
  CHECK(spirv[5 + 9] == 9);
  CHECK(spirv.back() == 1);

  // Idempotent.
  REQUIRE(MarkPositionInvariant(spirv));
  CHECK(spirv.size() == size + 3);
  CHECK(Count(spirv, invariant) == 1);
}

TEST_CASE("MarkPositionInvariant decorates a position struct member") {
  const std::array<uint32_t, 5> annotations = {Op(kOpMemberDecorate, 5), 4, 2,
                                               kDecorationBuiltIn, 0};
  std::vector<uint32_t> spirv = Module(annotations);
  REQUIRE(MarkPositionInvariant(spirv));
  const std::array<uint32_t, 4> invariant = {Op(kOpMemberDecorate, 4), 4, 2,
                                             kDecorationInvariant};
  CHECK(Count(spirv, invariant) == 1);
}

TEST_CASE("MarkPositionInvariant leaves other modules alone") {
  // Another built-in (PointSize = 1), no Position.
  const std::array<uint32_t, 4> annotations = {Op(kOpDecorate, 4), 9,
                                               kDecorationBuiltIn, 1};
  std::vector<uint32_t> spirv = Module(annotations);
  const std::vector<uint32_t> before = spirv;
  CHECK_FALSE(MarkPositionInvariant(spirv));
  CHECK(spirv == before);

  std::vector<uint32_t> not_spirv = {1, 2, 3, 4, 5, 6};
  CHECK_FALSE(MarkPositionInvariant(not_spirv));
  std::vector<uint32_t> empty;
  CHECK_FALSE(MarkPositionInvariant(empty));
  // An instruction longer than the module.
  std::vector<uint32_t> truncated = {kMagic, 0x00010600, 0, 10, 0,
                                     Op(kOpDecorate, 4), 9};
  CHECK_FALSE(MarkPositionInvariant(truncated));
}

#if BLUE_SHADERS_HAVE_SPIRV
TEST_CASE("The build's vertex shaders take the Invariant decoration") {
  for (const char* name : {"mesh.vert.spv", "depth.vert.spv",
                           "motion_velocity.vert.spv", "shadow.vert.spv"}) {
    CAPTURE(name);
    const auto bytes = platform::ReadFile(
        std::string(BLUE_SHADER_BLOB_DIR) + "/spirv/" + name, size_t{4} << 20);
    REQUIRE(bytes.ok());
    std::vector<uint32_t> spirv = ToSpirvWords(bytes.value());
    REQUIRE_FALSE(spirv.empty());
    const size_t size = spirv.size();
    REQUIRE(MarkPositionInvariant(spirv));
    CHECK(spirv.size() == size + 3);
    REQUIRE(MarkPositionInvariant(spirv));
    CHECK(spirv.size() == size + 3);
  }
}
#endif

}  // namespace
}  // namespace blue::shader
