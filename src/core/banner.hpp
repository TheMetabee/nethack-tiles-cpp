#pragma once

// Reads the NetHack version out of the startup banner.
//
// Port of src-tauri/src/banner.rs. Tile numbers are positional, so a sheet
// built for the wrong release draws the wrong picture for almost every
// glyph. One host can serve several releases: Hardfought's menu offers 3.4.3,
// 3.6.7 and 5.0.0 behind one SSH host, and the profile cannot know which one
// the player picked.
//
// The server says so itself. Every tty NetHack prints this line before the
// game starts, from one `Sprintf` in `util/makedefs.c`:
//
//          Version 5.0.0-0 Unix post-release, built Jul 10 2026 22:34:55.
//
// An out-of-range tile index is no longer a mismatch signal, because the two
// sheets overlap: index 1469 is "unexplored" on 5.0 and "statue of thug" on
// 3.6.7.

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "core/glyph.hpp"

namespace nh {

// What the server said it is.
struct ServerVersion {
  // The version as printed, e.g. `5.0.0-0`. The player sees it, so it stays
  // verbatim.
  std::string text;
  // The tile ordering this release uses, or none for a release this app has
  // no sheet for.
  std::optional<NetHackVersion> version;

  bool operator==(const ServerVersion&) const = default;
};

std::ostream& operator<<(std::ostream& os, const ServerVersion& found);

// Watches the terminal text for the startup banner.
class VersionWatch {
 public:
  // Records a chunk of terminal output. Returns the version once, on the
  // read that completes the banner.
  std::optional<ServerVersion> observe(std::string_view chunk);

  // Whether the banner is still to come. Once it is not, the caller can stop
  // decoding text it would only throw away.
  bool wants_output() const { return !done_; }

  // How much text is held for the rest of a line.
  std::size_t buffered() const { return tail_.size(); }

 private:
  std::string tail_;
  // The banner is printed once per game. To report it on every later read
  // would be noise.
  bool done_ = false;
};

}  // namespace nh
