#pragma once

// Types the dgamelaunch login sequence for the user.
//
// Port of src-tauri/src/autologin.rs. NAO and Hardfought do not authenticate
// players over SSH. Everyone connects as a shared game user
// (`nethack@nethack.alt.org`), and dgamelaunch then asks for the game account
// inside the terminal:
//
//    l) login
//    r) register new user
//    => l
//   Please enter your username.
//    => ian
//   Please enter your password.
//
// This is a pure state machine over the server output, so it can be tested
// without a network. It stops once the password is submitted. It does not
// pick a game from the menu after login: those menus differ per server and
// per NetHack version, and a wrong guess would start the wrong game.

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace nh {

// How far the login sequence has progressed.
enum class AutoLoginState {
  // Waiting for the dgamelaunch menu, to choose "login".
  AwaitingMenu,
  AwaitingUsername,
  AwaitingPassword,
  // Credentials submitted; the outcome is not known yet.
  Done,
  // The server confirmed the login.
  LoggedIn,
  // The server rejected the credentials. `AutoLogin::failure` gives the reason.
  Failed,
};

std::ostream& operator<<(std::ostream& os, AutoLoginState state);

// Caps the recent output kept for prompt matching. Prompts are short, so this
// only needs to span a chunk boundary or two.
inline constexpr std::size_t kLoginWindowLimit = 4096;

// How much output to scan for a rejection after the password is submitted.
// Without it the machine stops looking the instant it finishes typing and
// never reports a bad password. To scan forever risks taking in-game text for
// a login failure.
inline constexpr std::size_t kFailureWatchBytes = 4096;

// Drives the dgamelaunch login prompts.
//
// The password stays in memory for the login only. The observation window
// holds server output, which never echoes the password, so the window is
// safe to log. The password field is not: `operator<<` redacts it.
class AutoLogin {
 public:
  AutoLogin(std::string username, std::string password);

  AutoLoginState state() const { return state_; }
  // Why the login failed, when `state()` is `Failed`.
  const std::string& failure() const { return failure_; }
  // The account name, for status messages.
  const std::string& username() const { return username_; }

  // True once the machine will send no more input.
  bool is_finished() const;

  // True while the machine still has a reason to read server output. This
  // outlives `is_finished`: after the password goes, reading continues until
  // the outcome is known, so a rejection is noticed.
  bool wants_output() const;

  // Feeds a chunk of server output. Returns the bytes to send in reply.
  std::optional<std::string> observe(std::string_view chunk);

  std::size_t window_size() const { return window_.size(); }

  friend std::ostream& operator<<(std::ostream& os, const AutoLogin& login);

 private:
  void judge_outcome();
  void reject();

  std::string username_;
  std::string password_;
  AutoLoginState state_ = AutoLoginState::AwaitingMenu;
  std::string failure_;
  // Recent server output with the escape sequences removed.
  std::string window_;
  // Bytes read since the password was submitted.
  std::size_t watched_after_submit_ = 0;
};

// Removes ANSI escape sequences so prompt matching sees plain text.
// dgamelaunch paints its menus with colour and cursor-position codes.
std::string strip_ansi(std::string_view input);

}  // namespace nh
