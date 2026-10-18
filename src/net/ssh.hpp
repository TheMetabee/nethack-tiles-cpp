#pragma once

// SSH transport to a public NetHack server.
//
// Port of src-tauri/src/ssh.rs, on libssh in place of russh. Public servers
// run dgamelaunch: every player connects as the same shared SSH user
// (`nethack@nethack.alt.org`) and types the real account into the terminal
// afterwards. That login is core/autologin's job. This module only gets an
// interactive pseudo-terminal and moves bytes.
//
// It talks to a real network, so a manual run against NAO and Hardfought
// tests it. The decisions with logic in them (the host key policy, the
// defaults) are pure functions with unit tests.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "core/session.hpp"

namespace nh::ssh {

// No credential at all. dgamelaunch hosts often allow this for the shared
// game user.
struct AuthNone {};

// The published password of the shared game user, often the same as the
// user name. This is not the player's game password.
struct AuthPassword {
  std::string password;
};

// A private key, for users who have one registered.
struct AuthKey {
  std::filesystem::path path;
  std::optional<std::string> passphrase;
};

using Auth = std::variant<AuthNone, AuthPassword, AuthKey>;

// What to do when the server's host key is not trusted yet.
enum class HostKeyPolicy {
  // Refuse unless the key is already in known_hosts.
  Strict,
  // Record an unknown key on the first connection, but refuse a changed key.
  TrustOnFirstUse,
  // Accept anything. Never the default: it turns off the only defence
  // against a man in the middle.
  AcceptAny,
};

struct Config {
  std::string host;
  std::uint16_t port = 22;
  // The shared SSH user, e.g. `nethack`.
  std::string user;
  // Tried in order until one succeeds.
  std::vector<Auth> auth;
  // The TERM value for the pseudo-terminal.
  std::string term = "xterm-256color";
  std::uint32_t cols = 80;
  std::uint32_t rows = 24;
  HostKeyPolicy host_key_policy = HostKeyPolicy::TrustOnFirstUse;
  // How long to wait for the connection and the handshake.
  std::chrono::seconds connect_timeout{20};
  // The known_hosts file. Empty means OpenSSH's own `~/.ssh/known_hosts`,
  // which the Rust app used too, so keys it trusted stay trusted.
  std::optional<std::filesystem::path> known_hosts;

  // A typical dgamelaunch server: no credential first, then the usual shared
  // password.
  static Config dgamelaunch(std::string host, std::uint16_t port, std::string user);
};

class SshError : public std::runtime_error {
 public:
  enum class Kind {
    Connect,
    HostKeyChanged,
    HostKeyUnknown,
    HostKeyUncheckable,
    ConnectTimeout,
    AuthFailed,
    KeyLoad,
    Protocol,
  };

  SshError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

  static SshError connect(const std::string& host, std::uint16_t port, const std::string& cause);
  static SshError host_key_changed(const std::string& host, std::uint16_t port);
  static SshError host_key_unknown(const std::string& host, std::uint16_t port);
  static SshError host_key_uncheckable(const std::string& host, std::uint16_t port,
                                       const std::string& cause);
  static SshError connect_timeout(const std::string& host, std::uint16_t port,
                                  std::chrono::seconds timeout);
  static SshError auth_failed();
  static SshError key_load(const std::filesystem::path& path, const std::string& cause);
  static SshError protocol(const std::string& cause);

 private:
  Kind kind_;
};

// What known_hosts says about the server's key. The values follow libssh's
// `ssh_known_hosts_e`.
enum class KnownHost {
  // The key matches the one on record.
  Ok,
  // The host is on record with another key of the same type.
  Changed,
  // The host is on record only with a key of another type.
  Other,
  // The host is not on record.
  Unknown,
  // The known_hosts file does not exist yet.
  NotFound,
  // The file could not be read.
  Error,
};

enum class HostKeyAction {
  Accept,
  // Accept, and record the key in known_hosts.
  Learn,
  Reject,
};

struct HostKeyDecision {
  HostKeyAction action;
  // Why, when the key is rejected.
  std::optional<SshError::Kind> rejection;

  bool operator==(const HostKeyDecision&) const = default;
};

HostKeyDecision decide_host_key(KnownHost known, HostKeyPolicy policy);

// Connects, authenticates and opens an interactive shell. Blocks for the
// network, so call it off the UI thread. Throws SshError.
//
// The session sends no game output to `events` until `start()`. Status
// messages during the connect go to `events` at once.
std::unique_ptr<Session> connect(const Config& config, SessionEvents events);

}  // namespace nh::ssh
