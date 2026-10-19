#include "net/local.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace nh::local {

LocalError LocalError::not_found() {
  return {Kind::NotFound,
          "no NetHack found on this machine -- install one (`sudo apt install nethack-console`, "
          "`brew install nethack`) or set the command in the profile"};
}

LocalError LocalError::spawn(const std::string& command, const std::string& cause) {
  return {Kind::Spawn, command + " is not something this machine can run: " + cause};
}

LocalError LocalError::pty(const std::string& cause) {
  return {Kind::Pty, "could not open a pseudo-terminal: " + cause};
}

LocalError LocalError::unsupported() {
  return {Kind::Unsupported, "local play is not supported on this platform yet"};
}

namespace {

// Directories to look in beyond PATH. A GUI app on macOS is not started from
// a login shell, so it typically sees only /usr/bin:/bin:/usr/sbin:/sbin.
constexpr const char* kExtraDirs[] = {
    "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/games", "/usr/local/games",
};

// Executable names a local NetHack is installed under. Only vanilla: a
// variant such as NetHack4 numbers its tiles differently, so picking one up
// would draw the wrong map.
constexpr const char* kNames[] = {"nethack", "nethack-console"};

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

// Reads a run of digits at `at`, or nothing if there is none.
std::optional<unsigned> number_at(std::string_view text, std::size_t& at) {
  const std::size_t start = at;
  unsigned value = 0;
  while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
    if (value > 100000) return std::nullopt;
    value = value * 10 + static_cast<unsigned>(text[at] - '0');
    ++at;
  }
  if (at == start) return std::nullopt;
  return value;
}

}  // namespace

std::vector<fs::path> search_dirs(std::optional<std::string_view> path_env) {
  std::vector<fs::path> dirs;
  if (path_env) {
    std::string_view rest = *path_env;
    while (true) {
      const auto end = rest.find(kPathSeparator);
      const auto entry = rest.substr(0, end);
      // An empty entry means the current directory, as std::env::split_paths
      // reads it too.
      dirs.emplace_back(entry.empty() ? fs::path(".") : fs::path(std::string(entry)));
      if (end == std::string_view::npos) break;
      rest.remove_prefix(end + 1);
    }
  }
  for (const char* extra : kExtraDirs) {
    const fs::path dir(extra);
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
  }
  return dirs;
}

std::optional<fs::path> find_in(const std::vector<fs::path>& dirs,
                                const std::function<bool(const fs::path&)>& runnable) {
  for (const auto& dir : dirs) {
    for (const char* name : kNames) {
      fs::path candidate = dir / name;
      if (runnable(candidate)) return candidate;
    }
  }
  return std::nullopt;
}

bool is_runnable(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return false;
#ifdef _WIN32
  return true;
#else
  return access(path.c_str(), X_OK) == 0;
#endif
}

std::optional<fs::path> find() {
  const char* path = std::getenv("PATH");
  return find_in(search_dirs(path ? std::optional<std::string_view>(path) : std::nullopt),
                 is_runnable);
}

std::optional<NetHackVersion> parse_version(std::string_view output) {
  constexpr std::string_view kMarker = "Version ";
  const auto found = output.find(kMarker);
  if (found == std::string_view::npos) return std::nullopt;
  std::size_t at = found + kMarker.size();
  const auto major = number_at(output, at);
  if (!major || at >= output.size()) return std::nullopt;
  ++at;  // the separator, normally '.'
  const auto minor = number_at(output, at);
  if (!minor) return std::nullopt;
  // 3.7 renumbered the tiles and 5.0 continued from there, so anything newer
  // than 3.6 uses the same sheet as 5.0.
  if (*major < 3 || (*major == 3 && *minor <= 6)) return NetHackVersion::V36;
  return NetHackVersion::V50;
}

#ifdef _WIN32

std::optional<NetHackVersion> probe_version(const fs::path&, std::chrono::milliseconds) {
  return std::nullopt;
}

std::unique_ptr<Session> spawn(const Config&, SessionEvents) { throw LocalError::unsupported(); }

#else

