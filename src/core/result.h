#ifndef BLUE_CORE_RESULT_H_
#define BLUE_CORE_RESULT_H_

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>

#include "core/log.h"

namespace blue::core {

// Result<T, E> holds either a value of type T (success) or an error of type
// E. It stands in for the C++23 expected type. The project does not use
// exceptions, so recoverable failures are returned this way.
//
// Results are built only through the static factories, so a value and an
// error of the same type can never be confused:
//
//   Result<int, ParseError> Parse(std::string_view text);
//   ...
//   Result<int, ParseError> parsed = Parse("42");
//   if (!parsed.ok()) { return Result<...>::Error(parsed.error()); }
//   int answer = parsed.value();
//
// Calling value() on an error, or error() on a success, is a programming
// error and terminates the process through log::Fatal.
//
// Copyable iff both T and E are copyable; movable iff both are movable.
// A moved-from Result keeps its state (value or error) but its payload is
// in the moved-from state. Not thread-safe for concurrent mutation; const
// operations are safe to call concurrently.
template <typename T, typename E>
class [[nodiscard]] Result {
 public:
  // Returns a successful result holding `value`.
  static Result Ok(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  // Returns a failed result holding `error`.
  static Result Error(E error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  Result(const Result&) = default;
  Result& operator=(const Result&) = default;
  Result(Result&&) = default;
  Result& operator=(Result&&) = default;

  // Returns true when this result holds a value.
  bool ok() const { return storage_.index() == 0; }

  // Returns the value. Fatal if this result holds an error.
  const T& value() const& {
    RequireValue();
    return std::get<0>(storage_);
  }
  T& value() & {
    RequireValue();
    return std::get<0>(storage_);
  }
  // Consumes this result and moves the value out.
  T&& value() && {
    RequireValue();
    return std::get<0>(std::move(storage_));
  }

  // Returns the error. Fatal if this result holds a value.
  const E& error() const {
    if (ok()) {
      log::Fatal("Result::error() called on a successful Result");
    }
    return std::get<1>(storage_);
  }

 private:
  template <size_t kIndex, typename Arg>
  Result(std::in_place_index_t<kIndex> index, Arg&& arg)
      : storage_(index, std::forward<Arg>(arg)) {}

  void RequireValue() const {
    if (!ok()) {
      log::Fatal("Result::value() called on a failed Result");
    }
  }

  std::variant<T, E> storage_;
};

// Specialization for operations that produce no value: holds either success
// or an error of type E.
//
//   Result<void, FileError> Save();
//   if (Result<void, FileError> saved = Save(); !saved.ok()) { ... }
template <typename E>
class [[nodiscard]] Result<void, E> {
 public:
  // Returns a successful result.
  static Result Ok() { return Result(std::nullopt); }

  // Returns a failed result holding `error`.
  static Result Error(E error) { return Result(std::move(error)); }

  Result(const Result&) = default;
  Result& operator=(const Result&) = default;
  Result(Result&&) = default;
  Result& operator=(Result&&) = default;

  // Returns true when the operation succeeded.
  bool ok() const { return !error_.has_value(); }

  // Returns the error. Fatal if this result is a success.
  const E& error() const {
    if (ok()) {
      log::Fatal("Result::error() called on a successful Result");
    }
    return *error_;
  }

 private:
  explicit Result(std::optional<E> error) : error_(std::move(error)) {}

  std::optional<E> error_;
};

}  // namespace blue::core

#endif  // BLUE_CORE_RESULT_H_
