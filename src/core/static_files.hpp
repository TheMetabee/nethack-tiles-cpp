#pragma once

// Finds the file a URL path from the page names, for a webview that serves
// the built frontend itself (a WebKitGTK URI scheme handler). Windows does
// not need it: WebView2 maps a folder on its own.

#include <filesystem>
#include <optional>
#include <string_view>

namespace nh {

struct StaticFile {
  std::filesystem::path path;
  // The Content-Type to send with it.
  const char* mime;
};

// Maps `url_path` ("/assets/index-x.js", with any query or fragment) to a
// file under `root`. "/" names index.html. Returns nothing for a path that
// could leave `root`: a ".." part, a backslash, a colon or a NUL byte, also
// when percent-encoded. Whether the file exists is the caller's question.
std::optional<StaticFile> resolve_static_file(const std::filesystem::path& root,
                                              std::string_view url_path);

// The MIME type for a file name, by its extension. ES module scripts must
// come with a JavaScript type, or the webview refuses to run them.
const char* mime_type(const std::filesystem::path& file);

}  // namespace nh
