#include "core/statelog.hpp"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace nh::statelog {

namespace {

std::string describe(const fs::path& path, const std::error_code& error) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end()) + ": " + error.message();
}

}  // namespace

bool is_allowed_name(std::string_view name) {
  return std::find(kFilenames.begin(), kFilenames.end(), name) != kFilenames.end();
}

void clear(const fs::path& dir) {
  for (const auto name : kFilenames) {
    const fs::path path = dir / name;
    std::error_code error;
    // `remove` reports a missing file as false, not as an error.
    fs::remove(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw StateLogError(StateLogError::Kind::Remove, "removing " + describe(path, error));
    }
  }
}

void write_file(const fs::path& dir, std::string_view name, std::string_view contents) {
  if (!is_allowed_name(name)) {
    throw StateLogError(StateLogError::Kind::ForbiddenName,
                        std::string(name) + " is not a state-log file");
  }
  const fs::path path = dir / name;

  std::error_code error;
  fs::create_directories(dir, error);
  if (error) throw StateLogError(StateLogError::Kind::Write, "writing " + describe(dir, error));

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  if (!out) {
    throw StateLogError(StateLogError::Kind::Write,
                        "writing " + describe(path, std::make_error_code(std::errc::io_error)));
  }
}

void write_files(const fs::path& dir, const std::map<std::string, std::string>& files) {
  for (const auto& [name, contents] : files) write_file(dir, name, contents);
}

fs::path path_from_utf8(std::string_view utf8) {
  return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace nh::statelog
