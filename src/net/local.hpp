#pragma once

// Runs a NetHack installed on this machine, in a pseudo-terminal.
//
// Port of src-tauri/src/local.rs. NetHack's tty interface talks to a
// terminal, not a pipe: it asks for the window size with TIOCGWINSZ, puts
// the line discipline in raw mode, and refuses to start at all without a
// controlling terminal. So a local game needs a real pty, as the SSH
// transport gets one from the server.
//
// Tiles are a compile-time option in NetHack (TTY_TILES_ESCCODES), and most
// packaged builds (Homebrew's, Debian's) are built without it. Such a build
// plays perfectly well here, in ASCII; it never emits a tile code, which is
// what the "no tiles yet" hint in the page is for.
//
// Unix only, as in the Rust app. On Windows, spawn() throws Unsupported.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/glyph.hpp"
#include "core/session.hpp"

namespace nh::local {

class LocalError : public std::runtime_error {
 public:
  enum class Kind { NotFound, Spawn, Pty, Unsupported };

  LocalError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

  static LocalError not_found();
  static LocalError spawn(const std::string& command, const std::string& cause);
  static LocalError pty(const std::string& cause);
  static LocalError unsupported();

 private:
  Kind kind_;
};

// How to start the local game.
struct Config {
  std::filesystem::path command;
  std::vector<std::string> args;
  // TERM for the child. NetHack looks up its termcap under this name.
  std::string term = "xterm-256color";
  std::uint16_t cols = 80;
  std::uint16_t rows = 24;
};

// The directories to search, in priority order: the inherited PATH first,
// then the usual install locations. A GUI app is often not started from a
// login shell, so a packaged NetHack may be outside its PATH.
std::vector<std::filesystem::path> search_dirs(std::optional<std::string_view> path_env);

// The first NetHack executable in `dirs`, according to `runnable`. The
// predicate is injected so the search order is testable on any machine.
std::optional<std::filesystem::path> find_in(
    const std::vector<std::filesystem::path>& dirs,
    const std::function<bool(const std::filesystem::path&)>& runnable);

// Whether `path` is a file this machine can run.
bool is_runnable(const std::filesystem::path& path);

// Looks for a local NetHack on this machine.
std::optional<std::filesystem::path> find();

// Reads the NetHack release out of `nethack --version` output. Tile indices
// are positional and move between releases, so asking beats assuming.
std::optional<NetHackVersion> parse_version(std::string_view output);

// How long a --version probe may run. It happens while the window is being
// put together, so a `nethack` that does not answer must not hold the app up.
inline constexpr std::chrono::seconds kProbeTimeout{3};

// Asks a NetHack binary which release it is. Empty if it will not say, or
// not within `limit`; a probe still running then is killed.
std::optional<NetHackVersion> probe_version(const std::filesystem::path& command,
                                            std::chrono::milliseconds limit = kProbeTimeout);

// Starts the game in a pty. Throws LocalError.
//
// The session sends no game output to `events` until `start()`; the game
// waits on the full pty until then. Disconnecting hangs up on the game
// (SIGHUP), which NetHack answers by saving.
std::unique_ptr<Session> spawn(const Config& config, SessionEvents events);

#ifndef _WIN32
namespace detail {

// Opens a pty pair sized cols x rows, both ends close-on-exec. Exposed for
// the tests. Throws LocalError.
std::pair<int, int> open_pty(std::uint16_t cols, std::uint16_t rows);

}  // namespace detail
#endif

}  // namespace nh::local