namespace {

std::string errno_text(int error) { return std::generic_category().message(error); }

// Marks a descriptor close-on-exec, so a later fork+exec does not inherit
// it. One inherited copy of a pty slave keeps the pty open after the game
// exits, so the master never reports the end and a finished game looks like
// a running one.
bool set_cloexec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  return flags != -1 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

// A pipe whose ends are not inherited. macOS has no pipe2.
bool cloexec_pipe(int fds[2]) {
  if (pipe(fds) != 0) return false;
  if (set_cloexec(fds[0]) && set_cloexec(fds[1])) return true;
  close(fds[0]);
  close(fds[1]);
  return false;
}

// Waits up to `limit` for `pid` to end, then kills it. Returns its status.
int reap(pid_t pid, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  int status = 0;
  while (true) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return status;
    if (done == -1 && errno != EINTR) return 0;  // not ours to wait for any more
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  return status;
}

// The executable `command` names. A bare name is looked up on PATH, as a
// shell would, since execve does not search.
fs::path resolve(const fs::path& command) {
  if (command.native().find('/') != std::string::npos) return command;
  const char* path = std::getenv("PATH");
  if (path) {
    for (const auto& dir : search_dirs(std::string_view(path))) {
      if (is_runnable(dir / command)) return dir / command;
    }
  }
  return command;
}

// How long a hung-up game gets to save and exit before it is killed.
constexpr std::chrono::seconds kHangUpGrace{3};

// One read from the pty. NetHack redraws a screen at a time, so this only
// needs to be comfortably larger than a frame.
constexpr std::size_t kReadBuffer = 16 * 1024;

// Owns the pty master and the game's process, and moves bytes both ways on
// one pump thread.
class LocalSession final : public Session {
 public:
  LocalSession(int master, pid_t pid, int wake_read, int wake_write, SessionEvents events)
      : master_(master),
        pid_(pid),
        wake_read_(wake_read),
        wake_write_(wake_write),
        events_(std::move(events)) {}

  ~LocalSession() override {
    stop_ = true;
    wake();
    if (thread_.joinable()) {
      thread_.join();
    } else {
      // Never started: nobody is listening, so end the game quietly.
      kill(pid_, SIGHUP);
      close(master_);
      master_ = -1;
      reap(pid_, kHangUpGrace);
    }
    if (master_ != -1) close(master_);
    close(wake_read_);
    close(wake_write_);
  }

  void start() override {
    if (!thread_.joinable()) thread_ = std::thread([this] { pump(); });
  }

  void write(std::string bytes) override {
    {
      std::lock_guard lock(mutex_);
      if (closed_) throw Disconnected();
      commands_.push_back({Command::Kind::Data, std::move(bytes), 0, 0});
    }
    wake();
  }

  void resize(std::uint32_t cols, std::uint32_t rows) override {
    {
      std::lock_guard lock(mutex_);
      if (closed_) throw Disconnected();
      commands_.push_back({Command::Kind::Resize, {}, cols, rows});
    }
    wake();
  }

  void disconnect() override {
    stop_ = true;
    wake();
  }

  bool is_connected() const override { return !closed_; }

 private:
  struct Command {
    enum class Kind { Data, Resize } kind;
    std::string bytes;
    std::uint32_t cols;
    std::uint32_t rows;
  };

  void wake() {
    const char byte = 0;
    // A full pipe already means a wake-up is pending.
    [[maybe_unused]] const auto n = ::write(wake_write_, &byte, 1);
  }

  void drain_wake() {
    char buffer[64];
    while (read(wake_read_, buffer, sizeof buffer) > 0) {
    }
  }

  // Moves queued keys to `pending_` and applies resizes.
  void take_commands() {
    std::deque<Command> batch;
    {
      std::lock_guard lock(mutex_);
      batch.swap(commands_);
    }
    for (auto& command : batch) {
      if (command.kind == Command::Kind::Data) {
        pending_ += command.bytes;
        continue;
      }
      // NetHack redraws on SIGWINCH, which the kernel sends when this works.
      winsize size{};
      size.ws_col = static_cast<unsigned short>(command.cols);
      size.ws_row = static_cast<unsigned short>(command.rows);
      if (ioctl(master_, TIOCSWINSZ, &size) == -1) {
        std::cerr << "[local] window change failed: " << errno_text(errno) << "\n";
      }
    }
  }

  // Reads what the game wrote. Returns false once the game has gone: EOF,
  // or EIO, which is how Linux reports the last slave closing.
  bool read_output(char* buffer) {
    while (true) {
      const ssize_t n = read(master_, buffer, kReadBuffer);
      if (n > 0) {
        events_(SessionData{std::string(buffer, static_cast<std::size_t>(n))});
        continue;
      }
      if (n == -1 && errno == EINTR) continue;
      if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
      return false;
    }
  }

