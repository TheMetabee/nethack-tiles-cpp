// Port of the tests in src-tauri/src/local.rs. The Rust app's `with_timeout`
// tests become tests of the probe itself, which here kills a probe that runs
// too long instead of leaving a thread behind.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "net/local.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;
using namespace nh::local;
using nh::NetHackVersion;

namespace {

// A PATH written the way this system writes one.
std::string path_env(const char* first, const char* second) {
#ifdef _WIN32
  return std::string(first) + ";" + second;
#else
  return std::string(first) + ":" + second;
#endif
}

bool contains(const std::vector<fs::path>& dirs, const fs::path& dir) {
  return std::find(dirs.begin(), dirs.end(), dir) != dirs.end();
}

}  // namespace

TEST_CASE("the search starts with the inherited path") {
  const auto dirs = search_dirs(path_env("/first", "/second"));
  REQUIRE(dirs.size() >= 2);
  CHECK(dirs[0] == fs::path("/first"));
  CHECK(dirs[1] == fs::path("/second"));
}

TEST_CASE("the usual install locations are searched even when not on path") {
  // A GUI app on macOS inherits a bare PATH, so Homebrew's bin directory has
  // to be looked in explicitly or a local NetHack is never found.
  const auto dirs = search_dirs("/usr/bin");
  CHECK(contains(dirs, "/opt/homebrew/bin"));
  CHECK(contains(dirs, "/usr/games"));
}

TEST_CASE("a directory on path is not searched twice") {
  const auto dirs = search_dirs("/usr/games");
  CHECK(std::count(dirs.begin(), dirs.end(), fs::path("/usr/games")) == 1);
}

TEST_CASE("a missing path still searches the usual locations") {
  CHECK_FALSE(search_dirs(std::nullopt).empty());
}

TEST_CASE("the first runnable nethack in order wins") {
  const std::vector<fs::path> dirs{"/a", "/b"};
  const auto found = find_in(dirs, [](const fs::path& p) { return p == fs::path("/b/nethack"); });
  CHECK(found == fs::path("/b/nethack"));
}

TEST_CASE("nothing runnable means nothing found") {
  CHECK_FALSE(find_in({"/a"}, [](const fs::path&) { return false; }));
}

TEST_CASE("a file that is not executable is not a nethack") {
  // `nethack` is also the name of a directory in some installs.
  CHECK_FALSE(is_runnable("/"));
}

TEST_CASE("version 3.6 maps to the 3.6 tile sheet") {
  CHECK(parse_version("NetHack Version 3.6.7 - last build Wed Feb 16 12:00:00 2023.") ==
        NetHackVersion::V36);
}

TEST_CASE("version 5 maps to the 5.0 tile sheet") {
  CHECK(parse_version("NetHack Version 5.0.0 - last build Mon Jan  1 00:00:00 2026.") ==
        NetHackVersion::V50);
}

TEST_CASE("3.7 shares the 5.0 sheet") {
  // 3.7 is where the tile list was renumbered; 5.0 continued from it.
  CHECK(parse_version("NetHack Version 3.7.0-0 - last build Mon Jan  1 00:00:00 2025.") ==
        NetHackVersion::V50);
}

TEST_CASE("the banner a real build prints is understood") {
  // From `nethack --version` on a Homebrew install: the platform prefix is
  // what a stricter parser would trip over.
  CHECK(parse_version("MacOSX NetHack Version 3.6.7 - last build Fri Feb 24 12:33:19 2023.") ==
        NetHackVersion::V36);
  // And Debian's nethack-console, as Ubuntu 24.04 installs it.
  CHECK(parse_version("Unix NetHack Version 3.6.7 - last build Thu Mar 30 15:38:49 2023. "
                      "(18e7c1dbb6bf4f4e5ec3c8a9c4d1cc7a4ab2fd29)\n") == NetHackVersion::V36);
}

TEST_CASE("output with no version in it is not guessed at") {
  CHECK_FALSE(parse_version("command not found"));
  CHECK_FALSE(parse_version(""));
  CHECK_FALSE(parse_version("Version x.y"));
  CHECK_FALSE(parse_version("Version 3"));
}

TEST_CASE("an error says what to install") {
  CHECK(std::string(LocalError::not_found().what()).find("install") != std::string::npos);
}

#ifdef _WIN32

TEST_CASE("local play says it is unsupported on windows rather than failing oddly") {
  try {
    spawn(Config{"nethack"}, [](nh::SessionEvent) {});
    FAIL("spawn must throw");
  } catch (const LocalError& e) {
    CHECK(e.kind() == LocalError::Kind::Unsupported);
  }
}

