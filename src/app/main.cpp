// Hosts the React + xterm.js frontend in a system webview and answers its
// commands. Replaces the Tauri app shell (src-tauri/src/app.rs).
//
// Spike scope: tilesets, custom tilesets, the state log and default profiles
// are real; the session is a demo that draws one room through the ported
// demuxer and echoes keys. SSH, saved profiles and the keychain are not
// ported yet.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "app/bridge.hpp"
#include "core/demux.hpp"
#include "core/debuglog.hpp"
#include "core/glyph.hpp"
#include "core/statelog.hpp"
#include "core/tileset.hpp"

#ifdef _WIN32
#include <windows.h>
// After windows.h, which they depend on.
#include <shellapi.h>
#include <shobjidl.h>
#endif

namespace fs = std::filesystem;
using nh::app::Bridge;
using nh::app::json;

namespace {

// --- Files ------------------------------------------------------------------

fs::path exe_dir() {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return fs::path(buffer).parent_path();
#endif
  return fs::current_path();
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// --- Tilesets ---------------------------------------------------------------

using Tilesets = std::map<std::string, nh::Tileset>;

// Each bundled sheet is `<id>.json` (the manifest the frontend reads) beside
// `<id>.png`. A sheet that does not match its manifest is left out and
// logged: it would draw the wrong picture for almost every glyph.
Tilesets load_tilesets(const fs::path& dir) {
  Tilesets sheets;
  if (!fs::is_directory(dir)) return sheets;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() != ".json") continue;
    fs::path png = entry.path();
    png.replace_extension(".png");
    try {
      auto manifest = json::parse(read_file(entry.path())).get<nh::TilesetManifest>();
      const std::string id = manifest.id;
      sheets.insert_or_assign(id, nh::Tileset::load(std::move(manifest), read_file(png)));
    } catch (const std::exception& e) {
      std::cerr << "[app] the bundled tileset " << entry.path().string() << " is unusable: "
                << e.what() << "\n";
    }
  }
  return sheets;
}

json tileset_payload(const nh::Tileset& sheet) {
  return {{"manifest", sheet.manifest()}, {"dataUrl", sheet.data_url()}};
}

// --- Profiles (defaults only; storage is not ported yet) ---------------------

json make_profile(const std::string& id, const std::string& name, const std::string& host,
                  const std::string& version, const Tilesets& sheets) {
  std::string tileset_id;
  for (const auto& [sheet_id, sheet] : sheets) {
    if (json(sheet.manifest().version) == version) tileset_id = sheet_id;
  }
  return {
      {"id", id},           {"name", name},
      {"transport", "ssh"}, {"command", ""},
      {"host", host},       {"port", 22},
      {"sshUser", "nethack"}, {"gameUser", ""},
      {"version", version}, {"tilesetId", tileset_id},
      {"fontFamily", "Menlo, DejaVu Sans Mono, Consolas, monospace"},
      {"fontSize", 16},     {"scale", 1.0},
      {"lineHeight", 1.0},  {"letterSpacing", 0.0},
      {"pixelPerfect", false}, {"stateLogEnabled", false},
      {"stateLogDirectory", ""},
  };
}

// --- Stream items as the frontend expects them (see protocol.ts) -------------

json to_json(const nh::GlyphFlags& f) {
  return {
      {"hero", f.hero},         {"corpse", f.corpse},     {"invisible", f.invisible},
      {"detected", f.detected}, {"pet", f.pet},           {"ridden", f.ridden},
      {"statue", f.statue},     {"objpile", f.objpile},   {"bwLava", f.bw_lava},
      {"nothing", f.nothing},   {"unexplored", f.unexplored}, {"female", f.female},
  };
}

json to_json(const nh::StreamItem& item, nh::NetHackVersion version) {
  if (const auto* text = std::get_if<nh::Text>(&item)) {
    return {{"type", "text"},
            {"bytes", nh::app::latin1_to_utf8(text->bytes)},
            {"prints", text->prints}};
  }
  const auto& event = std::get<nh::Event>(item).event;
  json e;
  if (const auto* g = std::get_if<nh::GlyphStart>(&event)) {
    e = {{"kind", "glyphStart"},
         {"tile", g->tile},
         {"flags", to_json(nh::GlyphFlags::decode(g->flags, version))},
         {"rawFlags", g->flags}};
  } else if (std::holds_alternative<nh::GlyphEnd>(event)) {
    e = {{"kind", "glyphEnd"}};
  } else if (const auto* w = std::get_if<nh::SelectWindow>(&event)) {
    e = {{"kind", "selectWindow"}, {"winid", w->winid ? json(*w->winid) : json(nullptr)}};
  } else if (std::holds_alternative<nh::FrameSync>(event)) {
    e = {{"kind", "frameSync"}};
  } else {
    const auto& s = std::get<nh::Sound>(event);
    e = {{"kind", "sound"}, {"id", s.id ? json(*s.id) : json(nullptr)}};
  }
  return {{"type", "event"}, {"event", e}};
}

