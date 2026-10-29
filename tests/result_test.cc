#include "core/result.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>

namespace blue::core {
namespace {

enum class TestError { kBroken, kWorse };

TEST_CASE("Result holds a value") {
  const auto result = Result<int, TestError>::Ok(42);
  CHECK(result.ok());
  CHECK(result.value() == 42);
}

TEST_CASE("Result holds an error") {
  const auto result = Result<int, TestError>::Error(TestError::kWorse);
  CHECK_FALSE(result.ok());
  CHECK(result.error() == TestError::kWorse);
}

TEST_CASE("Result value is mutable through a non-const reference") {
  auto result = Result<std::string, TestError>::Ok("abc");
  result.value() += "def";
  CHECK(result.value() == "abcdef");
}

TEST_CASE("Result<void, E> reports success") {
  const auto result = Result<void, TestError>::Ok();
  CHECK(result.ok());
}

TEST_CASE("Result<void, E> reports an error") {
  const auto result = Result<void, TestError>::Error(TestError::kBroken);
  CHECK_FALSE(result.ok());
  CHECK(result.error() == TestError::kBroken);
}

TEST_CASE("Result supports a move-only value") {
  auto result =
      Result<std::unique_ptr<int>, TestError>::Ok(std::make_unique<int>(7));
  REQUIRE(result.ok());
  CHECK(*result.value() == 7);

  auto moved = std::move(result);
  REQUIRE(moved.ok());
  CHECK(*moved.value() == 7);
}

TEST_CASE("Result supports a move-only value taken with value() &&") {
  auto result =
      Result<std::unique_ptr<int>, TestError>::Ok(std::make_unique<int>(9));
  std::unique_ptr<int> taken = std::move(result).value();
  REQUIRE(taken != nullptr);
  CHECK(*taken == 9);
}

TEST_CASE("Result value() && moves the value out") {
  auto result = Result<std::string, TestError>::Ok(std::string(100, 'x'));
  std::string taken = std::move(result).value();
  CHECK(taken == std::string(100, 'x'));
}

TEST_CASE("Result allows the value and error types to be the same") {
  const auto value = Result<int, int>::Ok(3);
  const auto error = Result<int, int>::Error(3);
  CHECK(value.ok());
  CHECK(value.value() == 3);
  CHECK_FALSE(error.ok());
  CHECK(error.error() == 3);
}

TEST_CASE("Result is copyable when its types are") {
  const auto original = Result<std::string, TestError>::Ok("copy me");
  const auto copy = original;  // NOLINT: the copy is the point of the test.
  CHECK(copy.ok());
  CHECK(copy.value() == "copy me");
  CHECK(original.value() == "copy me");

  auto assigned = Result<std::string, TestError>::Error(TestError::kBroken);
  assigned = original;
  CHECK(assigned.ok());
  CHECK(assigned.value() == "copy me");
}

TEST_CASE("Result<void, E> is copyable and assignable") {
  const auto failed = Result<void, TestError>::Error(TestError::kWorse);
  auto copy = failed;  // NOLINT: the copy is the point of the test.
  CHECK(copy.error() == TestError::kWorse);
  copy = Result<void, TestError>::Ok();
  CHECK(copy.ok());
}

}  // namespace
}  // namespace blue::core