#else

namespace {

// A shell script in a fresh temporary directory, removed afterwards.
class Script {
 public:
  explicit Script(const std::string& body) {
    std::string pattern = (fs::temp_directory_path() / "nh-local-test-XXXXXX").string();
    REQUIRE(mkdtemp(pattern.data()) != nullptr);
    dir_ = pattern;
    path_ = dir_ / "nethack";
    std::ofstream(path_) << "#!/bin/sh\n" << body << "\n";
    fs::permissions(path_, fs::perms::owner_all);
  }
  ~Script() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path dir_;
  fs::path path_;
};

// Collects a session's events until it closes, or a deadline passes.
class Collector {
 public:
  nh::SessionEvents events() {
    return [this](nh::SessionEvent event) {
      std::lock_guard lock(mutex_);
      if (auto* data = std::get_if<nh::SessionData>(&event)) text_ += data->bytes;
      if (auto* closed = std::get_if<nh::SessionClosed>(&event)) {
        closed_ = true;
        reason_ = closed->reason;
      }
      changed_.notify_all();
    };
  }

  // Waits for the close. Fails the test instead of hanging on a child that
  // misbehaves.
  void wait_closed() {
    std::unique_lock lock(mutex_);
    const bool done =
        changed_.wait_for(lock, std::chrono::seconds(10), [this] { return closed_; });
    INFO("got so far: " << text_);
    REQUIRE(done);
  }

  void wait_for_text(const std::string& wanted) {
    std::unique_lock lock(mutex_);
    const bool done = changed_.wait_for(lock, std::chrono::seconds(10), [&] {
      return text_.find(wanted) != std::string::npos;
    });
    INFO("got so far: " << text_);
    REQUIRE(done);
  }

