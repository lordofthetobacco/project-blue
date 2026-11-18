#include "editor/commands.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/result.h"

namespace blue::editor {

namespace {

using VoidResult = core::Result<void, std::string>;

constexpr std::string_view kWaitCommand = "wait";

// Returns whether `c` separates words.
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

std::vector<std::vector<std::string>> ParseCommandLine(std::string_view line) {
  std::vector<std::vector<std::string>> commands(1);
  std::string word;
  bool in_word = false;
  bool quoted = false;
  const auto end_word = [&] {
    if (in_word) {
      commands.back().push_back(std::move(word));
      word.clear();
      in_word = false;
    }
  };
  for (const char c : line) {
    if (quoted) {
      if (c == '"') {
        quoted = false;
      } else {
        word.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
      in_word = true;  // "" is an empty word.
    } else if (c == ';') {
      end_word();
      commands.emplace_back();
    } else if (IsSpace(c)) {
      end_word();
    } else {
      word.push_back(c);
      in_word = true;
    }
  }
  end_word();
  std::erase_if(commands, [](const std::vector<std::string>& command) {
    return command.empty();
  });
  return commands;
}

core::Result<float, std::string> ParseFloatArg(std::string_view text,
                                               std::string_view what) {
  float value = 0.0f;
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed =
      std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end || !std::isfinite(value)) {
    return core::Result<float, std::string>::Error(
        std::format("{} must be a number, got '{}'", what, text));
  }
  return core::Result<float, std::string>::Ok(value);
}

core::Result<int, std::string> ParseIntArg(std::string_view text,
                                           std::string_view what) {
  int value = 0;
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed =
      std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return core::Result<int, std::string>::Error(
        std::format("{} must be an integer, got '{}'", what, text));
  }
  return core::Result<int, std::string>::Ok(value);
}

void CommandRegistry::Register(std::string name, std::string usage,
                               CommandHandler handler) {
  for (Command& command : commands_) {
    if (command.name == name) {
      core::log::Warn("Command '{}' registered twice; the last one wins", name);
      command.usage = std::move(usage);
      command.handler = std::move(handler);
      return;
    }
  }
  commands_.push_back({std::move(name), std::move(usage), std::move(handler)});
}

const CommandRegistry::Command* CommandRegistry::Find(
    std::string_view name) const {
  for (const Command& command : commands_) {
    if (command.name == name) {
      return &command;
    }
  }
  return nullptr;
}

VoidResult CommandRegistry::RunWords(std::span<const std::string> words) {
  const std::string& name = words.front();
  if (name == kWaitCommand) {
    return VoidResult::Ok();
  }
  if (name == "help") {
    for (const std::string& line : Help()) {
      core::log::Info("{}", line);
    }
    return VoidResult::Ok();
  }
  const Command* command = Find(name);
  if (command == nullptr) {
    return VoidResult::Error(std::format("{}: unknown command", name));
  }
  VoidResult result = command->handler(words.subspan(1));
  if (!result.ok()) {
    return VoidResult::Error(std::format("{}: {}", name, result.error()));
  }
  return VoidResult::Ok();
}

VoidResult CommandRegistry::Run(std::string_view line) {
  for (const std::vector<std::string>& words : ParseCommandLine(line)) {
    VoidResult result = RunWords(words);
    if (!result.ok()) {
      return result;
    }
  }
  return VoidResult::Ok();
}

void CommandRegistry::Enqueue(std::string_view line) {
  for (std::vector<std::string>& words : ParseCommandLine(line)) {
    queue_.push_back(std::move(words));
  }
}

std::vector<std::string> CommandRegistry::RunQueued() {
  std::vector<std::string> errors;
  if (wait_frames_ > 0) {
    --wait_frames_;
    return errors;
  }
  while (!queue_.empty()) {
    std::vector<std::string> words = std::move(queue_.front());
    queue_.pop_front();
    if (words.front() == kWaitCommand) {
      int frames = 1;
      if (words.size() > 1) {
        core::Result<int, std::string> parsed = ParseIntArg(words[1], "frames");
        frames = parsed.ok() ? std::max(parsed.value(), 0) : 1;
      }
      wait_frames_ = frames;
      if (wait_frames_ > 0) {
        break;
      }
      continue;
    }
    core::log::Info("> {}", [&words] {
      std::string joined;
      for (const std::string& word : words) {
        joined += joined.empty() ? word : " " + word;
      }
      return joined;
    }());
    VoidResult result = RunWords(words);
    if (!result.ok()) {
      core::log::Error("Command failed: {}", result.error());
      errors.push_back(result.error());
    }
  }
  return errors;
}

std::vector<std::string> CommandRegistry::Help() const {
  std::vector<std::string> lines;
  lines.reserve(commands_.size() + 2);
  for (const Command& command : commands_) {
    lines.push_back(std::format("{}: {}", command.name, command.usage));
  }
  lines.push_back("help: lists the commands");
  lines.push_back("wait <frames>: pauses a queued script");
  std::sort(lines.begin(), lines.end());
  return lines;
}

}  // namespace blue::editor
