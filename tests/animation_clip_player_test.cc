#include <doctest/doctest.h>

#include "animation/clip_player.h"

namespace blue::animation {

TEST_CASE("A new player is stopped at time 0") {
  const ClipPlayer player(1.5f);
  CHECK_FALSE(player.playing());
  CHECK_FALSE(player.finished());
  CHECK(player.time() == 0.0f);
  CHECK(player.duration() == 1.5f);
  CHECK(ClipPlayer(-1.0f).duration() == 0.0f);
}

TEST_CASE("A stopped player does not advance") {
  ClipPlayer player(1.5f);
  player.Update(0.5f);
  CHECK(player.time() == 0.0f);
}

TEST_CASE("Play advances from the start time and finishes at the end") {
  ClipPlayer player(1.5f);
  player.Play(0.333f);
  CHECK(player.playing());
  CHECK(player.time() == doctest::Approx(0.333f));

  player.Update(0.5f);
  CHECK(player.time() == doctest::Approx(0.833f));
  CHECK_FALSE(player.finished());

  player.Update(10.0f);  // large dt clamps at the end
  CHECK(player.time() == 1.5f);
  CHECK(player.finished());
  CHECK_FALSE(player.playing());

  player.Update(0.1f);  // stays finished
  CHECK(player.time() == 1.5f);
  CHECK(player.finished());
}

TEST_CASE("Play restarts a running player and clears finished") {
  ClipPlayer player(1.0f);
  player.Play(0.0f);
  player.Update(0.7f);
  player.Play(0.2f);
  CHECK(player.time() == doctest::Approx(0.2f));
  player.Update(2.0f);
  REQUIRE(player.finished());
  player.Play(0.0f);
  CHECK_FALSE(player.finished());
  CHECK(player.playing());
}

TEST_CASE("Play clamps the start time when not looping") {
  ClipPlayer player(1.0f);
  player.Play(5.0f);
  CHECK(player.time() == 1.0f);
  player.Play(-1.0f);
  CHECK(player.time() == 0.0f);
}

TEST_CASE("Speed scales time and negative speed plays backwards") {
  ClipPlayer player(2.0f);
  player.Play(0.0f, 2.0f);
  player.Update(0.25f);
  CHECK(player.time() == doctest::Approx(0.5f));

  player.Play(1.0f, -1.0f);
  player.Update(0.4f);
  CHECK(player.time() == doctest::Approx(0.6f));
  player.Update(5.0f);
  CHECK(player.time() == 0.0f);
  CHECK(player.finished());
}

TEST_CASE("A looping player wraps and never finishes") {
  ClipPlayer player(1.0f);
  player.Play(0.5f, 1.0f, /*loop=*/true);
  CHECK(player.looping());
  player.Update(0.75f);
  CHECK(player.time() == doctest::Approx(0.25f));
  player.Update(1000.3f);  // many loops in one update
  CHECK(player.time() == doctest::Approx(0.55f).epsilon(1.0e-3));
  CHECK(player.playing());
  CHECK_FALSE(player.finished());

  player.Play(-0.25f, -1.0f, /*loop=*/true);
  CHECK(player.time() == doctest::Approx(0.75f));
  player.Update(1.0f);
  CHECK(player.time() == doctest::Approx(0.75f));
  CHECK(player.time() < 1.0f);
}

TEST_CASE("A zero-length clip finishes immediately and loops at 0") {
  ClipPlayer player;
  player.Play(0.0f);
  player.Update(0.0f);
  CHECK(player.finished());
  player.Play(3.0f, 1.0f, /*loop=*/true);
  player.Update(1.0f);
  CHECK(player.time() == 0.0f);
}

TEST_CASE("Stop keeps the time") {
  ClipPlayer player(1.0f);
  player.Play(0.0f);
  player.Update(0.4f);
  player.Stop();
  CHECK_FALSE(player.playing());
  CHECK_FALSE(player.finished());
  player.Update(0.4f);
  CHECK(player.time() == doctest::Approx(0.4f));
}

}  // namespace blue::animation