  void write_pending() {
    while (!pending_.empty()) {
      const ssize_t n = ::write(master_, pending_.data(), pending_.size());
      if (n > 0) {
        pending_.erase(0, static_cast<std::size_t>(n));
      } else if (n == -1 && errno == EINTR) {
        continue;
      } else {
        // EAGAIN waits for the next POLLOUT. Anything else means the game
        // is going, and the read side will see it.
        if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) pending_.clear();
        return;
      }
    }
  }

  void pump() {
    std::vector<char> buffer(kReadBuffer);
    bool hung_up = false;
    std::chrono::steady_clock::time_point hung_up_at;
    bool killed = false;

    while (true) {
      if (stop_ && !hung_up) {
        // SIGHUP and not SIGKILL: NetHack answers a hangup by saving the
        // game. Killing it would lose the character and leave a lock file
        // that blocks the next start.
        kill(pid_, SIGHUP);
        hung_up = true;
        hung_up_at = std::chrono::steady_clock::now();
        pending_.clear();
      }
      if (hung_up && !killed && std::chrono::steady_clock::now() - hung_up_at >= kHangUpGrace) {
        kill(pid_, SIGKILL);
        killed = true;
      }
      if (!hung_up) {
        take_commands();
        write_pending();
      }

      pollfd fds[2] = {
          {master_, static_cast<short>(POLLIN | (pending_.empty() ? 0 : POLLOUT)), 0},
          {wake_read_, POLLIN, 0},
      };
      const int ready = poll(fds, 2, hung_up ? 100 : -1);
      if (ready == -1 && errno != EINTR) break;
      if (ready <= 0) continue;
      if (fds[1].revents & POLLIN) drain_wake();
      if (fds[0].revents & POLLOUT) write_pending();
      if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_output(buffer.data())) break;
      if (fds[0].revents & POLLNVAL) break;
    }

    // The pty has ended, so the game is gone or going. Reap it and say why.
    const int status = reap(pid_, kHangUpGrace);
    std::optional<std::string> reason;
    if (!hung_up) {
      if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        reason = "NetHack exited with status " + std::to_string(WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        reason = "NetHack was stopped by signal " + std::to_string(WTERMSIG(status)) + " (" +
                 strsignal(WTERMSIG(status)) + ")";
      }
    }
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      commands_.clear();
    }
    events_(SessionClosed{reason});
  }

  int master_;
  pid_t pid_;
  int wake_read_;
  int wake_write_;
  SessionEvents events_;
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Command> commands_;
  // Keys not yet taken by the pty. Pump thread only.
  std::string pending_;
  std::atomic<bool> stop_ = false;
  std::atomic<bool> closed_ = false;
};

}  // namespace

std::optional<NetHackVersion> probe_version(const fs::path& command,
                                            std::chrono::milliseconds limit) {
  int out[2];
  if (!cloexec_pipe(out)) return std::nullopt;

  // Some builds print the banner on stderr, so both go to the one pipe.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, out[1], 2);

  const std::string path = resolve(command).string();
  std::string flag = "--version";
  char* argv[] = {const_cast<char*>(path.c_str()), flag.data(), nullptr};
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out[1]);
  if (rc != 0) {
    close(out[0]);
    return std::nullopt;
  }

  // Read until EOF or the deadline. A probe still running then is killed:
  // something on PATH called `nethack` that does not answer must not hold
  // up the window.
  std::string text;
  const auto deadline = std::chrono::steady_clock::now() + limit;
  char buffer[4096];
  while (text.size() < 64 * 1024) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) break;
    pollfd fd{out[0], POLLIN, 0};
    const int ready = poll(&fd, 1, static_cast<int>(left.count()));
    if (ready == -1 && errno == EINTR) continue;
    if (ready <= 0) break;
    const ssize_t n = read(out[0], buffer, sizeof buffer);
    if (n == -1 && errno == EINTR) continue;
    if (n <= 0) break;
    text.append(buffer, static_cast<std::size_t>(n));
  }
  close(out[0]);
  // Whatever is left of the limit, so a probe never takes much longer.
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  reap(pid, std::max(left, std::chrono::milliseconds(0)));
  return parse_version(text);
}

