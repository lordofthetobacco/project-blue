#ifndef BLUE_EDITOR_COMMANDS_H_
#define BLUE_EDITOR_COMMANDS_H_

#include <cstddef>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace blue::editor {

// A text command's handler: gets the words after the command name (quotes
// removed) and returns a readable error on failure.
using CommandHandler = std::function<core::Result<void, std::string>(
    std::span<const std::string>)>;

// Named text commands: what menu items and shortcuts run, what --exec scripts
// call, and how the supervisor drives the editor without a mouse. Panels
// register their verbs (e.g. "select material 0", "set roughness 0.3") at
// startup. Lines may hold several commands separated by ';'; words are
// separated by spaces, and "double quotes" keep spaces in one word.
//
// Queued lines run between frames (RunQueued()), so a command never runs in
// the middle of building the UI. The special command "wait <frames>" pauses
// the queue for that many frames (e.g. to let a load show before the next
// step). Uses std::function: this is UI-rate code, not per-draw code.
// Neither copyable nor movable (handlers capture the panels). Not
// thread-safe.
class CommandRegistry {
 public:
  CommandRegistry() = default;
  CommandRegistry(const CommandRegistry&) = delete;
  CommandRegistry& operator=(const CommandRegistry&) = delete;

  // Registers `name` (one word, lower case by convention). `usage` is shown
  // by "help", e.g. "set <param> <value>: sets a material parameter".
  // Registering a name twice replaces the handler (and logs a warning).
  void Register(std::string name, std::string usage, CommandHandler handler);

  // Runs every command of `line` now, in order, stopping at the first
  // failure, which is returned as "<command>: <error>". Unknown commands
  // fail. "wait" is only meaningful in queued lines and is ignored here.
  core::Result<void, std::string> Run(std::string_view line);

  // Queues `line` for RunQueued().
  void Enqueue(std::string_view line);
  // Runs queued commands until the queue is empty or a "wait" starts; call
  // once per frame. Failures are logged and returned; the commands after a
  // failed one still run.
  std::vector<std::string> RunQueued();
  [[nodiscard]] bool idle() const {
    return queue_.empty() && wait_frames_ == 0;
  }

  // Returns "name: usage" for every command, sorted by name.
  [[nodiscard]] std::vector<std::string> Help() const;

 private:
  struct Command {
    std::string name;
    std::string usage;
    CommandHandler handler;
  };

  [[nodiscard]] const Command* Find(std::string_view name) const;
  core::Result<void, std::string> RunWords(std::span<const std::string> words);

  std::vector<Command> commands_;
  // Single commands (already split at ';') waiting to run, as words.
  std::deque<std::vector<std::string>> queue_;
  int wait_frames_ = 0;
};

// Splits `line` into commands at ';' (outside quotes) and each command into
// words; empty commands are dropped. Exposed for tests.
[[nodiscard]] std::vector<std::vector<std::string>> ParseCommandLine(
    std::string_view line);

// Parses a float / int command argument; the error names `what`.
[[nodiscard]] core::Result<float, std::string> ParseFloatArg(
    std::string_view text, std::string_view what);
[[nodiscard]] core::Result<int, std::string> ParseIntArg(std::string_view text,
                                                         std::string_view what);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_COMMANDS_H_