  std::string text() {
    std::lock_guard lock(mutex_);
    return text_;
  }
  std::optional<std::string> reason() {
    std::lock_guard lock(mutex_);
    return reason_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string text_;
  bool closed_ = false;
  std::optional<std::string> reason_;
};

Config command(const char* path, std::vector<std::string> args = {}) {
  Config config;
  config.command = path;
  config.args = std::move(args);
  return config;
}

}  // namespace

TEST_CASE("a quick probe returns its answer") {
  Script script("echo 'Unix NetHack Version 3.6.7 - last build'");
  CHECK(probe_version(script.path()) == NetHackVersion::V36);
}

TEST_CASE("a probe reads a banner printed on stderr") {
  Script script("echo 'NetHack Version 5.0.0' >&2");
  CHECK(probe_version(script.path()) == NetHackVersion::V50);
}

TEST_CASE("a probe that will not finish gives up instead of holding up startup") {
  Script script("sleep 30");
  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(probe_version(script.path(), std::chrono::milliseconds(200)));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("a probe of something that is not there finds no version") {
  CHECK_FALSE(probe_version("/nonexistent/nethack"));
}

TEST_CASE("a runnable script is runnable") {
  Script script("true");
  CHECK(is_runnable(script.path()));
  fs::permissions(script.path(), fs::perms::owner_read | fs::perms::owner_write);
  CHECK_FALSE(is_runnable(script.path()));
}

// A pty must not survive into a process that has nothing to do with it. One
// leaked copy of the slave keeps the pty open forever: the game exits, and
// the master never reports the end, so a finished game would look like one
// still being played.
TEST_CASE("a pty is not inherited by unrelated children") {
  const auto [master, slave] = detail::open_pty(80, 24);

  // Something else in the process starts a program, as the version probe
  // does. It has no business holding this pty.
  pid_t bystander = 0;
  char sleep_path[] = "/bin/sleep";
  char seconds[] = "30";
  char* argv[] = {sleep_path, seconds, nullptr};
  REQUIRE(posix_spawn(&bystander, sleep_path, nullptr, nullptr, argv, environ) == 0);
  close(slave);

  // Nothing holds the slave now, so the master must report the end at once.
  pollfd fd{master, POLLIN, 0};
  const int ready = poll(&fd, 1, 5000);
  char byte = 0;
  const bool ended = ready == 1 && read(master, &byte, 1) <= 0;

  kill(bystander, SIGKILL);
  waitpid(bystander, nullptr, 0);
  close(master);
  CHECK_MESSAGE(ended, "the pty outlived its slave: a bystander process inherited it");
}

TEST_CASE("a local command runs and its output comes back") {
  Collector collector;
  auto session = spawn(command("/bin/echo", {"hello"}), collector.events());
  session->start();
  collector.wait_closed();
  CHECK(collector.text().find("hello") != std::string::npos);
  // A clean exit needs no explanation.
  CHECK_FALSE(collector.reason());
}

TEST_CASE("a bare command name is found on path") {
  Collector collector;
  auto session = spawn(command("echo", {"found"}), collector.events());
  session->start();
  collector.wait_closed();
  CHECK(collector.text().find("found") != std::string::npos);
}

TEST_CASE("the child gets a real terminal of the size we asked for") {
  // `stty size` fails outright without a controlling terminal, so this covers
  // both halves: the pty exists, and NetHack will see 40x100.
  Collector collector;
  auto config = command("/bin/stty", {"size"});
  config.cols = 100;
  config.rows = 40;
  auto session = spawn(config, collector.events());
  session->start();
  collector.wait_closed();
  CHECK(collector.text().find("40 100") != std::string::npos);
}

TEST_CASE("the child is told its terminal type") {
  Collector collector;
  auto config = command("/bin/sh", {"-c", "echo \"term=$TERM\""});
  config.term = "xterm-test";
  auto session = spawn(config, collector.events());
  session->start();
  collector.wait_closed();
  CHECK(collector.text().find("term=xterm-test") != std::string::npos);
}

TEST_CASE("a resize reaches the game") {
  Collector collector;
  auto session = spawn(command("/bin/sh", {"-c", "read line; stty size"}), collector.events());
  session->start();
  session->resize(132, 50);
  session->write("go\n");
  collector.wait_closed();
  CHECK(collector.text().find("50 132") != std::string::npos);
}

TEST_CASE("a failing game says so rather than closing silently") {
  Collector collector;
  auto session = spawn(command("/bin/sh", {"-c", "exit 3"}), collector.events());
  session->start();
  collector.wait_closed();
  const auto reason = collector.reason();
  REQUIRE(reason);
  CHECK(reason->find('3') != std::string::npos);
}

TEST_CASE("a command this machine cannot run is an error not a hang") {
  try {
    spawn(command("/nonexistent/nethack"), [](nh::SessionEvent) {});
    FAIL("must not pretend to have started");
  } catch (const LocalError& e) {
    CHECK(e.kind() == LocalError::Kind::Spawn);
    CHECK(std::string(e.what()).find("/nonexistent/nethack") != std::string::npos);
  }
}

TEST_CASE("keystrokes reach the child") {
  Collector collector;
  auto session = spawn(command("/bin/cat"), collector.events());
  session->start();
  session->write("knock\n");
  collector.wait_for_text("knock");
  session->disconnect();
  collector.wait_closed();
}

TEST_CASE("disconnecting hangs up on the game, which can then save") {
  // NetHack saves on SIGHUP. This stand-in says it did, then exits.
  Collector collector;
  auto session = spawn(command("/bin/sh", {"-c", "trap 'echo saved; exit 0' HUP; echo ready; "
                                                 "while :; do sleep 0.1; done"}),
                       collector.events());
  session->start();
  collector.wait_for_text("ready");
  session->disconnect();
  collector.wait_closed();
  // A hangup the player asked for is not an error.
  CHECK_FALSE(collector.reason());
  CHECK_FALSE(session->is_connected());
  CHECK_THROWS_AS(session->write("x"), nh::Disconnected);
}

TEST_CASE("a session dropped before it starts ends its game") {
  Script script("trap 'exit 0' HUP; while :; do sleep 0.1; done");
  auto session = spawn(command(script.path().c_str()), [](nh::SessionEvent) {});
  const auto start = std::chrono::steady_clock::now();
  session.reset();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

// Starts the NetHack installed on this machine and checks it draws
// something. Skipped unless NH_TEST_LOCAL_NETHACK=1: it depends on the
// machine having one, and it leaves a save file behind.
TEST_CASE("the installed nethack starts and draws" *
          doctest::skip(std::string_view(std::getenv("NH_TEST_LOCAL_NETHACK")
                                             ? std::getenv("NH_TEST_LOCAL_NETHACK")
                                             : "") != "1")) {
  const auto found = find();
  REQUIRE_MESSAGE(found, "no NetHack installed to test against");
  Collector collector;
  Config config;
  config.command = *found;
  auto session = spawn(config, collector.events());
  session->start();
  // The tty interface paints first of all, which emits an escape sequence.
  collector.wait_for_text("\x1b");
  MESSAGE(found->string() << " drew " << collector.text().size() << " bytes");
  session->disconnect();
  collector.wait_closed();
}

#endif
