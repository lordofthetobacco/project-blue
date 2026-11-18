#ifndef BLUE_COOK_INTERNAL_PARALLEL_H_
#define BLUE_COOK_INTERNAL_PARALLEL_H_

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

// Work splitting for the cook library. Cooking is an offline step, so plain
// threads started per call are cheap enough next to the work they do.

namespace blue::cook::internal {

// Calls `work(i)` for every i in [0, count) on up to
// std::thread::hardware_concurrency() threads (the calling thread included)
// and returns when all calls have finished. Items are handed out one at a
// time, so uneven items balance. `work` must be safe to call concurrently for
// different indices. Order of calls is unspecified; results must not depend
// on it.
template <typename Work>
void ParallelFor(int count, const Work& work) {
  if (count <= 0) {
    return;
  }
  const int hardware =
      std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const int thread_count = std::min(hardware, count);
  std::atomic<int> next{0};
  const auto run = [&next, &work, count] {
    for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
      work(i);
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(thread_count - 1));
  for (int t = 1; t < thread_count; ++t) {
    threads.emplace_back(run);
  }
  run();
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace blue::cook::internal

#endif  // BLUE_COOK_INTERNAL_PARALLEL_H_
