#include "net/replication.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "net/interpolation.h"
#include "net/messages.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/world_object.h"
#include "sim/player_state.h"

namespace blue::net {
namespace {

// Expected numbers of dropped weapons, moving bodies and taken pick-ups;
// more only grow the vectors.
constexpr size_t kReservedDropped = 64;
constexpr size_t kReservedBodies = 256;
constexpr size_t kReservedPickups = 64;

InterpSample SampleOf(uint32_t tick, const RemotePlayer& player) {
  return InterpSample{.tick = tick,
                      .position = player.position,
                      .velocity = player.velocity,
                      .yaw = player.yaw,
                      .pitch = player.pitch,
                      .flags = player.flags,
                      .health = player.health,
                      .weapon_key = player.weapon_key};
}

InterpSample SampleOf(uint32_t tick, const WorldObject& object) {
  return InterpSample{.tick = tick,
                      .position = object.position,
                      .velocity = object.linear_velocity,
                      .rotation = object.rotation,
                      .at_rest = object.at_rest};
}

}  // namespace

Replication::Replication() {
  dropped_.reserve(kReservedDropped);
  bodies_.reserve(kReservedBodies);
  taken_pickups_.reserve(kReservedPickups);
}

void Replication::Reset(std::optional<uint8_t> own_id) {
  own_id_ = own_id;
  message_counts_ = MessageCounts{};
  players_seen_ = 0;
  for (PlayerInfo& player : players_) {
    player = PlayerInfo{};
  }
  dropped_.clear();
  bodies_.clear();
  taken_pickups_.clear();
  world_synced_ = false;
  shots_.Clear();
  impacts_.Clear();
  kills_.Clear();
}

PlayerInfo* Replication::Find(uint8_t id) {
  return id < players_.size() ? &players_[id] : nullptr;
}

void Replication::AddDropped(const DroppedWeapon& weapon) {
  RemoveDropped(weapon.dropped_id);
  ReplicatedDroppedWeapon& added = dropped_.emplace_back();
  added.dropped_id = weapon.dropped_id;
  added.weapon_key = weapon.weapon_key;
  added.rounds_in_magazine = weapon.rounds_in_magazine;
  added.position = weapon.position;
  added.rotation = weapon.rotation;
}

void Replication::RemoveDropped(uint32_t dropped_id) {
  std::erase_if(dropped_, [dropped_id](const ReplicatedDroppedWeapon& w) {
    return w.dropped_id == dropped_id;
  });
}

void Replication::AddTakenPickup(uint32_t instance_id) {
  if (std::ranges::find(taken_pickups_, instance_id) == taken_pickups_.end()) {
    taken_pickups_.push_back(instance_id);
  }
}

void Replication::Count(const Message& message) {
  MessageCounts& c = message_counts_;
  switch (TypeOf(message)) {
    case MessageType::kPlayerJoined:
      ++c.joined;
      break;
    case MessageType::kPlayerLeft:
      ++c.left;
      break;
    case MessageType::kDamage:
      ++c.damage;
      break;
    case MessageType::kDeath:
      ++c.deaths;
      break;
    case MessageType::kRespawn:
      ++c.respawns;
      break;
    case MessageType::kShot:
      ++c.shots;
      break;
    case MessageType::kImpact:
      ++c.impacts;
      break;
    case MessageType::kWeaponDropped:
      ++c.drops;
      break;
    case MessageType::kDroppedTaken:
      ++c.dropped_taken;
      break;
    case MessageType::kDroppedRemoved:
      ++c.dropped_removed;
      break;
    case MessageType::kPickupTaken:
      ++c.pickups_taken;
      break;
    case MessageType::kWorldSyncPickupsTaken:
    case MessageType::kWorldSyncDroppedWeapon:
    case MessageType::kWorldSyncDone:
      ++c.world_sync;
      break;
    case MessageType::kClientHello:
    case MessageType::kServerWelcome:
    case MessageType::kServerReject:
      break;
  }
}

void Replication::Apply(const Message& message, double now_seconds) {
  Count(message);
  if (const auto* joined = std::get_if<PlayerJoined>(&message)) {
    if (PlayerInfo* player = Find(joined->player_id)) {
      *player = PlayerInfo{};
      player->joined = true;
      player->name = joined->name;
      player->health = sim::kMaxHealth;
    }
  } else if (const auto* left = std::get_if<PlayerLeft>(&message)) {
    if (PlayerInfo* player = Find(left->player_id)) {
      *player = PlayerInfo{};
    }
  } else if (const auto* damage = std::get_if<Damage>(&message)) {
    if (PlayerInfo* player = Find(damage->victim)) {
      player->health = damage->health_after;
    }
  } else if (const auto* death = std::get_if<Death>(&message)) {
    KillRecord record{.killer = death->killer,
                      .victim = death->victim,
                      .weapon_key = death->weapon_key,
                      .time_seconds = now_seconds};
    if (PlayerInfo* victim = Find(death->victim)) {
      ++victim->deaths;
      victim->health = 0;
      record.victim_name = victim->name;
    }
    if (death->killer.has_value() && *death->killer != death->victim) {
      if (PlayerInfo* killer = Find(*death->killer)) {
        ++killer->kills;
        record.killer_name = killer->name;
      }
    }
    kills_.Push(record);
  } else if (const auto* respawn = std::get_if<Respawn>(&message)) {
    if (PlayerInfo* player = Find(respawn->player_id)) {
      player->health = sim::kMaxHealth;
      // The jump to the spawn point must not be interpolated.
      player->motion.Clear();
      player->has_pose = false;
    }
  } else if (const auto* shot = std::get_if<Shot>(&message)) {
    if (shot->shooter != own_id_) {
      shots_.Push(ShotRecord{.shooter = shot->shooter,
                             .origin = shot->origin,
                             .direction = shot->direction,
                             .weapon_key = shot->weapon_key,
                             .time_seconds = now_seconds});
    }
  } else if (const auto* impact = std::get_if<Impact>(&message)) {
    impacts_.Push(ImpactRecord{.point = impact->point,
                               .normal = impact->normal,
                               .time_seconds = now_seconds});
  } else if (const auto* dropped = std::get_if<WeaponDropped>(&message)) {
    AddDropped(dropped->weapon);
  } else if (const auto* synced =
                 std::get_if<WorldSyncDroppedWeapon>(&message)) {
    AddDropped(synced->weapon);
  } else if (const auto* taken = std::get_if<DroppedTaken>(&message)) {
    RemoveDropped(taken->dropped_id);
  } else if (const auto* removed = std::get_if<DroppedRemoved>(&message)) {
    RemoveDropped(removed->dropped_id);
  } else if (const auto* pickup = std::get_if<PickupTaken>(&message)) {
    AddTakenPickup(pickup->instance_id);
  } else if (const auto* pickups =
                 std::get_if<WorldSyncPickupsTaken>(&message)) {
    const size_t count =
        std::min<size_t>(pickups->count, pickups->instance_ids.size());
    for (size_t i = 0; i < count; ++i) {
      AddTakenPickup(pickups->instance_ids[i]);
    }
  } else if (std::holds_alternative<WorldSyncDone>(message)) {
    world_synced_ = true;
  }
  // Handshake messages never come through the reliable channel.
}

void Replication::ApplyPlayers(uint32_t server_tick, const PlayerSet& players) {
  for (size_t id = 0; id < players_.size(); ++id) {
    PlayerInfo& info = players_[id];
    const auto player_id = static_cast<uint8_t>(id);
    if (own_id_ == player_id) {
      continue;
    }
    const RemotePlayerQ* state = players.Find(player_id);
    if (state == nullptr) {
      info.motion.Clear();
      info.has_pose = false;
      continue;
    }
    info.motion.Push(SampleOf(server_tick, Dequantize(*state)));
    players_seen_ |= uint64_t{1} << id;
  }
}

void Replication::ApplyObjects(uint32_t server_tick,
                               std::span<const WorldObject> objects) {
  for (const WorldObject& object : objects) {
    const InterpSample sample = SampleOf(server_tick, object);
    if (object.kind == ObjectKind::kDroppedWeapon) {
      auto it = std::ranges::find(dropped_, object.id,
                                  &ReplicatedDroppedWeapon::dropped_id);
      if (it != dropped_.end()) {
        it->motion.Push(sample);
      }
      continue;
    }
    auto it =
        std::ranges::find(bodies_, object.id, &ReplicatedBody::instance_id);
    if (it == bodies_.end()) {
      ReplicatedBody& added = bodies_.emplace_back();
      added.instance_id = object.id;
      added.motion.Push(sample);
    } else {
      it->motion.Push(sample);
    }
  }
}

void Replication::Sample(ViewTime time, double max_extrapolation_ticks) {
  for (PlayerInfo& player : players_) {
    const std::optional<InterpSample> pose =
        player.motion.Sample(time, max_extrapolation_ticks);
    player.has_pose = pose.has_value();
    if (pose.has_value()) {
      player.pose = *pose;
    }
  }
  for (ReplicatedDroppedWeapon& weapon : dropped_) {
    if (const std::optional<InterpSample> pose =
            weapon.motion.Sample(time, max_extrapolation_ticks)) {
      weapon.position = pose->position;
      weapon.rotation = pose->rotation;
    }
  }
  for (ReplicatedBody& body : bodies_) {
    if (const std::optional<InterpSample> pose =
            body.motion.Sample(time, max_extrapolation_ticks)) {
      body.has_pose = true;
      body.position = pose->position;
      body.rotation = pose->rotation;
    }
  }
}

}  // namespace blue::net
