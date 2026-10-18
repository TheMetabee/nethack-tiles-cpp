// Port of the tests in src-tauri/src/statelog.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/statelog.hpp"

namespace fs = std::filesystem;
using namespace nh::statelog;

namespace {

class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    std::ostringstream name;
    name << "nethack-tiles-statelog-" << tag << "-" << std::this_thread::get_id();
    path = fs::temp_directory_path() / name.str();
    std::error_code ignored;
    fs::remove_all(path, ignored);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  fs::path path;
};

std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

TEST_CASE("a known file is written into the directory") {
  TempDir dir("write");
  write_file(dir.path, "screen.txt", "@\n");
  CHECK(read(dir.path / "screen.txt") == "@\n");
}

TEST_CASE("the container contents file is a known name") {
  TempDir dir("containers");
  write_file(dir.path, "containers.txt", "Contents of the sack:\n");
  CHECK(read(dir.path / "containers.txt") == "Contents of the sack:\n");
}

TEST_CASE("an unknown name is refused") {
  TempDir dir("forbid");
  try {
    write_file(dir.path, "secret.txt", "nope");
    FAIL("an unknown name was written");
  } catch (const StateLogError& e) {
    CHECK(e.kind() == StateLogError::Kind::ForbiddenName);
  }
  CHECK_FALSE(fs::exists(dir.path / "secret.txt"));
}

TEST_CASE("a path separator in the name is refused") {
  TempDir dir("sep");
  CHECK_THROWS_AS(write_file(dir.path, "../screen.txt", "nope"), StateLogError);
  CHECK_THROWS_AS(write_file(dir.path, "sub/screen.txt", "nope"), StateLogError);
  CHECK_THROWS_AS(write_file(dir.path, "sub\\screen.txt", "nope"), StateLogError);
}

TEST_CASE("clear removes state files and leaves other files") {
  TempDir dir("clear");
  write_file(dir.path, "screen.txt", "@\n");
  write_file(dir.path, "messages.txt", "hi\n");
  std::ofstream(dir.path / "notes.md") << "mine";

  clear(dir.path);

  CHECK_FALSE(fs::exists(dir.path / "screen.txt"));
  CHECK_FALSE(fs::exists(dir.path / "messages.txt"));
  CHECK(read(dir.path / "notes.md") == "mine");
}

TEST_CASE("clear of a missing directory is fine") {
  TempDir dir("missing");
  CHECK_NOTHROW(clear(dir.path / "nope"));
}

TEST_CASE("write files replaces several names at once") {
  TempDir dir("batch");
  write_files(dir.path, {{"screen.txt", "a\n"}, {"messages.txt", "b\n"}});
  CHECK(read(dir.path / "screen.txt") == "a\n");
  CHECK(read(dir.path / "messages.txt") == "b\n");
}

// Not in the Rust suite, where every string is UTF-8 already. Here the page's
// UTF-8 must not be read in the ANSI code page.
TEST_CASE("a directory name outside ascii is written where the page said") {
  TempDir dir("utf8");
  const std::string name = "\xc3\xa9t\xc3\xa9";  // "été" in UTF-8
  write_file(dir.path / path_from_utf8(name), "screen.txt", "@\n");
  CHECK(fs::exists(dir.path / fs::path(L"été") / "screen.txt"));
}
