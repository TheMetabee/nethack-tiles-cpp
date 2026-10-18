#include "net/ssh.hpp"

#include <libssh/libssh.h>

#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace nh::ssh {

namespace {

std::string endpoint(const std::string& host, std::uint16_t port) {
  return host + ":" + std::to_string(port);
}

struct SessionDeleter {
  void operator()(ssh_session session) const {
    ssh_disconnect(session);
    ssh_free(session);
  }
};
using SessionPtr = std::unique_ptr<ssh_session_struct, SessionDeleter>;

struct ChannelDeleter {
  void operator()(ssh_channel channel) const {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
  }
};
using ChannelPtr = std::unique_ptr<ssh_channel_struct, ChannelDeleter>;

struct KeyDeleter {
  void operator()(ssh_key key) const { ssh_key_free(key); }
};
using KeyPtr = std::unique_ptr<ssh_key_struct, KeyDeleter>;

void initialise_library() {
  static std::once_flag once;
  std::call_once(once, [] { ssh_init(); });
}

KnownHost known_host(ssh_known_hosts_e state) {
  switch (state) {
    case SSH_KNOWN_HOSTS_OK: return KnownHost::Ok;
    case SSH_KNOWN_HOSTS_CHANGED: return KnownHost::Changed;
    case SSH_KNOWN_HOSTS_OTHER: return KnownHost::Other;
    case SSH_KNOWN_HOSTS_UNKNOWN: return KnownHost::Unknown;
    case SSH_KNOWN_HOSTS_NOT_FOUND: return KnownHost::NotFound;
    case SSH_KNOWN_HOSTS_ERROR: break;
  }
  return KnownHost::Error;
}

// The server key's SHA-256 fingerprint, as OpenSSH prints it.
std::string fingerprint(ssh_session session) {
  ssh_key raw = nullptr;
  if (ssh_get_server_publickey(session, &raw) != SSH_OK) return "unknown fingerprint";
  const KeyPtr key(raw);
  unsigned char* hash = nullptr;
  size_t length = 0;
  if (ssh_get_publickey_hash(key.get(), SSH_PUBLICKEY_HASH_SHA256, &hash, &length) != 0) {
    return "unknown fingerprint";
  }
  char* text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
  ssh_clean_pubkey_hash(&hash);
  std::string out = text ? text : "unknown fingerprint";
  ssh_string_free_char(text);
  return out;
}

// Answers every keyboard-interactive prompt with `password`. Many dgamelaunch
// hosts offer only keyboard-interactive for what is in fact password auth.
bool keyboard_interactive(ssh_session session, const std::string& password) {
  int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
  while (rc == SSH_AUTH_INFO) {
    const int prompts = ssh_userauth_kbdint_getnprompts(session);
    for (int i = 0; i < prompts; ++i) {
      ssh_userauth_kbdint_setanswer(session, static_cast<unsigned>(i), password.c_str());
    }
    rc = ssh_userauth_kbdint(session, nullptr, nullptr);
  }
  if (rc == SSH_AUTH_ERROR) throw SshError::protocol(ssh_get_error(session));
  return rc == SSH_AUTH_SUCCESS;
}

// Tries each configured method until one succeeds.
void authenticate(ssh_session session, const Config& config) {
  for (const auto& method : config.auth) {
    int rc = SSH_AUTH_DENIED;
    if (std::holds_alternative<AuthNone>(method)) {
      rc = ssh_userauth_none(session, nullptr);
    } else if (const auto* password = std::get_if<AuthPassword>(&method)) {
      rc = ssh_userauth_password(session, nullptr, password->password.c_str());
      if (rc == SSH_AUTH_DENIED && keyboard_interactive(session, password->password)) return;
    } else {
      const auto& key_auth = std::get<AuthKey>(method);
      ssh_key raw = nullptr;
      // libssh opens the file with the ANSI code page on Windows.
      if (ssh_pki_import_privkey_file(key_auth.path.string().c_str(),
                                      key_auth.passphrase ? key_auth.passphrase->c_str() : nullptr,
                                      nullptr, nullptr, &raw) != SSH_OK) {
        throw SshError::key_load(key_auth.path, "the key is unreadable, or the passphrase is wrong");
      }
      const KeyPtr key(raw);
      rc = ssh_userauth_publickey(session, nullptr, key.get());
    }
    if (rc == SSH_AUTH_SUCCESS) return;
    if (rc == SSH_AUTH_ERROR) throw SshError::protocol(ssh_get_error(session));
  }
  throw SshError::auth_failed();
}

// NetHack is turn-based, and a player can think for minutes. A packet every
// 30 seconds keeps routers from dropping the idle connection, and a failed
// send finds a dead one.
constexpr auto kKeepalive = std::chrono::seconds(30);

// How long one wait for server output lasts before the queued keys get a
// turn. libssh is not safe to call from two threads, so this thread does all
// the work, and keys wait at most this long.
constexpr int kPollMs = 10;

// Owns the channel for the life of the session and moves bytes both ways.
// Every libssh call happens on the pump thread.
class SshSession final : public Session {
 public:
  SshSession(SessionPtr session, ChannelPtr channel, SessionEvents events)
      : session_(std::move(session)), channel_(std::move(channel)), events_(std::move(events)) {}

