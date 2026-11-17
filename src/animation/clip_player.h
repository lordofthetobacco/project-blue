#ifndef BLUE_ANIMATION_CLIP_PLAYER_H_
#define BLUE_ANIMATION_CLIP_PLAYER_H_

namespace blue::animation {

// Time cursor for playing a clip of a given duration; it does not reference
// the clip itself. Pass time() to SampleClip() after Update().
//
// A non-looping player clamps its time to [0, duration] and stops with
// finished() set when it reaches the end in the direction it plays (time
// `duration` for positive speed, 0 for negative). A looping player wraps the
// time into [0, duration) and never finishes. Any dt is safe, including dt
// spanning many loops. Copyable value type.
class ClipPlayer {
 public:
  // A stopped player for a zero-length clip.
  ClipPlayer() = default;
  // A stopped player at time 0 for a clip lasting `duration` seconds
  // (negative is treated as 0).
  explicit ClipPlayer(float duration);

  ClipPlayer(const ClipPlayer&) = default;
  ClipPlayer& operator=(const ClipPlayer&) = default;

  // Starts (or restarts, if already playing) at `start_time` seconds,
  // advancing `speed` clip-seconds per second (negative plays backwards).
  // `start_time` is clamped, or wrapped when looping. Clears finished().
  void Play(float start_time, float speed = 1.0f, bool loop = false);

  // Stops advancing and keeps the current time. Clears finished().
  void Stop();

  // Advances a playing player by `dt` >= 0 seconds; no-op when stopped.
  void Update(float dt);

  float time() const { return time_; }
  float duration() const { return duration_; }
  float speed() const { return speed_; }
  bool looping() const { return loop_; }
  bool playing() const { return playing_; }
  // True from the Update() that reached the end until the next Play()/Stop().
  bool finished() const { return finished_; }

 private:
  float duration_ = 0.0f;
  float time_ = 0.0f;
  float speed_ = 1.0f;
  bool loop_ = false;
  bool playing_ = false;
  bool finished_ = false;
};

}  // namespace blue::animation

#endif  // BLUE_ANIMATION_CLIP_PLAYER_H_
