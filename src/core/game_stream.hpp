#pragma once

// What happens to each chunk of game output before the page sees it.
//
// Port of `consume_session` and its helpers in src-tauri/src/app.rs, without
// the I/O: the server's banner gives the real NetHack version, the auto-login
// answers the dgamelaunch prompts, and the demultiplexer splits tile codes
// from terminal text. The caller sends what comes out.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/autologin.hpp"
#include "core/banner.hpp"
#include "core/demux.hpp"
#include "core/glyph.hpp"

namespace nh {

// How a login attempt ended, for the status bar.
struct LoginOutcome {
  bool logged_in = false;
  // The status text. On a failure, the reason.
  std::string message;
};

// The results of one chunk.
struct GameStreamOutput {
  // Bytes to send to the server: the auto-login's answer to a prompt.
  std::optional<std::string> reply;
  // Set once per session, when the startup banner names the release.
  std::optional<ServerVersion> server_version;
  // Set with `server_version` when the profile expects another release.
  std::optional<std::string> version_warning;
  // Set once, when the login succeeds or fails.
  std::optional<LoginOutcome> login;
  std::vector<StreamItem> items;
  // True on the first chunk that holds a tile escape code.
  bool tiledata_seen = false;
};

class GameStream {
 public:
  // `version` is the release the profile expects. `login` is empty when the
  // player types the login.
  GameStream(NetHackVersion version, std::optional<AutoLogin> login);

  GameStreamOutput feed(std::string_view bytes);

 private:
  NetHackVersion version_;
  std::optional<AutoLogin> login_;
  // The profile only records which release the player expects. One host can
  // serve several, so the server's own word decides. See banner.hpp.
  VersionWatch version_watch_;
  Demuxer demuxer_;
  bool announced_tiledata_ = false;
};

// The warning to show when the server is not the release the profile
// expects, or none when they agree.
//
// One host can serve several releases: Hardfought's menu offers 3.4.3, 3.6.7
// and 5.0.0 behind one SSH host. The profile holds a guess, and the startup
// banner holds the truth.
std::optional<std::string> version_warning(const ServerVersion& found, NetHackVersion profile);

const char* version_name(NetHackVersion version);

// Decides whether to answer the dgamelaunch login prompt for a profile.
//
// A saved password is always used: a player who typed one in wants it used.
// Both halves must be known, because the name is typed first, and a password
// sent in its place shows on the screen in clear text. A local game is
// already logged in as whoever is at the keyboard.
std::optional<AutoLogin> autologin_for(bool local, const std::string& game_user,
                                       const std::optional<std::string>& password);

}  // namespace nh