// --- Demo session -------------------------------------------------------------

// One cell drawn the way NetHack's tty port draws a map glyph.
std::string glyph(std::uint32_t tile, char ch, std::uint32_t flags = 0) {
  return "\x1b[1;0;" + std::to_string(tile) + ";" + std::to_string(flags) + "z" + ch +
         "\x1b[1;1z";
}

std::string move_to(int row, int col) {
  return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

// Cuts a line to the terminal width, so nothing wraps and scrolls the map.
std::string fit(std::string line, int cols) {
  if (static_cast<int>(line.size()) > cols) line.resize(static_cast<std::size_t>(cols));
  return line;
}

// A lit room drawn with NetHack 3.6.x tile numbers (the vanilla 3.6.7 sheet):
// the cmap tiles start at 850, the Valkyrie is 348 and the little dog is 16.
// Like NetHack, it lays itself out for the terminal size it was told about.
std::string demo_screen(int cols, int rows, const std::string& message) {
  constexpr int kWidth = 11, kHeight = 6;
  const int top = 4, left = std::max(1, (cols - kWidth) / 2);
  const int hero_row = top + 2, hero_col = left + 5;

  std::string s = "[H[2J";
  s += move_to(1, 1) + fit(message, cols);
  s += "[1;2;3z";  // select the map window

  for (int r = 0; r < kHeight; ++r) {
    s += move_to(top + r, left);
    for (int c = 0; c < kWidth; ++c) {
      const bool t = r == 0, b = r == kHeight - 1;
      const bool l = c == 0, rt = c == kWidth - 1;
      if (t && l) s += glyph(853, '-');
      else if (t && rt) s += glyph(854, '-');
      else if (b && l) s += glyph(855, '-');
      else if (b && rt) s += glyph(856, '-');
      else if (b && c == 5) s += glyph(864, '|');  // open door
      else if (t || b) s += glyph(852, '-');
      else if (l || rt) s += glyph(851, '|');
      else if (r == 1 && c == 2) s += glyph(881, '{');  // fountain
      else if (r == 2 && c == 5) s += glyph(348, '@');
      else if (r == 2 && c == 6) s += "[1;37m" + glyph(16, 'd', 0x08) + "[0m";  // pet
      else if (r == 4 && c == 8) s += glyph(874, '>');
      else s += glyph(869, '.');
    }
  }
  for (int r = 0; r < 3; ++r) s += move_to(top + kHeight + r, left + 5) + glyph(871, '#');

  s += "[1;2;1z";  // the status window
  s += move_to(rows - 1, 1) +
       fit("Valkyrie the Stripling   St:18/02 Dx:13 Co:18 In:7 Wi:10 Ch:10 Lawful", cols);
  s += move_to(rows, 1) + fit("Dlvl:1 $:0 HP:16(16) Pw:1(1) AC:6 Xp:1/0 T:1", cols);
  s += move_to(hero_row, hero_col) + "[1;3z";  // cursor on the hero; frame done
  return s;
}

class DemoSession {
 public:
  explicit DemoSession(Bridge& bridge) : bridge_(bridge) {}
  ~DemoSession() { stop(); }

  void connect(const std::string& where, nh::NetHackVersion version, int cols, int rows,
               std::uint32_t tile_count) {
    stop();
    {
      std::lock_guard lock(mutex_);
      version_ = version;
      demuxer_ = nh::Demuxer{};
      // Diagnostics, off unless the environment asks for them. See
      // core/debuglog.hpp.
      debug_file_ = nh::file_from_env("NHTILES_LOG");
      debug_ = debug_file_ ? std::make_unique<nh::TileDebugLog>(*debug_file_, tile_count) : nullptr;
      raw_ = nh::file_from_env("NHTILES_RAW");
      cols_ = cols;
      rows_ = rows;
      message_ = "Hello Valkyrie, welcome to the C++ backend demo! Type any key.";
    }
    running_ = true;
    bridge_.emit("nh://status", {{"state", "connecting"}, {"message", where}});
    worker_ = std::thread([this, where] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      if (!running_) return;
      bridge_.emit("nh://status", {{"state", "connected"}, {"message", where + " (demo)"}});
      redraw();
      bridge_.emit("nh://tiledata-seen", nullptr);
    });
  }

  void write(std::string_view data) {
    if (!running_) throw std::runtime_error("not connected");
    std::string shown;
    for (const char c : data) {
      const auto b = static_cast<unsigned char>(c);
      shown += b >= 0x20 && b < 0x7f ? std::string(1, c) : "^" + std::to_string(b);
    }
    {
      std::lock_guard lock(mutex_);
      message_ = "You pressed: " + shown;
    }
    redraw();
  }

  // NetHack answers a SIGWINCH by redrawing for the new size.
  void resize(int cols, int rows) {
    {
      std::lock_guard lock(mutex_);
      cols_ = cols;
      rows_ = rows;
    }
    if (running_) redraw();
  }

  void disconnect() {
    if (!running_) return;
    stop();
    bridge_.emit("nh://status", {{"state", "closed"}, {"message", nullptr}});
  }

 private:
  void stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    if (debug_) debug_->summarize();
    debug_.reset();
    debug_file_.reset();
    raw_.reset();
  }

  void redraw() {
    std::lock_guard lock(mutex_);
    const std::string bytes = demo_screen(cols_, rows_, message_);
    if (raw_) raw_->write(bytes.data(), static_cast<std::streamsize>(bytes.size())).flush();
    const auto decoded = demuxer_.feed(bytes);
    if (debug_) debug_->observe(decoded);
    json items = json::array();
    for (const auto& item : decoded) items.push_back(to_json(item, version_));
    bridge_.emit("nh://stream", items);
  }

  Bridge& bridge_;
  std::mutex mutex_;
  nh::Demuxer demuxer_;
  std::unique_ptr<std::ofstream> debug_file_;
  std::unique_ptr<nh::TileDebugLog> debug_;
  std::unique_ptr<std::ofstream> raw_;
  std::thread worker_;
  std::atomic<bool> running_ = false;
  nh::NetHackVersion version_ = nh::NetHackVersion::V36;
  int cols_ = 80;
  int rows_ = 24;
  std::string message_;
};