  ~SshSession() override {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  void start() override {
    if (!thread_.joinable()) thread_ = std::thread([this] { pump(); });
  }

  void write(std::string bytes) override {
    std::lock_guard lock(mutex_);
    if (closed_) throw Disconnected();
    commands_.push_back({Command::Kind::Data, std::move(bytes), 0, 0});
  }

  void resize(std::uint32_t cols, std::uint32_t rows) override {
    std::lock_guard lock(mutex_);
    if (closed_) throw Disconnected();
    commands_.push_back({Command::Kind::Resize, {}, cols, rows});
  }

  void disconnect() override { stop_ = true; }

  bool is_connected() const override { return !closed_; }

 private:
  struct Command {
    enum class Kind { Data, Resize } kind;
    std::string bytes;
    std::uint32_t cols;
    std::uint32_t rows;
  };

  // Sends the queued commands. Returns the reason on a failure.
  std::optional<std::string> run_commands() {
    std::deque<Command> batch;
    {
      std::lock_guard lock(mutex_);
      batch.swap(commands_);
    }
    for (const auto& command : batch) {
      if (command.kind == Command::Kind::Resize) {
        if (ssh_channel_change_pty_size(channel_.get(), static_cast<int>(command.cols),
                                        static_cast<int>(command.rows)) != SSH_OK) {
          std::cerr << "[ssh] window change failed: " << ssh_get_error(session_.get()) << "\n";
        }
        continue;
      }
      std::size_t sent = 0;
      while (sent < command.bytes.size()) {
        const int n = ssh_channel_write(channel_.get(), command.bytes.data() + sent,
                                        static_cast<uint32_t>(command.bytes.size() - sent));
        if (n == SSH_ERROR) return std::string("send failed: ") + ssh_get_error(session_.get());
        sent += static_cast<std::size_t>(n);
      }
    }
    return std::nullopt;
  }

  void pump() {
    std::optional<std::string> reason;
    auto last_keepalive = std::chrono::steady_clock::now();
    char buffer[16384];

    while (!stop_) {
      if ((reason = run_commands())) break;

      const int n = ssh_channel_read_timeout(channel_.get(), buffer, sizeof buffer, 0, kPollMs);
      if (n == SSH_ERROR) {
        reason = std::string("the connection failed: ") + ssh_get_error(session_.get());
        break;
      }
      if (n > 0) events_(SessionData{std::string(buffer, static_cast<std::size_t>(n))});

      // Standard error goes to the same terminal, as a pseudo-terminal does.
      const int e = ssh_channel_read_nonblocking(channel_.get(), buffer, sizeof buffer, 1);
      if (e > 0) events_(SessionData{std::string(buffer, static_cast<std::size_t>(e))});

      if (n == 0 && e <= 0 &&
          (ssh_channel_is_eof(channel_.get()) || ssh_channel_is_closed(channel_.get()))) {
        uint32_t code = 0;
        char* signal = nullptr;
        int core_dumped = 0;
        if (ssh_channel_get_exit_state(channel_.get(), &code, &signal, &core_dumped) == SSH_OK &&
            code != 0) {
          reason = "the game exited with status " + std::to_string(code);
        }
        ssh_string_free_char(signal);
        break;
      }

      if (const auto now = std::chrono::steady_clock::now(); now - last_keepalive >= kKeepalive) {
        last_keepalive = now;
        if (ssh_send_ignore(session_.get(), "") != SSH_OK) {
          reason = std::string("the connection failed: ") + ssh_get_error(session_.get());
          break;
        }
      }
    }

    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      commands_.clear();
    }
    events_(SessionClosed{reason});
  }

  SessionPtr session_;
  ChannelPtr channel_;
  SessionEvents events_;
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Command> commands_;
  std::atomic<bool> stop_ = false;
  std::atomic<bool> closed_ = false;
};

}  // namespace

Config Config::dgamelaunch(std::string host, std::uint16_t port, std::string user) {
  Config config;
  config.host = std::move(host);
  config.port = port;
  config.auth = {AuthNone{}, AuthPassword{user}};
  config.user = std::move(user);
  return config;
}

SshError SshError::connect(const std::string& host, std::uint16_t port, const std::string& cause) {
  return {Kind::Connect, "could not reach " + endpoint(host, port) + ": " + cause};
}

SshError SshError::host_key_changed(const std::string& host, std::uint16_t port) {
  return {Kind::HostKeyChanged,
          "the host key for " + endpoint(host, port) +
              " has changed since it was trusted -- refusing to connect; remove the stale entry "
              "from ~/.ssh/known_hosts if this is expected"};
}

SshError SshError::host_key_unknown(const std::string& host, std::uint16_t port) {
  return {Kind::HostKeyUnknown,
          "the host key for " + endpoint(host, port) + " is not in ~/.ssh/known_hosts"};
}

