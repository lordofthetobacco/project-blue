#include "world/draw_history.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "core/extent.h"
#include "render/scene_view.h"

namespace blue::world {
namespace {

constexpr uint64_t kRun = 7;
constexpr core::Extent2D kExtent{640, 480};

glm::mat4 At(float x) {
  return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

std::vector<render::DrawItem> Items(std::initializer_list<float> xs) {
  std::vector<render::DrawItem> items;
  for (const float x : xs) {
    items.push_back({.transform = At(x)});
  }
  return items;
}

// Tracks `items` as run `key` in a frame of their own.
void Frame(DrawHistory& history, std::vector<render::DrawItem>& items,
           uint64_t key = kRun) {
  history.BeginFrame();
  history.Track(key, items);
}

render::Camera CameraAt(float x) {
  render::Camera camera;
  camera.view = At(-x);
  camera.projection = glm::mat4(2.0f);
  return camera;
}

}  // namespace

TEST_CASE("A run seen for the first time has no previous transforms") {
  DrawHistory history;
  std::vector<render::DrawItem> items = Items({1.0f, 2.0f});
  Frame(history, items);
  CHECK_FALSE(items[0].previous_transform.has_value());
  CHECK_FALSE(items[1].previous_transform.has_value());
}

TEST_CASE("Only the items that moved get their previous transform") {
  DrawHistory history;
  std::vector<render::DrawItem> first = Items({1.0f, 2.0f, 3.0f});
  Frame(history, first);
  std::vector<render::DrawItem> second = Items({1.0f, 2.5f, 3.0f});
  Frame(history, second);
  CHECK_FALSE(second[0].previous_transform.has_value());
  REQUIRE(second[1].previous_transform.has_value());
  CHECK(*second[1].previous_transform == At(2.0f));
  CHECK_FALSE(second[1].motion_blur);  // The world is never blurred.
  CHECK_FALSE(second[2].previous_transform.has_value());

  // The previous transform is always the one of the frame before.
  std::vector<render::DrawItem> third = Items({1.0f, 4.0f, 3.0f});
  Frame(history, third);
  REQUIRE(third[1].previous_transform.has_value());
  CHECK(*third[1].previous_transform == At(2.5f));

  // An item that stopped stands still again.
  std::vector<render::DrawItem> fourth = Items({1.0f, 4.0f, 3.0f});
  Frame(history, fourth);
  CHECK_FALSE(fourth[1].previous_transform.has_value());
}

TEST_CASE("A previous transform set by someone else is kept") {
  DrawHistory history;
  std::vector<render::DrawItem> first = Items({1.0f});
  Frame(history, first);
  std::vector<render::DrawItem> second = Items({2.0f});
  second[0].previous_transform = At(9.0f);
  Frame(history, second);
  CHECK(*second[0].previous_transform == At(9.0f));
  CHECK(second[0].motion_blur);
}

TEST_CASE("A run that changed its item count starts over") {
  DrawHistory history;
  std::vector<render::DrawItem> first = Items({1.0f, 2.0f});
  Frame(history, first);
  std::vector<render::DrawItem> second = Items({5.0f, 6.0f, 7.0f});
  Frame(history, second);
  for (const render::DrawItem& item : second) {
    CHECK_FALSE(item.previous_transform.has_value());
  }
  std::vector<render::DrawItem> third = Items({5.0f, 6.5f, 7.0f});
  Frame(history, third);
  CHECK(third[1].previous_transform.has_value());
}

TEST_CASE("A run that skipped a frame starts over") {
  DrawHistory history;
  std::vector<render::DrawItem> first = Items({1.0f});
  Frame(history, first);
  history.BeginFrame();  // A frame without the run.
  std::vector<render::DrawItem> later = Items({2.0f});
  Frame(history, later);
  CHECK_FALSE(later[0].previous_transform.has_value());
}

TEST_CASE("A forgotten run starts over") {
  DrawHistory history;
  std::vector<render::DrawItem> first = Items({1.0f});
  Frame(history, first);
  history.Forget(kRun);
  std::vector<render::DrawItem> second = Items({2.0f});
  Frame(history, second);
  CHECK_FALSE(second[0].previous_transform.has_value());
}

TEST_CASE("Runs are kept apart by their key") {
  DrawHistory history;
  std::vector<render::DrawItem> a = Items({1.0f});
  std::vector<render::DrawItem> b = Items({10.0f});
  history.BeginFrame();
  history.Track(1, a);
  history.Track(2, b);
  std::vector<render::DrawItem> a2 = Items({2.0f});
  std::vector<render::DrawItem> b2 = Items({10.0f});
  history.BeginFrame();
  history.Track(1, a2);
  history.Track(2, b2);
  REQUIRE(a2[0].previous_transform.has_value());
  CHECK(*a2[0].previous_transform == At(1.0f));
  CHECK_FALSE(b2[0].previous_transform.has_value());

  // Another key (e.g. another world layout) has no history of its own.
  std::vector<render::DrawItem> c = Items({3.0f});
  history.BeginFrame();
  history.Track(3, c);
  CHECK_FALSE(c[0].previous_transform.has_value());
}

TEST_CASE("The camera's previous frame is valid from the second frame on") {
  DrawHistory history;
  render::Camera first = CameraAt(0.0f);
  history.BeginFrame();
  history.TrackCamera(first, kExtent);
  CHECK_FALSE(first.previous_valid);
  CHECK(first.previous_view_projection == first.projection * first.view);

  render::Camera second = CameraAt(1.0f);
  history.BeginFrame();
  history.TrackCamera(second, kExtent);
  CHECK(second.previous_valid);
  CHECK(second.previous_view_projection == first.projection * first.view);
}

TEST_CASE("A cut, a resize and a skipped frame invalidate the camera") {
  DrawHistory history;
  const auto track = [&history](float x, core::Extent2D extent) {
    render::Camera camera = CameraAt(x);
    history.BeginFrame();
    history.TrackCamera(camera, extent);
    return camera;
  };
  (void)track(0.0f, kExtent);
  CHECK(track(1.0f, kExtent).previous_valid);

  history.Cut();
  const render::Camera cut = track(50.0f, kExtent);
  CHECK_FALSE(cut.previous_valid);
  CHECK(cut.previous_view_projection == cut.projection * cut.view);
  CHECK(track(51.0f, kExtent).previous_valid);

  CHECK_FALSE(track(52.0f, core::Extent2D{800, 600}).previous_valid);
  CHECK(track(53.0f, core::Extent2D{800, 600}).previous_valid);

  history.BeginFrame();  // A frame without the camera.
  CHECK_FALSE(track(54.0f, core::Extent2D{800, 600}).previous_valid);

  history.Clear();
  CHECK_FALSE(track(55.0f, core::Extent2D{800, 600}).previous_valid);
}

}  // namespace blue::world
