// Port of the tests in src-tauri/src/autologin.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "core/autologin.hpp"

using nh::AutoLogin;
using nh::AutoLoginState;
using nh::strip_ansi;
using Reply = std::optional<std::string>;

namespace {

AutoLogin login() { return AutoLogin("username", "hunter2"); }

constexpr const char* kMenu =
    "## Welcome to the public server\n"
    "\n"
    " l) login\n"
    " r) register new user\n"
    " w) watch games in progress\n"
    " q) quit\n"
    " => ";

// The real thing, captured from nethack.alt.org. It contains no newlines at
// all: every entry is placed with `ESC[row;colH`.
constexpr const char* kNaoMenu =
    "\x1b[H\x1b[2J\x1b[2d ## \x1b(B\x1b[0;1m\x1b[33mnethack.alt.org - "
    "http://nethack.alt.org/\x1b[3;2H\x1b[39m\x1b(B\x1b[m##\x1b[4d\x08\x08## Games on this server "
    "are recorded for in-progress viewing and playback!\x1b[6;3HNot logged in.\x1b[8;3Hl) "
    "Login\x1b[9;3Hr) Register new user\x1b[10;3Hw) Watch games in progress\x1b[12;3Hs) server "
    "info\x1b[13;3Hm) MOTD/news (updated: 2026.07.19)\x1b[15;3Hq) Quit\x1b[19;3H=> ";

// The screen NAO draws once the password is accepted.
constexpr const char* kNaoLoggedIn =
    "\x1b[H\x1b[2J\x1b[2d ## \x1b(B\x1b[0;1m\x1b[33mnethack.alt.org "
    "- http://nethack.alt.org/\x1b[3;2H\x1b[39m\x1b(B\x1b[m##\x1b[4d\x08\x08##\x1b[6d\x08Logged in "
    "as: \x1b(B\x1b[0;1musername\x1b[8;3H\x1b(B\x1b[mc) Change password\x1b[9;3He) Change email "
    "address\x1b[10;3Hw) Watch games in progress\x1b[13;3Hp) Play NetHack 5.0.0\x1b[22;3H=> ";

}  // namespace

TEST_CASE("strip ansi removes csi sequences") {
  CHECK(strip_ansi("\x1b[1;31mred\x1b[0m text") == "red text");
  CHECK(strip_ansi("\x1b[2J\x1b[Hclear") == "clear");
  CHECK(strip_ansi("\x1b(Bplain") == "plain");
  CHECK(strip_ansi("no escapes") == "no escapes");
}

TEST_CASE("starts out awaiting the menu") {
  CHECK(login().state() == AutoLoginState::AwaitingMenu);
  CHECK_FALSE(login().is_finished());
}

TEST_CASE("ordinary output before the menu sends nothing") {
  auto a = login();
  CHECK(a.observe("connecting...\r\n") == Reply());
  CHECK(a.state() == AutoLoginState::AwaitingMenu);
}

TEST_CASE("the menu selects the login option") {
  auto a = login();
  CHECK(a.observe(kMenu) == Reply("l"));
  CHECK(a.state() == AutoLoginState::AwaitingUsername);
}

TEST_CASE("a coloured menu still matches") {
  auto a = login();
  CHECK(a.observe("\x1b[2J\x1b[H\x1b[1;32m l) login\x1b[0m\r\n r) register\r\n") == Reply("l"));
}

TEST_CASE("the username prompt sends the username and a newline") {
  auto a = login();
  a.observe(kMenu);
  CHECK(a.observe("Please enter your username.\r\n => ") == Reply("username\n"));
  CHECK(a.state() == AutoLoginState::AwaitingPassword);
}

TEST_CASE("the password prompt sends the password and finishes") {
  auto a = login();
  a.observe(kMenu);
  a.observe("Please enter your username.\r\n => ");
  CHECK(a.observe("Please enter your password.\r\n => ") == Reply("hunter2\n"));
  CHECK(a.state() == AutoLoginState::Done);
  CHECK(a.is_finished());
}

TEST_CASE("a prompt split across chunks still matches") {
  auto a = login();
  a.observe(kMenu);
  CHECK(a.observe("Please enter your user") == Reply());
  CHECK(a.observe("name.\r\n => ") == Reply("username\n"));
}

TEST_CASE("the menu is answered only once") {
  auto a = login();
  CHECK(a.observe(kMenu) == Reply("l"));
  // The server redraws the menu while it processes the keystroke.
  CHECK(a.observe(kMenu) == Reply());
}

TEST_CASE("output after completion is ignored") {
  auto a = login();
  a.observe(kMenu);
  a.observe("Please enter your username.\r\n");
  a.observe("Please enter your password.\r\n");
  CHECK(a.observe("Please enter your password.\r\n") == Reply());
  CHECK(a.state() == AutoLoginState::Done);
}

