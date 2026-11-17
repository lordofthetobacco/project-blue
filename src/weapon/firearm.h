#ifndef BLUE_WEAPON_FIREARM_H_
#define BLUE_WEAPON_FIREARM_H_

#include <array>
#include <span>
#include <vector>

namespace blue::weapon {

enum class FireMode { kSemi, kAuto };

// Most shots a single Firearm::Update can report. At 800 rounds per minute
// this covers a 1.1 s frame; further shots due in that frame are dropped.
inline constexpr int kMaxShotsPerUpdate = 16;

// Static description of a magazine-fed, closed-bolt rifle. Defaults describe
// an M16A2-like rifle firing 5.56x45 mm M855.
struct FirearmSpec {
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
  FireMode fire_mode = FireMode::kSemi;  // selector position at start
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

// Ammunition mechanism of the rifle: chamber, bolt, magazine, spares, fire
// selector and reload timing. No positions, rendering or audio; callers react
// to FirearmEvents.
//
// Rules: a shot needs a chambered round. After a shot the bolt strips the next
// round from the magazine into the chamber or, with an empty magazine, locks
// back with the chamber empty, so a fresh rifle fires capacity + 1 rounds.
// Semi fires once per trigger pull; auto fires at the cyclic rate from a time
// accumulator (the first shot of a pull is immediate, a long frame can fire
// several shots). A reload is refused when the magazine is full with a round
// chambered, when no spare magazine is left, and while reloading; nothing
// fires during it. The magazine is pulled at 30 % of the reload (it returns
// to the spares unless empty), the fullest spare is seated at 65 % and, after
// an empty reload, the bolt release chambers its top round at 85 %.
//
// Deterministic, frame-rate independent, no allocation after construction.
// Copyable. Not thread-safe.
class Firearm {
 public:
  // Starts loaded: a full magazine plus one round chambered, and
  // `spec.spare_magazines` full spares. Negative counts are treated as 0.
  explicit Firearm(const FirearmSpec& spec = {});
  Firearm(const Firearm&) = default;
  Firearm& operator=(const Firearm&) = default;

  // Advances the mechanism by `dt` seconds (negative counts as 0) under
  // `input` and returns what happened.
  FirearmEvents Update(const FirearmInput& input, float dt);

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
