// The URL-to-file lookup behind the Linux webview's URI scheme. No Rust
// counterpart: Tauri did this inside its own protocol handler.

#include <doctest/doctest.h>

#include <string>

#include "core/static_files.hpp"

namespace fs = std::filesystem;
using nh::mime_type;
using nh::resolve_static_file;

namespace {

const fs::path kRoot = fs::path("dist");

std::string resolved(std::string_view url_path) {
  const auto file = resolve_static_file(kRoot, url_path);
  return file ? file->path.generic_string() : std::string("<refused>");
}

}  // namespace

TEST_CASE("a path names the file under the root") {
  CHECK(resolved("/assets/index-BMkvyeMa.js") == "dist/assets/index-BMkvyeMa.js");
  CHECK(resolved("/index.html") == "dist/index.html");
}

TEST_CASE("the root and an empty path name index.html") {
  CHECK(resolved("/") == "dist/index.html");
  CHECK(resolved("") == "dist/index.html");
}

TEST_CASE("a query or a fragment is not part of the file name") {
  CHECK(resolved("/assets/app.css?v=3") == "dist/assets/app.css");
  CHECK(resolved("/index.html#top") == "dist/index.html");
}

TEST_CASE("a path that climbs out of the root is refused") {
  CHECK(resolved("/../secret.txt") == "<refused>");
  CHECK(resolved("/assets/../../secret.txt") == "<refused>");
  CHECK(resolved("/assets/..") == "<refused>");
}

TEST_CASE("a percent-encoded climb is refused too") {
  // A handler that decoded after checking would let this through.
  CHECK(resolved("/%2e%2e/secret.txt") == "<refused>");
  CHECK(resolved("/assets/%2E%2E/%2E%2E/secret.txt") == "<refused>");
  CHECK(resolved("/..%2fsecret.txt") == "<refused>");
}

TEST_CASE("backslashes colons and nul bytes are refused") {
  CHECK(resolved("/assets\\..\\secret.txt") == "<refused>");
  CHECK(resolved("/C:/Windows/win.ini") == "<refused>");
  CHECK(resolved("/index.html%00.png") == "<refused>");
}

TEST_CASE("a percent-encoded name is decoded") {
  CHECK(resolved("/assets/a%20b.png") == "dist/assets/a b.png");
}

TEST_CASE("a broken percent escape is refused") {
  CHECK(resolved("/assets/a%2.png") == "<refused>");
  CHECK(resolved("/assets/a%zz.png") == "<refused>");
}

TEST_CASE("the found file carries its mime type") {
  const auto file = resolve_static_file(kRoot, "/assets/index-x.js");
  REQUIRE(file);
  CHECK(std::string(file->mime) == "text/javascript");
}

TEST_CASE("each kind of file the frontend build makes has its mime type") {
  CHECK(std::string(mime_type("index.html")) == "text/html");
  CHECK(std::string(mime_type("a.js")) == "text/javascript");
  CHECK(std::string(mime_type("a.mjs")) == "text/javascript");
  CHECK(std::string(mime_type("a.css")) == "text/css");
  CHECK(std::string(mime_type("a.json")) == "application/json");
  CHECK(std::string(mime_type("PTMono-Regular-CKCVTacG.ttf")) == "font/ttf");
  CHECK(std::string(mime_type("a.woff2")) == "font/woff2");
  CHECK(std::string(mime_type("a.png")) == "image/png");
  CHECK(std::string(mime_type("a.svg")) == "image/svg+xml");
}

TEST_CASE("the extension is matched without regard to case") {
  CHECK(std::string(mime_type("INDEX.HTML")) == "text/html");
}

TEST_CASE("an unknown kind of file is sent as plain bytes") {
  CHECK(std::string(mime_type("a.xyz")) == "application/octet-stream");
  CHECK(std::string(mime_type("README")) == "application/octet-stream");
}
