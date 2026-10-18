#include "core/static_files.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace fs = std::filesystem;

namespace nh {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes %XX escapes. Returns nothing for a broken escape.
std::optional<std::string> percent_decode(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out += text[i];
      continue;
    }
    if (i + 2 >= text.size()) return std::nullopt;
    const int high = hex_value(text[i + 1]), low = hex_value(text[i + 2]);
    if (high < 0 || low < 0) return std::nullopt;
    out += static_cast<char>(high * 16 + low);
    i += 2;
  }
  return out;
}

}  // namespace

std::optional<StaticFile> resolve_static_file(const fs::path& root, std::string_view url_path) {
  url_path = url_path.substr(0, url_path.find_first_of("?#"));
  // Decode first and check after, so an encoded ".." cannot slip past.
  const auto decoded = percent_decode(url_path);
  if (!decoded) return std::nullopt;
  if (decoded->find_first_of(std::string_view("\\:\0", 3)) != std::string::npos) {
    return std::nullopt;
  }

  fs::path relative;
  for (std::size_t from = 0; from <= decoded->size();) {
    std::size_t slash = decoded->find('/', from);
    if (slash == std::string::npos) slash = decoded->size();
    const std::string part = decoded->substr(from, slash - from);
    from = slash + 1;
    if (part.empty()) continue;
    if (part == "." || part == "..") return std::nullopt;
    // URL paths are UTF-8; a plain std::string path is not, on Windows.
    relative /= fs::path(std::u8string(part.begin(), part.end()));
  }
  if (relative.empty()) relative = "index.html";

  fs::path path = root / relative;
  return StaticFile{path, mime_type(path)};
}

const char* mime_type(const fs::path& file) {
  std::string extension = file.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension == ".html" || extension == ".htm") return "text/html";
  if (extension == ".js" || extension == ".mjs") return "text/javascript";
  if (extension == ".css") return "text/css";
  if (extension == ".json" || extension == ".map") return "application/json";
  if (extension == ".png") return "image/png";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".ttf") return "font/ttf";
  if (extension == ".woff") return "font/woff";
  if (extension == ".woff2") return "font/woff2";
  if (extension == ".txt") return "text/plain";
  return "application/octet-stream";
}

}  // namespace nh
