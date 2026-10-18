#include "core/autologin.hpp"

#include <algorithm>
#include <cctype>

namespace nh {

namespace {

std::string to_lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool mentions(std::string_view text, std::string_view needle) {
  return to_lower(text).find(needle) != std::string::npos;
}

// True if the screen offers the dgamelaunch "login" entry, e.g. `l) Login`.
//
// This is not anchored to a line. dgamelaunch paints its menu with cursor
// moves (`ESC[8;3Hl) Login ESC[9;3Hr) Register new user`) and sends no
// newline on the screen, so once the escape codes are gone the whole menu is
// one line. A match that needs `l)` at the start of a line never fires on a
// real server.
bool offers_login_menu(std::string_view text) {
  const std::string lower = to_lower(text);
  for (std::size_t at = lower.find("l)"); at != std::string::npos;
       at = lower.find("l)", at + 1)) {
    // Reject `control)`: the `l` must start the entry, not end a word.
    const bool standalone = at == 0 || !std::isalnum(static_cast<unsigned char>(lower[at - 1]));
    if (!standalone) continue;
    std::size_t i = at + 2;
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (lower.compare(i, 3, "log") == 0) return true;
  }
  return false;
}

// Rejection wording, for the servers that send any.
bool says_rejected(std::string_view text) {
  return mentions(text, "login failed") || mentions(text, "invalid password");
}

}  // namespace

std::ostream& operator<<(std::ostream& os, AutoLoginState state) {
  switch (state) {
    case AutoLoginState::AwaitingMenu: return os << "AwaitingMenu";
    case AutoLoginState::AwaitingUsername: return os << "AwaitingUsername";
    case AutoLoginState::AwaitingPassword: return os << "AwaitingPassword";
    case AutoLoginState::Done: return os << "Done";
    case AutoLoginState::LoggedIn: return os << "LoggedIn";
    case AutoLoginState::Failed: return os << "Failed";
  }
  return os << "?";
}

AutoLogin::AutoLogin(std::string username, std::string password)
    : username_(std::move(username)), password_(std::move(password)) {}

bool AutoLogin::is_finished() const {
  return state_ == AutoLoginState::Done || state_ == AutoLoginState::LoggedIn ||
         state_ == AutoLoginState::Failed;
}

bool AutoLogin::wants_output() const {
  switch (state_) {
    case AutoLoginState::Failed:
    case AutoLoginState::LoggedIn: return false;
    case AutoLoginState::Done: return watched_after_submit_ < kFailureWatchBytes;
    default: return true;
  }
}

std::optional<std::string> AutoLogin::observe(std::string_view chunk) {
  if (!wants_output()) return std::nullopt;
  if (state_ == AutoLoginState::Done) watched_after_submit_ += chunk.size();

  window_ += strip_ansi(chunk);
  if (window_.size() > kLoginWindowLimit) window_.erase(0, window_.size() - kLoginWindowLimit);

  if (state_ == AutoLoginState::Done) {
    judge_outcome();
    return std::nullopt;
  }

  // Some servers say so outright, and it can arrive at any point.
  if (says_rejected(window_)) {
    reject();
    return std::nullopt;
  }

  std::string reply;
  AutoLoginState next;
  if (state_ == AutoLoginState::AwaitingMenu && offers_login_menu(window_)) {
    reply = "l";
    next = AutoLoginState::AwaitingUsername;
  } else if (state_ == AutoLoginState::AwaitingUsername && mentions(window_, "username")) {
    reply = username_ + "\n";
    next = AutoLoginState::AwaitingPassword;
  } else if (state_ == AutoLoginState::AwaitingPassword && mentions(window_, "password")) {
    reply = password_ + "\n";
    next = AutoLoginState::Done;
  } else {
    return std::nullopt;
  }

  // Drop what was acted on, so a redraw of the same prompt does not trigger a
  // second reply.
  window_.clear();
  state_ = next;
  return reply;
}

// Decides whether the server accepted the credentials.
//
// dgamelaunch does not always say anything when it rejects a password:
// nethack.alt.org only redraws the "Not logged in." menu. So the return of
// the menu is the rejection, and the account name on the screen is the
// confirmation.
void AutoLogin::judge_outcome() {
  if (mentions(window_, "logged in as")) {
    state_ = AutoLoginState::LoggedIn;
    window_.clear();
  } else if (says_rejected(window_) || mentions(window_, "not logged in") ||
             offers_login_menu(window_)) {
    reject();
  }
}

void AutoLogin::reject() {
  state_ = AutoLoginState::Failed;
  failure_ = "the server rejected that game account name or password";
  window_.clear();
}

std::ostream& operator<<(std::ostream& os, const AutoLogin& login) {
  return os << "AutoLogin{username: \"" << login.username_
            << "\", password: <redacted>, state: " << login.state_ << "}";
}

std::string strip_ansi(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  std::size_t i = 0;
  while (i < input.size()) {
    if (input[i] == '\x1b') {
      ++i;
      if (i < input.size() && input[i] == '[') {
        ++i;
        while (i < input.size() && !(input[i] >= 0x40 && input[i] <= 0x7e)) ++i;
        ++i;  // the final byte
      } else {
        // ESC and one byte, e.g. the charset selection ESC ( B.
        i += 2;
      }
      continue;
    }
    out += input[i];
    ++i;
  }
  return out;
}

}  // namespace nh
