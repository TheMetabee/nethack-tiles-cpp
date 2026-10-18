// Port of the tests in src-tauri/src/app.rs for version_warning and
// autologin_for, plus the per-chunk work of `consume_session`, which the
// Rust app had no test for.

#include <doctest/doctest.h>

#include <string>

#include "core/game_stream.hpp"

using namespace nh;

namespace {

ServerVersion found(std::string text, std::optional<NetHackVersion> version) {
  return {std::move(text), version};
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

constexpr const char* kNaoMenu =
    "\x1b[H\x1b[2J\x1b[6;3HNot logged in.\x1b[8;3Hl) Login\x1b[9;3Hr) Register new user";
constexpr const char* kBanner50 =
    "\x1b[7;1H         Version 5.0.0-0 Unix post-release, built Jul 10 2026 22:34:55.\r";

}  // namespace

// --- autologin_for --------------------------------------------------------------

TEST_CASE("a saved login is typed at the prompt") {
  CHECK(autologin_for(false, "username", std::string("hunter2")).has_value());
}

TEST_CASE("a local game has no prompt to answer") {
  CHECK_FALSE(autologin_for(true, "username", std::string("hunter2")).has_value());
}

TEST_CASE("without a saved password the player types it") {
  CHECK_FALSE(autologin_for(false, "username", std::nullopt).has_value());
}

TEST_CASE("a password without a username answers nothing") {
  // To send a password to the menu that asks for a name would type the
  // secret where the screen shows it.
  CHECK_FALSE(autologin_for(false, "", std::string("hunter2")).has_value());
}

// --- version_warning --------------------------------------------------------------

TEST_CASE("a server running the release the profile expects says nothing") {
  CHECK_FALSE(version_warning(found("3.6.7", NetHackVersion::V36), NetHackVersion::V36));
}

TEST_CASE("a 5.0 server under a 3.6 profile is reported") {
  // The case this exists for: Hardfought serves 3.6.7 and 5.0.0 from one
  // host, and the wrong pair draws a statue of a thug on every unexplored
  // square.
  const auto warning = version_warning(found("5.0.0-0", NetHackVersion::V50), NetHackVersion::V36);
  REQUIRE(warning);
  CHECK(has(*warning, "5.0.0-0"));
  CHECK(has(*warning, "3.6"));
}

TEST_CASE("the warning says which setting to change") {
  // A warning with no next step only says that something is broken.
  const auto warning = version_warning(found("5.0.0-0", NetHackVersion::V50), NetHackVersion::V36);
  REQUIRE(warning);
  CHECK(has(*warning, "version"));
}

TEST_CASE("a release with no sheet is named rather than guessed at") {
  const auto warning = version_warning(found("3.4.3", std::nullopt), NetHackVersion::V36);
  REQUIRE(warning);
  CHECK(has(*warning, "3.4.3"));
  // It must not claim a sheet would fix it, because none would.
  CHECK_FALSE(has(*warning, "5.0"));
}

// Not in the Rust suite: the full text, which the page shows as it is.
TEST_CASE("the mismatch warning reads as the rust app wrote it") {
  CHECK(*version_warning(found("5.0.0-0", NetHackVersion::V50), NetHackVersion::V36) ==
        "This server runs NetHack 5.0.0-0, but this profile is set to NetHack 3.6. "
        "Tiles will be wrong. Disconnect, then set the profile's NetHack version to "
        "NetHack 5.0 / 3.7.");
}

// --- GameStream -------------------------------------------------------------------

TEST_CASE("the stream demultiplexes game output") {
  GameStream stream(NetHackVersion::V36, std::nullopt);
  const auto out = stream.feed("\x1b[1;0;348;0z@\x1b[1;1z");
  REQUIRE(out.items.size() == 3);
  CHECK(out.items[0] == StreamItem{Event{GlyphStart{348, 0}}});
  CHECK(out.tiledata_seen);
}

TEST_CASE("tile data is announced only on the first chunk that has it") {
  GameStream stream(NetHackVersion::V36, std::nullopt);
  CHECK_FALSE(stream.feed("plain text").tiledata_seen);
  CHECK(stream.feed("\x1b[1;3z").tiledata_seen);
  CHECK_FALSE(stream.feed("\x1b[1;3z").tiledata_seen);
}

TEST_CASE("the login menu is answered through the reply") {
  GameStream stream(NetHackVersion::V50, autologin_for(false, "username", std::string("hunter2")));
  CHECK(stream.feed(kNaoMenu).reply == std::optional<std::string>("l"));
  CHECK(stream.feed("Please enter your username.").reply ==
        std::optional<std::string>("username\n"));
  CHECK(stream.feed("Please enter your password.").reply ==
        std::optional<std::string>("hunter2\n"));
}

TEST_CASE("a confirmed login is reported once with the game account name") {
  GameStream stream(NetHackVersion::V50, autologin_for(false, "username", std::string("hunter2")));
  stream.feed(kNaoMenu);
  stream.feed("Please enter your username.");
  stream.feed("Please enter your password.");
  const auto out = stream.feed("\x1b[6dLogged in as: username");
  REQUIRE(out.login);
  CHECK(out.login->logged_in);
  // The status bar has already reported the SSH account, which is another
  // account entirely, so the message names this one.
  CHECK(out.login->message == "Logged in to the game server as username");
  CHECK_FALSE(stream.feed("\x1b[6dLogged in as: username").login);
}

TEST_CASE("a rejected login is reported once with the reason") {
  GameStream stream(NetHackVersion::V50, autologin_for(false, "username", std::string("hunter2")));
  stream.feed(kNaoMenu);
  stream.feed("Please enter your username.");
  stream.feed("Please enter your password.");
  const auto out = stream.feed(kNaoMenu);
  REQUIRE(out.login);
  CHECK_FALSE(out.login->logged_in);
  CHECK(has(out.login->message, "rejected"));
  CHECK_FALSE(stream.feed(kNaoMenu).login);
}

TEST_CASE("without a login the menu gets no reply") {
  GameStream stream(NetHackVersion::V50, std::nullopt);
  CHECK_FALSE(stream.feed(kNaoMenu).reply);
}

TEST_CASE("the banner is reported once with a warning when the profile is wrong") {
  GameStream stream(NetHackVersion::V36, std::nullopt);
  const auto out = stream.feed(kBanner50);
  REQUIRE(out.server_version);
  CHECK(out.server_version->text == "5.0.0-0");
  REQUIRE(out.version_warning);
  CHECK(has(*out.version_warning, "5.0.0-0"));
  CHECK_FALSE(stream.feed(kBanner50).server_version);
}

TEST_CASE("the banner of the expected release carries no warning") {
  GameStream stream(NetHackVersion::V50, std::nullopt);
  const auto out = stream.feed(kBanner50);
  REQUIRE(out.server_version);
  CHECK_FALSE(out.version_warning);
}

TEST_CASE("a cp437 byte reaches the banner watch and the demultiplexer unchanged") {
  // IBMgraphics sends raw CP437. The bytes must pass through as bytes.
  GameStream stream(NetHackVersion::V36, std::nullopt);
  const auto out = stream.feed("\xcd\xfa");
  REQUIRE(out.items.size() == 1);
  CHECK(std::get<Text>(out.items[0]).bytes == "\xcd\xfa");
}