std::pair<int, int> detail::open_pty(std::uint16_t cols, std::uint16_t rows) {
  winsize size{};
  size.ws_col = cols;
  size.ws_row = rows;
  int master = -1;
  int slave = -1;
  if (openpty(&master, &slave, nullptr, nullptr, &size) != 0) {
    throw LocalError::pty(errno_text(errno));
  }
  // There is still a hair of a window between openpty and these calls in
  // which another thread could fork; openpty offers no atomic way to close it.
  if (!set_cloexec(master) || !set_cloexec(slave)) {
    const int error = errno;
    close(master);
    close(slave);
    throw LocalError::pty(errno_text(error));
  }
  return {master, slave};
}

std::unique_ptr<Session> spawn(const Config& config, SessionEvents events) {
  const std::string display = config.command.string();
  const fs::path path = resolve(config.command);

  // Everything the child needs is made before the fork: between fork and
  // exec, a threaded process may only make async-signal-safe calls.
  std::vector<std::string> arg_strings{path.string()};
  arg_strings.insert(arg_strings.end(), config.args.begin(), config.args.end());
  std::vector<char*> argv;
  for (auto& arg : arg_strings) argv.push_back(arg.data());
  argv.push_back(nullptr);

  std::vector<std::string> env_strings{"TERM=" + config.term};
  for (char** entry = environ; entry && *entry; ++entry) {
    if (std::strncmp(*entry, "TERM=", 5) != 0) env_strings.emplace_back(*entry);
  }
  std::vector<char*> envp;
  for (auto& entry : env_strings) envp.push_back(entry.data());
  envp.push_back(nullptr);

  const auto [master, slave] = detail::open_pty(config.cols, config.rows);
  if (!set_nonblocking(master)) {
    const int error = errno;
    close(master);
    close(slave);
    throw LocalError::pty(errno_text(error));
  }

  // The child writes its errno here if exec fails. The write end closes on a
  // successful exec, so the parent reads EOF.
  int failed[2];
  int wake[2];
  if (!cloexec_pipe(failed)) {
    const int error = errno;
    close(master);
    close(slave);
    throw LocalError::pty(errno_text(error));
  }
  if (!cloexec_pipe(wake) || !set_nonblocking(wake[0]) || !set_nonblocking(wake[1])) {
    const int error = errno;
    close(master);
    close(slave);
    close(failed[0]);
    close(failed[1]);
    throw LocalError::pty(errno_text(error));
  }

  const pid_t pid = fork();
  if (pid == 0) {
    // The child. Signals the app ignores or blocks would stay that way
    // through exec, and NetHack needs SIGHUP and SIGWINCH.
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, nullptr);
    for (const int sig : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD, SIGWINCH}) {
      signal(sig, SIG_DFL);
    }
    int error = 0;
    // A new session, so the game is out of the app's process group, then
    // the pty as its controlling terminal, without which NetHack cannot read
    // keys or catch SIGWINCH.
    if (setsid() == -1 || ioctl(slave, TIOCSCTTY, 0) == -1 || dup2(slave, 0) == -1 ||
        dup2(slave, 1) == -1 || dup2(slave, 2) == -1) {
      error = errno;
    } else {
      execve(argv[0], argv.data(), envp.data());
      error = errno;
    }
    [[maybe_unused]] const auto n = ::write(failed[1], &error, sizeof error);
    _exit(127);
  }

  const int fork_error = errno;
  // Held open only for the child to inherit. Keeping a copy would mean the
  // master never sees the end when the game exits.
  close(slave);
  close(failed[1]);
  if (pid == -1) {
    close(master);
    close(failed[0]);
    close(wake[0]);
    close(wake[1]);
    throw LocalError::spawn(display, errno_text(fork_error));
  }

  int child_error = 0;
  ssize_t n = 0;
  do {
    n = read(failed[0], &child_error, sizeof child_error);
  } while (n == -1 && errno == EINTR);
  close(failed[0]);
  if (n > 0) {
    reap(pid, std::chrono::seconds(1));
    close(master);
    close(wake[0]);
    close(wake[1]);
    throw LocalError::spawn(display, errno_text(child_error));
  }

  events(SessionStatus{"Playing " + display + " on this machine"});
  return std::make_unique<LocalSession>(master, pid, wake[0], wake[1], std::move(events));
}

#endif

}  // namespace nh::local