// --- Native integrations ----------------------------------------------------

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

// The system folder picker. Runs on the UI thread, where webview has already
// set up COM.
std::optional<std::string> pick_folder(HWND owner, const std::string& title) {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    throw std::runtime_error("the folder picker is unavailable");
  }
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(widen(title).c_str());

  std::optional<std::string> chosen;
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        chosen = narrow(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return chosen;
}
#endif

// --- The page -----------------------------------------------------------------

// The origin the built frontend is served from. `.example` is reserved and
// never resolves on a network, and Microsoft's WebView2 samples use it for
// this job.
constexpr const char* kAppHost = "nethack-tiles.example";

// Serves `folder` at https://<kAppHost>/ inside the webview, as Tauri serves
// its `frontendDist`. The files never go through a socket, so no other
// program on the machine can reach them.
//
// The mapping has no default document: `/` names the folder itself and fails
// with ERR_ACCESS_DENIED. Load `/index.html` by name.
void serve_folder(webview::webview& view, const fs::path& folder) {
#ifdef _WIN32
  const auto controller = view.browser_controller();
  if (!controller.ok()) throw std::runtime_error("the webview has no controller yet");
  ICoreWebView2* core = nullptr;
  if (FAILED(static_cast<ICoreWebView2Controller*>(controller.value())->get_CoreWebView2(&core))) {
    throw std::runtime_error("the webview is not ready");
  }
  // The folder mapping came with ICoreWebView2_3, in runtime 1.0.864.
  ICoreWebView2_3* core3 = nullptr;
  const HRESULT found = core->QueryInterface(IID_PPV_ARGS(&core3));
  core->Release();
  if (FAILED(found)) {
    throw std::runtime_error("this WebView2 runtime is too old to serve the app; update it");
  }
  const HRESULT mapped = core3->SetVirtualHostNameToFolderMapping(
      widen(kAppHost).c_str(), folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
  core3->Release();
  if (FAILED(mapped)) throw std::runtime_error("the webview refused to serve " + folder.string());
#else
  // WKWebView needs a WKURLSchemeHandler and WebKitGTK a URI scheme. Neither
  // is written yet.
  (void)view;
  (void)folder;
  throw std::runtime_error(
      "serving the built frontend is not ported to this platform yet; set NH_FRONTEND_URL");
#endif
}

// Where the page comes from. NH_FRONTEND_URL wins. Without it, a Debug build
// loads the Vite dev server, so frontend edits hot-reload, and a Release
// build serves the frontend that was built next to the executable.
std::string frontend_url(webview::webview& view, const fs::path& dist) {
  if (const char* url = std::getenv("NH_FRONTEND_URL"); url && *url) return url;
#ifndef NDEBUG
  (void)view;
  (void)dist;
  return "http://localhost:1420";
#else
  if (!fs::exists(dist / "index.html")) {
    throw std::runtime_error("the built frontend is missing from " + dist.string() +
                             ". Build the frontend target, or set NH_FRONTEND_URL.");
  }
  serve_folder(view, dist);
  return std::string("https://") + kAppHost + "/index.html";
#endif
}

// Shown in place of the frontend when it cannot load, so the window does
// not stay blank.
std::string error_page(const std::string& message) {
  std::string escaped;
  for (const char c : message) {
    switch (c) {
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '&': escaped += "&amp;"; break;
      default: escaped += c;
    }
  }
  return "<body style=\"background:#0d1416;color:#cfc9b8;font:16px monospace;padding:2em\">"
         "<h2>The app cannot start</h2><p>" +
         escaped + "</p></body>";
}

void open_url(const std::string& url) {
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
    throw std::runtime_error("only http and https links open");
  }
  if (url.find_first_of("'\"\\ ") != std::string::npos) {
    throw std::runtime_error("refusing a link with quotes or spaces");
  }
#ifdef _WIN32
  ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::system(("open '" + url + "'").c_str());
#else
  std::system(("xdg-open '" + url + "'").c_str());
#endif
}