SshError SshError::host_key_uncheckable(const std::string& host, std::uint16_t port,
                                        const std::string& cause) {
  return {Kind::HostKeyUncheckable,
          "could not check the host key for " + endpoint(host, port) + ": " + cause};
}

SshError SshError::connect_timeout(const std::string& host, std::uint16_t port,
                                   std::chrono::seconds timeout) {
  return {Kind::ConnectTimeout, endpoint(host, port) + " did not answer within " +
                                    std::to_string(timeout.count()) + "s"};
}

SshError SshError::auth_failed() {
  return {Kind::AuthFailed, "the server rejected every configured authentication method"};
}

SshError SshError::key_load(const std::filesystem::path& path, const std::string& cause) {
  return {Kind::KeyLoad, "could not read the private key " + path.string() + ": " + cause};
}

SshError SshError::protocol(const std::string& cause) {
  return {Kind::Protocol, "ssh error: " + cause};
}

HostKeyDecision decide_host_key(KnownHost known, HostKeyPolicy policy) {
  if (policy == HostKeyPolicy::AcceptAny || known == KnownHost::Ok) {
    return {HostKeyAction::Accept, std::nullopt};
  }
  switch (known) {
    // russh refused any key that differed from the record, whatever its
    // type, and so does this.
    case KnownHost::Changed:
    case KnownHost::Other: return {HostKeyAction::Reject, SshError::Kind::HostKeyChanged};
    case KnownHost::Unknown:
    case KnownHost::NotFound:
      if (policy == HostKeyPolicy::TrustOnFirstUse) return {HostKeyAction::Learn, std::nullopt};
      return {HostKeyAction::Reject, SshError::Kind::HostKeyUnknown};
    default: return {HostKeyAction::Reject, SshError::Kind::HostKeyUncheckable};
  }
}

std::unique_ptr<Session> connect(const Config& config, SessionEvents events) {
  initialise_library();
  SessionPtr session(ssh_new());
  if (!session) throw SshError::protocol("cannot create an SSH session");

  ssh_session s = session.get();
  const unsigned int port = config.port;
  // Bounds the connect and every blocking step of the handshake. Without it,
  // a server that takes the TCP connection and never finishes the handshake
  // leaves the status bar on "Connecting..." for ever.
  const long timeout = static_cast<long>(config.connect_timeout.count());
  // russh read no ~/.ssh/config, so a host alias there must not change where
  // this connects.
  const bool process_config = false;
  ssh_options_set(s, SSH_OPTIONS_HOST, config.host.c_str());
  ssh_options_set(s, SSH_OPTIONS_PORT, &port);
  ssh_options_set(s, SSH_OPTIONS_USER, config.user.c_str());
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
  ssh_options_set(s, SSH_OPTIONS_PROCESS_CONFIG, &process_config);
  if (config.known_hosts) {
    ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, config.known_hosts->string().c_str());
  }

  events(SessionStatus{"Connecting to " + endpoint(config.host, config.port) + "..."});

  if (ssh_connect(s) != SSH_OK) {
    const std::string cause = ssh_get_error(s);
    if (cause.find("imeout") != std::string::npos) {
      throw SshError::connect_timeout(config.host, config.port, config.connect_timeout);
    }
    throw SshError::connect(config.host, config.port, cause);
  }

  const auto decision =
      decide_host_key(known_host(ssh_session_is_known_server(s)), config.host_key_policy);
  if (decision.action == HostKeyAction::Reject) {
    switch (*decision.rejection) {
      case SshError::Kind::HostKeyChanged:
        throw SshError::host_key_changed(config.host, config.port);
      case SshError::Kind::HostKeyUnknown:
        throw SshError::host_key_unknown(config.host, config.port);
      default: throw SshError::host_key_uncheckable(config.host, config.port, ssh_get_error(s));
    }
  }
  if (decision.action == HostKeyAction::Learn) {
    if (ssh_session_update_known_hosts(s) != SSH_OK) {
      std::cerr << "[ssh] could not record the host key in known_hosts: " << ssh_get_error(s)
                << "\n";
    }
    events(SessionStatus{"Trusting new host key for " + endpoint(config.host, config.port) +
                         " (" + fingerprint(s) + ")"});
  }

  authenticate(s, config);
  // Explicit on purpose: this is the shared SSH account. "Authenticated"
  // would read as if the player's game account were in.
  events(SessionStatus{"SSH connected as " + config.user + " -- game login next"});

  ChannelPtr channel(ssh_channel_new(s));
  if (!channel || ssh_channel_open_session(channel.get()) != SSH_OK ||
      ssh_channel_request_pty_size(channel.get(), config.term.c_str(),
                                   static_cast<int>(config.cols),
                                   static_cast<int>(config.rows)) != SSH_OK ||
      ssh_channel_request_shell(channel.get()) != SSH_OK) {
    throw SshError::protocol(ssh_get_error(s));
  }

  return std::make_unique<SshSession>(std::move(session), std::move(channel), std::move(events));
}

}  // namespace nh::ssh
