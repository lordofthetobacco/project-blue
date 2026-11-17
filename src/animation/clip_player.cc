#include "animation/clip_player.h"

#include <algorithm>
#include <cmath>

namespace blue::animation {
namespace {

// Returns `time` wrapped into [0, duration); 0 for a zero-length clip.
float Wrap(float time, float duration) {
  if (duration <= 0.0f) {
    return 0.0f;
  }
  float wrapped = std::fmod(time, duration);
  if (wrapped < 0.0f) {
    wrapped += duration;
  }
  // fmod of a tiny negative value plus duration can round up to duration.
  return wrapped >= duration ? 0.0f : wrapped;
}

}  // namespace

ClipPlayer::ClipPlayer(float duration) : duration_(std::max(duration, 0.0f)) {}

void ClipPlayer::Play(float start_time, float speed, bool loop) {
  speed_ = speed;
  loop_ = loop;
  time_ = loop ? Wrap(start_time, duration_)
               : std::clamp(start_time, 0.0f, duration_);
  playing_ = true;
  finished_ = false;
}

void ClipPlayer::Stop() {
  playing_ = false;
  finished_ = false;
}

void ClipPlayer::Update(float dt) {
  if (!playing_) {
    return;
  }
  const float advanced = time_ + dt * speed_;
  if (loop_) {
    time_ = Wrap(advanced, duration_);
    return;
  }
  time_ = std::clamp(advanced, 0.0f, duration_);
  const bool reached_end = speed_ >= 0.0f ? time_ >= duration_ : time_ <= 0.0f;
  if (reached_end) {
    playing_ = false;
    finished_ = true;
  }
}

}  // namespace blue::animation