json not_ported(const std::string& what) {
  throw std::runtime_error(what + " is not ported to the C++ backend yet");
}

}  // namespace

int main() {
  const fs::path assets = exe_dir() / "assets";
  Tilesets sheets = load_tilesets(assets / "tiles");
  std::cerr << "[app] " << sheets.size() << " tile sheets from " << assets.string() << "\n";

  json profiles = json::array({
      make_profile("nethack-alt-org", "NetHack.alt.org", "nethack.alt.org", "v50", sheets),
      make_profile("hardfought-org", "Hardfought (US)", "us.hardfought.org", "v36", sheets),
  });

  webview::webview view(true, nullptr);
  view.set_title("NetHack Tiles Client (C++)");
  view.set_size(1100, 760, WEBVIEW_HINT_NONE);

  Bridge bridge(view);
  DemoSession session(bridge);

  const auto find_profile = [&](const std::string& id) -> json& {
    for (auto& p : profiles) {
      if (p["id"] == id) return p;
    }
    throw std::runtime_error("no profile with id " + id);
  };

  bridge.command("list_profiles", [&](const json&) { return profiles; });
  bridge.command("last_used_profile", [](const json&) { return json(nullptr); });
  bridge.command("has_saved_password", [](const json&) { return json(false); });
  bridge.command("save_profile", [&](const json& args) {
    // Kept in memory only until profile storage is ported.
    const json& profile = args.at("profile");
    try {
      find_profile(profile.at("id")) = profile;
    } catch (const std::runtime_error&) {
      profiles.push_back(profile);
    }
    return json(nullptr);
  });
  bridge.command("delete_profile", [&](const json& args) {
    const std::string id = args.at("id");
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(),
                                  [&](const json& p) { return p["id"] == id; }),
                   profiles.end());
    return json(nullptr);
  });

  bridge.command("list_tilesets", [&](const json&) {
    std::vector<const nh::TilesetManifest*> manifests;
    for (const auto& [id, sheet] : sheets) manifests.push_back(&sheet.manifest());
    std::sort(manifests.begin(), manifests.end(),
              [](const auto* a, const auto* b) { return a->name < b->name; });
    json list = json::array();
    for (const auto* manifest : manifests) list.push_back(*manifest);
    return list;
  });
  bridge.command("get_tileset", [&](const json& args) {
    const std::string id = args.at("id");
    const auto sheet = sheets.find(id);
    if (sheet == sheets.end()) throw std::runtime_error("no tileset with id \"" + id + "\"");
    return tileset_payload(sheet->second);
  });
  // Loads a sheet the player chose. The manifest gives its geometry. A wrong
  // one shears the tiles, so the loader checks the dimensions.
  bridge.command("add_custom_tileset", [&](const json& args) {
    auto manifest = args.at("manifest").get<nh::TilesetManifest>();
    const std::string path = args.at("path");
    std::string png;
    try {
      png = read_file(nh::statelog::path_from_utf8(path));
    } catch (const std::exception&) {
      throw std::runtime_error("reading " + path + ": cannot read the file");
    }
    const std::string id = manifest.id;
    const auto& sheet =
        sheets.insert_or_assign(id, nh::Tileset::load(std::move(manifest), std::move(png)))
            .first->second;
    return tileset_payload(sheet);
  });

  bridge.command("session_connect", [&](const json& args) {
    const json& request = args.at("request");
    const json& profile = find_profile(request.at("profileId"));
    const auto version =
        profile["version"] == "v50" ? nh::NetHackVersion::V50 : nh::NetHackVersion::V36;
    const auto sheet = sheets.find(profile.value("tilesetId", std::string()));
    const std::uint32_t tile_count =
        sheet == sheets.end() ? 0 : sheet->second.manifest().tile_count;
    session.connect(profile["host"].get<std::string>(), version,
                    request.at("cols").get<int>(), request.at("rows").get<int>(), tile_count);

    // A new game starts a new state log. Files from the last one would
    // describe a dungeon the player has left.
    const std::string directory = profile.value("stateLogDirectory", std::string());
    if (profile.value("stateLogEnabled", false) && !directory.empty()) {
      try {
        nh::statelog::clear(nh::statelog::path_from_utf8(directory));
      } catch (const std::exception& e) {
        std::cerr << "[app] could not clear the state log at " << directory << ": " << e.what()
                  << "\n";
      }
    }
    return json(nullptr);
  });
  bridge.command("session_write", [&](const json& args) {
    session.write(args.at("data").get<std::string>());
    return json(nullptr);
  });
  bridge.command("session_write_bytes", [&](const json& args) {
    std::string bytes;
    for (const auto& b : args.at("bytes")) bytes.push_back(static_cast<char>(b.get<int>()));
    session.write(bytes);
    return json(nullptr);
  });
  bridge.command("session_resize", [&](const json& args) {
    std::cerr << "[session] resize " << args.at("cols") << "x" << args.at("rows") << "\n";
    session.resize(args.at("cols").get<int>(), args.at("rows").get<int>());
    return json(nullptr);
  });
  bridge.command("session_disconnect", [&](const json&) {
    session.disconnect();
    return json(nullptr);
  });

  // The page decides the text. The names are checked against the fixed
  // list in core/statelog.hpp, so a bad payload cannot write anywhere else.
  bridge.command("state_log_write", [](const json& args) {
    const json& request = args.at("request");
    nh::statelog::write_files(
        nh::statelog::path_from_utf8(request.at("directory").get<std::string>()),
        request.at("files").get<std::map<std::string, std::string>>());
    return json(nullptr);
  });
  bridge.command("pick_folder", [&](const json& args) {
#ifdef _WIN32
    const auto window = view.window();
    const auto chosen =
        pick_folder(window.ok() ? static_cast<HWND>(window.value()) : nullptr,
                    args.value("title", std::string("Choose a folder")));
    return chosen ? json(*chosen) : json(nullptr);
#else
    (void)args;
    return not_ported("The folder picker on this platform");
#endif
  });
  bridge.command("open_url", [](const json& args) {
    open_url(args.at("url").get<std::string>());
    return json(nullptr);
  });
  bridge.command("log_frontend_error", [](const json& args) {
    std::cerr << "[frontend] " << args.value("message", std::string()) << "\n";
    return json(nullptr);
  });

  try {
    const std::string url = frontend_url(view, exe_dir() / "frontend");
    std::cerr << "[app] loading " << url << "\n";
    view.navigate(url);
  } catch (const std::exception& e) {
    std::cerr << "[app] " << e.what() << "\n";
    view.set_html(error_page(e.what()));
  }
  view.run();
  return 0;
}
