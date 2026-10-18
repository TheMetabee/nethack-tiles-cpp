#pragma once

// Writes the LLM state-log directory.
//
// Port of src-tauri/src/statelog.rs. The frontend decides the text. This
// module only creates, replaces and deletes a fixed set of names, so a bad
// payload from the page cannot write anywhere else.

#include <array>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nh::statelog {

// Files this module creates or deletes. Anything else in the directory
// belongs to the player.
inline constexpr std::array<std::string_view, 7> kFilenames = {
    "README.md",     "screen.txt",  "level.txt",      "messages.txt",
    "inventory.txt", "dungeon.txt", "containers.txt",
};

class StateLogError : public std::runtime_error {
 public:
  enum class Kind { ForbiddenName, Write, Remove };

  StateLogError(Kind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

bool is_allowed_name(std::string_view name);

// Deletes the known state files. Other entries in `dir` stay. A missing file
// or a missing directory is not an error.
void clear(const std::filesystem::path& dir);

void write_file(const std::filesystem::path& dir, std::string_view name,
                std::string_view contents);

void write_files(const std::filesystem::path& dir,
                 const std::map<std::string, std::string>& files);

// The page sends paths as UTF-8. On Windows a `path` made from a plain
// `std::string` reads it in the ANSI code page instead.
std::filesystem::path path_from_utf8(std::string_view utf8);

}  // namespace nh::statelog