TEST_CASE("the menu a real server actually sends is recognised") {
  // dgamelaunch draws its menu with cursor positioning and never sends a
  // newline, so a match per line never fires.
  auto a = login();
  CHECK(a.observe(kNaoMenu) == Reply("l"));
}

TEST_CASE("a full nao session runs end to end") {
  auto a = login();
  std::vector<std::string> sent;
  for (const char* chunk : {
           kNaoMenu,
           "\x1b[H\x1b[2J\x1b[2d ## nethack.alt.org\r\x1b[6d Please enter your username. "
           "(blank entry aborts)\r\x1b[8d => ",
           "username\x1b[H\x1b[2J\x1b[2d ## nethack.alt.org\r\x1b[6d Please enter your "
           "password.\r\x1b[8d => ",
           kNaoLoggedIn,
       }) {
    if (auto bytes = a.observe(chunk)) sent.push_back(*bytes);
  }
  CHECK(sent == std::vector<std::string>{"l", "username\n", "hunter2\n"});
  CHECK(a.state() == AutoLoginState::LoggedIn);
}

TEST_CASE("a confirmed login stops watching for a rejection") {
  // Otherwise in-game text could still trip the failure detector.
  auto a = login();
  a.observe(kNaoMenu);
  a.observe("Please enter your username.");
  a.observe("Please enter your password.");
  a.observe(kNaoLoggedIn);
  CHECK_FALSE(a.wants_output());
}

TEST_CASE("a bad password is detected by the login menu coming back") {
  // NAO says nothing about the failure. It redraws the "Not logged in." menu,
  // so a watch for the words "login failed" never notices.
  auto a = login();
  a.observe(kNaoMenu);
  a.observe("Please enter your username.");
  a.observe("Please enter your password.");
  a.observe(kNaoMenu);
  CHECK(a.state() == AutoLoginState::Failed);
  CHECK_FALSE(a.failure().empty());
}

TEST_CASE("the login menu before the password is submitted is not a rejection") {
  // The same menu starts the sequence. Only its return afterwards means
  // anything.
  auto a = login();
  a.observe(kNaoMenu);
  CHECK(a.state() == AutoLoginState::AwaitingUsername);
}

TEST_CASE("a rejected password is reported as failure") {
  auto a = login();
  a.observe(kMenu);
  a.observe("Please enter your username.\r\n");
  a.observe("Please enter your password.\r\n");
  a.observe("Login failed.\r\n");
  CHECK(a.state() == AutoLoginState::Failed);
  CHECK(a.is_finished());
}

TEST_CASE("failure watching stops once the window is exhausted") {
  auto a = login();
  a.observe(kMenu);
  a.observe("Please enter your username.\r\n");
  a.observe("Please enter your password.\r\n");
  CHECK(a.wants_output());

  a.observe(std::string(nh::kFailureWatchBytes + 1, 'x'));
  CHECK_FALSE(a.wants_output());

  // In-game text that says "login failed" much later must not mark the login
  // as failed after the fact.
  a.observe("You read a scroll labelled LOGIN FAILED.\r\n");
  CHECK(a.state() == AutoLoginState::Done);
}

TEST_CASE("a full session runs end to end") {
  auto a = login();
  std::vector<std::string> sent;
  for (const char* chunk : {
           "\x1b[2J\x1b[H",
           kMenu,
           "l\r\nPlease enter your username.\r\n => ",
           "ian\r\nPlease enter your password.\r\n => ",
           "\r\nLogged in as ian.\r\n",
       }) {
    if (auto bytes = a.observe(chunk)) sent.push_back(*bytes);
  }
  CHECK(sent == std::vector<std::string>{"l", "username\n", "hunter2\n"});
  CHECK(a.state() == AutoLoginState::LoggedIn);
}

TEST_CASE("the observation window stays bounded") {
  auto a = login();
  std::string noise;
  for (int i = 0; i < 100; ++i) noise += "noise ";
  for (int i = 0; i < 200; ++i) a.observe(noise);
  CHECK(a.window_size() <= nh::kLoginWindowLimit * 2);
}

TEST_CASE("a long banner before the menu does not hide it") {
  auto a = login();
  std::string banner;
  for (int i = 0; i < 500; ++i) banner += "banner line\r\n";
  a.observe(banner);
  CHECK(a.observe(kMenu) == Reply("l"));
}

TEST_CASE("debug output does not leak the password") {
  std::ostringstream rendered;
  rendered << login();
  CHECK(rendered.str().find("hunter2") == std::string::npos);
  CHECK(rendered.str().find("username") != std::string::npos);
}
