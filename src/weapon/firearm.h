#ifndef BLUE_WEAPON_FIREARM_H_
#define BLUE_WEAPON_FIREARM_H_

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace blue::weapon {

// Trigger behaviour. kDoubleAction fires once per pull like kSemi (the
// longer double-action trigger pull is not modelled).
enum class FireMode { kSemi, kAuto, kDoubleAction };

inline constexpr int kFireModeCount = 3;

// A set of fire modes: bit i set = FireMode with value i available.
using FireModeMask = uint32_t;

// Returns the mask holding only `mode`.
constexpr FireModeMask FireModeBit(FireMode mode) {
  return FireModeMask{1} << static_cast<uint32_t>(mode);
}

// What sort of weapon a firearm is; decides the Loadout slot it occupies.
// Indexes tables (kWeaponKindCount entries).
enum class WeaponKind { kPistol, kRifle };

inline constexpr int kWeaponKindCount = 2;

// Most shots a single Firearm::Update can report. At 800 rounds per minute
// this covers a 1.1 s frame; further shots due in that frame are dropped.
inline constexpr int kMaxShotsPerUpdate = 16;

// Most spare magazines a FirearmState holds. Firearm::SaveState() keeps the
// first kMaxSavedSpareMagazines spares and counts the rest in
// FirearmState::spares_dropped.
inline constexpr int kMaxSavedSpareMagazines = 8;

// Seconds two saved reload or cycle timers may differ by and still count as
// the same state in NearlyEqual() (1 ms: well below one 60 Hz tick).
inline constexpr float kStateTimeTolerance = 1.0e-3f;

// Every part of a Firearm that changes while it is used, as plain data
// (trivially copyable, fixed size) for per-tick histories and network
// snapshots. The FirearmSpec is not part of it. Firearm::SaveState() makes
// one and Firearm::RestoreState() applies it.
struct FirearmState {
  FireMode fire_mode = FireMode::kSemi;
  int16_t magazine = 0;            // rounds in the inserted magazine
  int16_t incoming_magazine = -1;  // rounds of the magazine being seated
  bool magazine_inserted = false;
  bool chambered = false;
  bool bolt_locked = false;
  bool empty_reload = false;
  bool pull_used = false;        // the current trigger pull fired or clicked
  int8_t reload_stage = 0;       // reload steps already performed
  float reload_elapsed = 0.0f;   // s
  float reload_duration = 0.0f;  // s, 0 when not reloading
  float cycle_remaining = 0.0f;  // s until the bolt can fire again
  // Round counts of the spares in the firearm's own order; entries at
  // spare_count and above are 0.
  std::array<int16_t, kMaxSavedSpareMagazines> spares{};
  uint8_t spare_count = 0;
  // Spares the firearm carried beyond kMaxSavedSpareMagazines when this state
  // was saved; restoring the state loses them. 0 for every stock weapon.
  uint8_t spares_dropped = 0;

  // Exact comparison, float timers included.
  bool operator==(const FirearmState&) const = default;
};

// Returns whether `a` and `b` are the same state, allowing the float timers
// (reload_elapsed, reload_duration, cycle_remaining) to differ by up to
// `tolerance` seconds; every other field must match exactly.
[[nodiscard]] bool NearlyEqual(const FirearmState& a, const FirearmState& b,
                               float tolerance = kStateTimeTolerance);

// Static description of a magazine-fed, closed-bolt firearm (rifle or pistol;
// a pistol's slide locked back counts as the bolt). Defaults describe an
// M16A2-like rifle firing 5.56x45 mm M855.
struct FirearmSpec {
  // Shown to the player. Must refer to storage outliving the spec (a
  // literal).
  std::string_view display_name = "M16A2";
  int magazine_capacity = 30;
  float rounds_per_minute = 800.0f;      // cyclic rate, > 0
  float tactical_reload_seconds = 2.2f;  // reload with a round still chambered
  float empty_reload_seconds = 2.9f;     // includes releasing the bolt
  float muzzle_velocity = 948.0f;        // m/s
  float bullet_mass = 0.004f;            // kg
  // Quadratic drag coefficient 0.5 * rho * Cd * A / m in 1/m (air 1.225
  // kg/m^3, Cd ~0.3 at Mach 2.8, 5.7 mm diameter, 4 g): the bullet's
  // deceleration is drag * |v|^2.
  float bullet_drag = 1.2e-3f;
  int spare_magazines = 4;  // carried in addition to the inserted one, full
  // Selector position at start. A mode missing from `fire_modes` starts in
  // the first available mode (enum order) instead.
  FireMode fire_mode = FireMode::kSemi;
  // Modes the selector offers; it cycles through them in enum order. With a
  // single mode the selector toggle is ignored (no event). An empty set
  // counts as kSemi only.
  FireModeMask fire_modes =
      FireModeBit(FireMode::kSemi) | FireModeBit(FireMode::kAuto);
  WeaponKind kind = WeaponKind::kRifle;
  // kg, loaded: what a dropped weapon weighs as a physics body (M16A2 3.77
  // kg empty plus a loaded 30-round magazine).
  float mass = 3.99f;
  // False for a weapon missing a vital part (weapon/parts/weapon_stats.h):
  // the trigger does nothing (no shot, no dry fire) and no reload starts.
  bool functional = true;
};

// One frame of player intent.
struct FirearmInput {
  bool trigger_held = false;
  bool reload_pressed = false;     // edge: true on the frame it was pressed
  bool fire_mode_pressed = false;  // edge
  // False blocks shots and dry fire (e.g. while sprinting). The trigger pull
  // is not consumed while blocked: a pull still held when the block ends
  // fires as if it had just begun.
  bool can_fire = true;
};

// What happened during one Firearm::Update.
struct FirearmEvents {
  int shots = 0;  // fired during this update, at most kMaxShotsPerUpdate
  // shot_age[i] for i < shots: seconds between shot i and the END of this
  // update, in firing order (non-increasing), each within [0, dt]. Spawn the
  // bullet with this age so its flight does not depend on the frame rate.
  std::array<float, kMaxShotsPerUpdate> shot_age{};
  bool dry_fire = false;  // trigger pulled on an empty chamber, once per pull
  bool reload_started = false;
  bool reload_finished = false;
  bool fire_mode_changed = false;
};

// Ammunition mechanism of a firearm: chamber, bolt, magazine, spares, fire
// selector and reload timing. No positions, rendering or audio; callers react
// to FirearmEvents.
//
// Rules: a shot needs a chambered round. After a shot the bolt strips the next
// round from the magazine into the chamber or, with an empty magazine, locks
// back with the chamber empty, so a fresh weapon fires capacity + 1 rounds.
// Semi and double action fire once per trigger pull; auto fires at the cyclic
// rate from a time accumulator (the first shot of a pull is immediate, a long
// frame can fire several shots). A reload is refused when the magazine is full
// with a round chambered, when no spare magazine is left, when the weapon has
// no magazine fitted (capacity 0) or is not functional, and while reloading;
// nothing fires during it. The magazine is pulled at 30 % of the reload (it
// returns to the spares unless empty), the fullest spare is seated at 65 % and,
// after an empty reload, the bolt release chambers its top round at 85 %.
//
// Deterministic, frame-rate independent, no allocation after construction.
// Copyable. Not thread-safe.
class Firearm {
 public:
  // Starts loaded: a full magazine plus one round chambered, and
  // `spec.spare_magazines` full spares. Negative counts are treated as 0.
  // Capacity 0 (no magazine fitted): only the chambered round, no spares.
  explicit Firearm(const FirearmSpec& spec = {});
  Firearm(const Firearm&) = default;
  Firearm& operator=(const Firearm&) = default;

  // Advances the mechanism by `dt` seconds (negative counts as 0) under
  // `input` and returns what happened.
  FirearmEvents Update(const FirearmInput& input, float dt);

  // Aborts a running reload (the weapon is put away mid-reload) and returns
  // whether one was running. No events. Steps already performed stay done,
  // the rest are dropped, and the magazine in hand goes back to the spares:
  // - before the old magazine is pulled: nothing has changed;
  // - after the pull, before the new one is seated: the weapon is left with
  //   no magazine (a chambered round can still be fired);
  // - after seating, before the bolt release of an empty reload: the new
  //   magazine stays in and the bolt stays locked back.
  // A later reload starts from that state like any other.
  bool CancelReload();

  // Gives the weapon a new spec after its parts changed, keeping its state
  // where the new parts allow it. A running reload is cancelled first
  // (CancelReload). Then:
  // - the chambered round and a locked-back bolt stay;
  // - capacity 0 (the magazine was taken off): no magazine is inserted and
  //   its rounds are gone with it; reloading is impossible until one is
  //   fitted again;
  // - a magazine fitted where there was none (capacity was 0) is inserted
  //   full;
  // - otherwise rounds above a smaller capacity are dropped, and a magazine
  //   pulled by a cancelled reload stays out;
  // - spare magazines keep their rounds, cut to a smaller capacity;
  // - the fire mode stays if the new spec offers it, else the spec's start
  //   mode is used.
  // Allocates nothing.
  void Refit(const FirearmSpec& spec);

  // Returns the mechanism's state (the spec is not part of it). A firearm
  // carrying more than kMaxSavedSpareMagazines spares is saved with the first
  // ones only; FirearmState::spares_dropped counts the rest.
  [[nodiscard]] FirearmState SaveState() const;
  // Puts the mechanism into `state`, keeping the spec. Restoring a state saved
  // from this firearm and running the same inputs reproduces the same events
  // and states. Allocates only when `state` holds more spares than this
  // firearm has room reserved for (more than it ever carried).
  void RestoreState(const FirearmState& state);

  [[nodiscard]] int rounds_in_magazine() const { return magazine_; }
  [[nodiscard]] bool magazine_inserted() const { return magazine_inserted_; }
  [[nodiscard]] bool round_chambered() const { return chambered_; }
  // True after the last round fired from an empty magazine, until the bolt is
  // released at the end of an empty reload.
  [[nodiscard]] bool bolt_locked_back() const { return bolt_locked_; }
  [[nodiscard]] bool reloading() const { return reload_duration_ > 0.0f; }
  // Progress of the running reload in [0, 1]; 0 when not reloading.
  [[nodiscard]] float reload_fraction() const;
  // True while the running reload is an empty (bolt locked back) one.
  [[nodiscard]] bool empty_reload() const { return empty_reload_; }
  // Rounds chambered plus in the inserted magazine.
  [[nodiscard]] int rounds_loaded() const;
  [[nodiscard]] FireMode fire_mode() const { return fire_mode_; }
  // Round counts of the carried spare magazines, none empty, in no
  // particular order.
  [[nodiscard]] std::span<const int> spare_magazines() const {
    return spare_magazines_;
  }
  [[nodiscard]] const FirearmSpec& spec() const { return spec_; }

 private:
  void UpdateReload(float dt, FirearmEvents& events);
  void UpdateTrigger(const FirearmInput& input, float dt,
                     FirearmEvents& events);
  bool TryStartReload();
  // Fires the chambered round and cycles the bolt.
  void Discharge();
  float ShotInterval() const;

  FirearmSpec spec_;
  FireMode fire_mode_;
  int magazine_ = 0;
  bool magazine_inserted_ = true;
  bool chambered_ = true;
  bool bolt_locked_ = false;
  // Rounds of the magazine being seated; -1 when none is waiting.
  int incoming_magazine_ = -1;
  std::vector<int> spare_magazines_;  // capacity reserved for all magazines
  bool empty_reload_ = false;
  float reload_elapsed_ = 0.0f;   // s
  float reload_duration_ = 0.0f;  // s, 0 when not reloading
  int reload_stage_ = 0;          // reload steps already performed
  // Seconds until the bolt has cycled and can fire again, >= 0.
  float cycle_remaining_ = 0.0f;
  bool pull_used_ = false;  // the current trigger pull fired or clicked
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_FIREARM_H_
