// UserCommand lists on the wire: full and delta encodings, canonical
// angles and the decoder's canonicality checks (docs/netcode/protocol.md
// section 8).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "net/bit_stream.h"
#include "net/quantize.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"
#include "sim/user_command.h"

namespace blue::net {
namespace {

namespace held = sim::buttons::held;
namespace pressed = sim::buttons::pressed;

sim::UserCommand Command(uint32_t tick) {
  sim::UserCommand command;
  command.tick = tick;
  command.held = held::kForward | held::kSprint;
  command.yaw = 1.5f;
  command.pitch = -0.25f;
  return command;
}

CommandList RoundTrip(const CommandList& list) {
  std::array<uint8_t, 512> buffer{};
  BitWriter writer(buffer);
  WriteCommandList(list, writer);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  BitReader reader(std::span<const uint8_t>(buffer.data(), size.value()));
  const CommandList decoded = ReadCommandList(reader);
  REQUIRE(reader.Finish().ok());
  return decoded;
}

TEST_CASE("Canonical quantises angles and clears unused interaction ids") {
  sim::UserCommand command = Command(5);
  command.yaw = -std::numbers::pi_v<float> / 2.0f;
  command.pitch = 3.0f;
  command.interact_id = 77;  // kind none: not sent
  const sim::UserCommand canonical = Canonical(command);
  CHECK(canonical.yaw == DequantizeYaw(3u << 18, kFineAngleBits));
  CHECK(canonical.pitch == DequantizePitch(1 << 18, kFineAngleBits));
  CHECK(canonical.interact_id == 0);
  CHECK(Canonical(canonical) == canonical);
}

TEST_CASE("command lists round trip to their canonical form") {
  std::vector<sim::UserCommand> commands;
  for (uint32_t i = 0; i < kMaxCommandsPerPacket; ++i) {
    sim::UserCommand c = Command(1u - i);  // wraps through tick 0
    c.held = i % 3 == 0 ? 0xFF : held::kForward;
    c.pressed = i == 2 ? pressed::kJump | pressed::kZeroDown : 0;
    c.yaw = i == 4 ? 6.2831f : 0.001f * static_cast<float>(i);  // wraps
    c.pitch = i == 5 ? -1.6f : 0.25f;
    c.select_slot = i == 6 ? int8_t{7} : int8_t{-1};
    c.interact_kind = i == 7 ? interact_kind::kMax : interact_kind::kNone;
    c.interact_id = i == 7 ? UINT32_MAX : 0;
    commands.push_back(c);
  }
  WireResult<CommandList> list = CommandList::FromCommands(commands);
  REQUIRE(list.ok());
  CHECK(RoundTrip(list.value()) == list.value().Canonical());
}

TEST_CASE("command lists validate their shape") {
  CHECK(CommandList::FromCommands({}).error() == WireError::kUsage);
  const std::array<sim::UserCommand, 2> gap = {Command(10), Command(8)};
  CHECK(CommandList::FromCommands(gap).error() == WireError::kUsage);
  CommandList full;
  for (uint32_t i = 0; i < kMaxCommandsPerPacket; ++i) {
    REQUIRE(full.Push(Command(100 - i)).ok());
  }
  CHECK(full.Push(Command(100 - kMaxCommandsPerPacket)).error() ==
        WireError::kUsage);

  std::array<uint8_t, 64> buffer{};
  BitWriter empty(buffer);
  WriteCommandList(CommandList(), empty);
  CHECK(empty.error() == WireError::kUsage);

  sim::UserCommand wide = Command(1);
  wide.held = 0x100;
  BitWriter wide_writer(buffer);
  WriteCommandList(CommandList::FromCommands({&wide, 1}).value(), wide_writer);
  CHECK(wide_writer.error() == WireError::kOutOfRange);

  sim::UserCommand bad_slot = Command(1);
  bad_slot.select_slot = 8;
  BitWriter slot_writer(buffer);
  WriteCommandList(CommandList::FromCommands({&bad_slot, 1}).value(),
                   slot_writer);
  CHECK(slot_writer.error() == WireError::kOutOfRange);
}

// Writes a two-command list whose delta sets the bit named by
// `changed_field` (0 held .. 4 actions) and then writes `body` verbatim.
// Returns the decode error, or nullopt if the list decodes.
template <typename BodyFn>
std::optional<WireError> DecodeDeltaError(int changed_field, BodyFn body) {
  std::array<uint8_t, 64> buffer{};
  BitWriter writer(buffer);
  writer.WriteBoundedU32(2, 1, kMaxCommandsPerPacket);
  // Newest command in full: tick 9, held 1, pressed 0, yaw 0, pitch 0,
  // no slot, no interaction.
  writer.WriteU32(9);
  writer.WriteBits(1, 8);
  writer.WriteBits(0, 8);
  writer.WriteBits(0, kFineAngleBits);
  writer.WriteBoundedI32(0, -PitchLimit(kFineAngleBits),
                         PitchLimit(kFineAngleBits));
  writer.WriteBool(false);
  writer.WriteBoundedU32(0, 0, interact_kind::kMax);
  for (int field = 0; field < 5; ++field) {
    writer.WriteBool(field == changed_field);
    if (field == changed_field) {
      body(writer);
    }
  }
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  BitReader reader(std::span<const uint8_t>(buffer.data(), size.value()));
  ReadCommandList(reader);
  const WireResult<void> finished = reader.Finish();
  if (finished.ok()) {
    return std::nullopt;
  }
  return finished.error();
}

TEST_CASE("delta decoding rejects unchanged and out-of-range values") {
  CHECK(DecodeDeltaError(0, [](BitWriter& w) { w.WriteBits(1, 8); }) ==
        WireError::kNonCanonical);
  CHECK(DecodeDeltaError(1, [](BitWriter& w) { w.WriteBits(0, 8); }) ==
        WireError::kNonCanonical);
  CHECK(DecodeDeltaError(2, [](BitWriter& w) { w.WriteVarInt(0); }) ==
        WireError::kNonCanonical);
  CHECK(DecodeDeltaError(2, [](BitWriter& w) { w.WriteVarInt(1 << 19); }) ==
        WireError::kOutOfRange);
  CHECK(DecodeDeltaError(3, [](BitWriter& w) { w.WriteVarInt(0); }) ==
        WireError::kNonCanonical);
  CHECK(DecodeDeltaError(3, [](BitWriter& w) {
          w.WriteVarInt(PitchLimit(kFineAngleBits) + 1);
        }) == WireError::kOutOfRange);
  CHECK(DecodeDeltaError(4, [](BitWriter& w) {
          w.WriteBool(false);
          w.WriteBoundedU32(0, 0, interact_kind::kMax);
        }) == WireError::kNonCanonical);
  // A valid delta for comparison: yaw -1 wraps to the top of the turn.
  CHECK(DecodeDeltaError(2, [](BitWriter& w) { w.WriteVarInt(-1); }) ==
        std::nullopt);
}

}  // namespace
}  // namespace blue::net
