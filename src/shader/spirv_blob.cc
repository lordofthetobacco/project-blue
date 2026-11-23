#include "shader/spirv_blob.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace blue::shader {

namespace {

// SPIR-V 1.6, section 2.3 (module layout) and 3 (binary form).
constexpr uint32_t kMagic = 0x07230203;
constexpr size_t kHeaderWords = 5;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kDecorationInvariant = 18;
constexpr uint32_t kBuiltInPosition = 0;

constexpr uint32_t Opcode(uint32_t word) { return word & 0xFFFFu; }
constexpr uint32_t WordCount(uint32_t word) { return word >> 16; }
constexpr uint32_t FirstWord(uint32_t opcode, uint32_t word_count) {
  return (word_count << 16) | opcode;
}

}  // namespace

std::vector<uint32_t> ToSpirvWords(std::span<const std::byte> bytes) {
  std::vector<uint32_t> words;
  if (bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) {
    return words;
  }
  words.resize(bytes.size() / sizeof(uint32_t));
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

bool MarkPositionInvariant(std::vector<uint32_t>& spirv) {
  if (spirv.size() < kHeaderWords || spirv[0] != kMagic) {
    return false;
  }
  // The decoration to add, right after the Position decoration it mirrors
  // (decorations may come in any order within the annotation section).
  std::array<uint32_t, 4> invariant{};
  size_t invariant_words = 0;
  size_t insert_at = 0;

  size_t at = kHeaderWords;
  while (at < spirv.size()) {
    const uint32_t count = WordCount(spirv[at]);
    if (count == 0 || at + count > spirv.size()) {
      return false;  // Malformed.
    }
    const uint32_t opcode = Opcode(spirv[at]);
    // OpDecorate <target> BuiltIn Position
    if (opcode == kOpDecorate && count == 4 &&
        spirv[at + 2] == kDecorationBuiltIn &&
        spirv[at + 3] == kBuiltInPosition) {
      invariant = {FirstWord(kOpDecorate, 3), spirv[at + 1],
                   kDecorationInvariant, 0};
      invariant_words = 3;
      insert_at = at + count;
    }
    // OpMemberDecorate <struct> <member> BuiltIn Position
    if (opcode == kOpMemberDecorate && count == 5 &&
        spirv[at + 3] == kDecorationBuiltIn &&
        spirv[at + 4] == kBuiltInPosition) {
      invariant = {FirstWord(kOpMemberDecorate, 4), spirv[at + 1],
                   spirv[at + 2], kDecorationInvariant};
      invariant_words = 4;
      insert_at = at + count;
    }
    at += count;
  }
  if (invariant_words == 0) {
    return false;
  }

  // Already there (the function is idempotent)?
  for (at = kHeaderWords; at < spirv.size(); at += WordCount(spirv[at])) {
    bool same = WordCount(spirv[at]) == invariant_words;
    for (size_t i = 0; same && i < invariant_words; ++i) {
      same = spirv[at + i] == invariant[i];
    }
    if (same) {
      return true;
    }
  }
  spirv.insert(spirv.begin() + static_cast<ptrdiff_t>(insert_at),
               invariant.begin(),
               invariant.begin() + static_cast<ptrdiff_t>(invariant_words));
  return true;
}

}  // namespace blue::shader
