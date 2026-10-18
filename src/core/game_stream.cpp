#include "core/game_stream.hpp"

namespace nh {

GameStream::GameStream(NetHackVersion version, std::optional<AutoLogin> login)
    : version_(version), login_(std::move(login)) {}

GameStreamOutput GameStream::feed(std::string_view bytes) {
  GameStreamOutput out;

  if (version_watch_.wants_output()) {
    if (auto found = version_watch_.observe(bytes)) {
      out.version_warning = version_warning(*found, version_);
      out.server_version = std::move(found);
    }
  }

  if (login_) {
    // Only the plain text matters here, and only until the credentials are
    // in.
    out.reply = login_->observe(bytes);
    if (login_->state() == AutoLoginState::Failed) {
      out.login = LoginOutcome{false, login_->failure()};
      login_.reset();
    } else if (login_->state() == AutoLoginState::LoggedIn) {
      // Name the account: the status bar has already reported the SSH
      // connection, and the two are different accounts entirely.
      out.login = LoginOutcome{true, "Logged in to the game server as " + login_->username()};
      login_.reset();
    } else if (!login_->wants_output()) {
      login_.reset();
    }
  }

  out.items = demuxer_.feed(bytes);
  if (!announced_tiledata_ && demuxer_.saw_tile_data()) {
    announced_tiledata_ = true;
    out.tiledata_seen = true;
  }
  return out;
}

const char* version_name(NetHackVersion version) {
  return version == NetHackVersion::V36 ? "NetHack 3.6" : "NetHack 5.0 / 3.7";
}

std::optional<std::string> version_warning(const ServerVersion& found, NetHackVersion profile) {
  if (found.version == profile) return std::nullopt;
  const std::string expected = version_name(profile);
  if (found.version) {
    return "This server runs NetHack " + found.text + ", but this profile is set to " +
           expected +
           ". Tiles will be wrong. Disconnect, then set the profile's NetHack version to " +
           version_name(*found.version) + ".";
  }
  // The name of the release is the useful half even with no sheet to offer.
  // It says why the tiles look wrong, so the player does not look for a
  // setting that would put them right.
  return "This server runs NetHack " + found.text +
         ", which this app has no tile sheet for. Tiles will be wrong. Turn tiles off, or "
         "start a " +
         expected + " game on this server.";
}

std::optional<AutoLogin> autologin_for(bool local, const std::string& game_user,
                                       const std::optional<std::string>& password) {
  if (local || game_user.empty() || !password) return std::nullopt;
  return AutoLogin(game_user, *password);
}

}  // namespace nh
