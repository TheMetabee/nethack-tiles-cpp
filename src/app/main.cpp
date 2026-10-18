// Hosts the React + xterm.js frontend in a system webview and answers its
// commands. Replaces the Tauri app shell (src-tauri/src/app.rs).
//
// Ported: SSH sessions with the auto-login, saved profiles with passwords in
// the keychain (Windows), tilesets, custom tilesets, the state log and the
// debug logs. Not yet: local play, and the keychain on macOS and Linux.
// NH_DEMO=1 replaces SSH with a demo that draws one room.

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
#include "core/game_stream.hpp"
#include "core/session.hpp"
#include "core/glyph.hpp"
#include "core/keychain.hpp"
#include "core/profiles.hpp"
#include "core/statelog.hpp"
#include "core/tileset.hpp"
#include "net/ssh.hpp"

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

// --- Profiles -------------------------------------------------------------------

// The bundled sheet built for `version`. Tile numbers are positional and
// renumbered between NetHack lines, so this pairing is not cosmetic.
std::string sheet_for_version(const Tilesets& sheets, nh::NetHackVersion version) {
  for (const auto& [id, sheet] : sheets) {
    if (sheet.manifest().version == version) return id;
  }
  return sheets.empty() ? std::string() : sheets.begin()->first;
}

// Opens the saved profiles: the Rust app's own file and keychain entries, so
// either app sees what the other saved.
nh::ProfileStore open_profiles(const Tilesets& sheets) {
  const fs::path path = nh::ProfileStore::default_path();
  // The bundle identifier changed, which moved both the config directory and
  // the keychain service. Neither is worth making anyone set up again.
  try {
    nh::ProfileStore::adopt_legacy(path, nh::ProfileStore::legacy_path());
  } catch (const std::exception& e) {
    std::cerr << "[profiles] could not adopt the profiles from the previous version: " << e.what()
              << "\n";
  }

  std::unique_ptr<nh::SecretStore> secrets;
  if (nh::KeychainSecrets::available()) {
    secrets = std::make_unique<nh::WithLegacy>(
        std::make_unique<nh::KeychainSecrets>(nh::kKeychainService),
        std::make_unique<nh::KeychainSecrets>(nh::kLegacyKeychainService));
  } else {
    std::cerr << "[profiles] this platform's keychain is not ported; passwords last until the "
                 "app closes\n";
    secrets = std::make_unique<nh::MemorySecrets>();
  }

  auto store = nh::ProfileStore::load(path, std::move(secrets));
  // Nobody's first run should start on an empty screen: offer the two public
  // servers, each with the sheet for its release.
  if (store.is_first_run()) {
    for (auto profile : nh::default_profiles(std::nullopt)) {
      profile.tileset_id = sheet_for_version(sheets, profile.version);
      try {
        store.upsert(std::move(profile));
      } catch (const std::exception& e) {
        std::cerr << "[profiles] could not write the default profiles: " << e.what() << "\n";
      }
    }
  }
  std::cerr << "[profiles] " << store.profiles().size() << " profiles from "
            << store.path().string() << "\n";
  return store;
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

// Draws one room and answers keys, like a game with no network behind it.
// NH_DEMO=1 selects it in place of SSH, for work on the page.
class DemoSession final : public nh::Session {
 public:
  DemoSession(int cols, int rows, nh::SessionEvents events)
      : cols_(cols), rows_(rows), events_(std::move(events)) {}

  void start() override { draw(); }

  void write(std::string bytes) override {
    if (closed_) throw nh::Disconnected();
    std::string shown;
    for (const char c : bytes) {
      const auto b = static_cast<unsigned char>(c);
      shown += b >= 0x20 && b < 0x7f ? std::string(1, c) : "^" + std::to_string(b);
    }
    {
      std::lock_guard lock(mutex_);
      message_ = "You pressed: " + shown;
    }
    draw();
  }

  // NetHack answers a SIGWINCH by drawing again for the new size.
  void resize(std::uint32_t cols, std::uint32_t rows) override {
    if (closed_) throw nh::Disconnected();
    {
      std::lock_guard lock(mutex_);
      cols_ = static_cast<int>(cols);
      rows_ = static_cast<int>(rows);
    }
    draw();
  }

  void disconnect() override {
    if (closed_.exchange(true)) return;
    events_(nh::SessionClosed{});
  }

  bool is_connected() const override { return !closed_; }

 private:
  void draw() {
    std::string screen;
    {
      std::lock_guard lock(mutex_);
      screen = demo_screen(cols_, rows_, message_);
    }
    events_(nh::SessionData{std::move(screen)});
  }

  std::mutex mutex_;
  int cols_;
  int rows_;
  std::string message_ = "Hello Valkyrie, welcome to the C++ backend demo! Type any key.";
  nh::SessionEvents events_;
  std::atomic<bool> closed_ = false;
};

// What a connect needs from the profile.
struct ConnectPlan {
  std::string host;
  std::uint16_t port = 22;
  std::string ssh_user;
  nh::NetHackVersion version = nh::NetHackVersion::V36;
  std::optional<nh::AutoLogin> login;
  std::uint32_t tile_count = 0;
  std::uint32_t cols = 80;
  std::uint32_t rows = 24;
  bool demo = false;
};

// Owns the one live session and turns its events into page events. Port of
// the session half of src-tauri/src/app.rs (`session_connect` and
// `consume_session`).
//
// A transport sends events on its own thread. No session call happens under
// `mutex_`, because a transport can send an event from inside a call, and
// the event handler takes `mutex_`.
class SessionRunner {
 public:
  explicit SessionRunner(Bridge& bridge) : bridge_(bridge) {}

  ~SessionRunner() {
    std::shared_ptr<nh::Session> session;
    {
      std::lock_guard lock(mutex_);
      ++generation_;
      session = std::move(session_);
    }
    if (session) session->disconnect();
    if (connecting_.joinable()) connecting_.join();
  }

  // Connects on a worker thread, then settles `reply`: the page shows the
  // game only once the shell is open, and shows the error otherwise.
  void connect(ConnectPlan plan, Bridge::Reply reply) {
    drop_retired();
    std::uint64_t generation = 0;
    {
      std::lock_guard lock(mutex_);
      if (session_ || busy_) throw std::runtime_error("already connected -- disconnect first");
      busy_ = true;
      generation = ++generation_;
      version_ = plan.version;
      stream_.emplace(plan.version, std::move(plan.login));
      // Diagnostics, off unless the environment asks for them. See
      // core/debuglog.hpp.
      debug_file_ = nh::file_from_env("NHTILES_LOG");
      debug_ = debug_file_ ? std::make_unique<nh::TileDebugLog>(*debug_file_, plan.tile_count)
                           : nullptr;
      raw_ = nh::file_from_env("NHTILES_RAW");
    }
    if (connecting_.joinable()) connecting_.join();

    status("connecting", plan.demo ? plan.host + " (demo)"
                                   : plan.host + ":" + std::to_string(plan.port));
    connecting_ = std::thread([this, plan = std::move(plan), reply, generation] {
      const auto events = [this, generation](nh::SessionEvent event) {
        on_event(generation, std::move(event));
      };
      std::shared_ptr<nh::Session> session;
      try {
        if (plan.demo) {
          session = std::make_shared<DemoSession>(static_cast<int>(plan.cols),
                                                  static_cast<int>(plan.rows), events);
        } else {
          auto config = nh::ssh::Config::dgamelaunch(plan.host, plan.port, plan.ssh_user);
          config.cols = plan.cols;
          config.rows = plan.rows;
          session = nh::ssh::connect(config, events);
        }
      } catch (const std::exception& e) {
        {
          std::lock_guard lock(mutex_);
          busy_ = false;
        }
        status("error", std::string(e.what()));
        reply(false, e.what());
        return;
      }
      {
        std::lock_guard lock(mutex_);
        busy_ = false;
        if (generation != generation_) return;  // the app is closing
        session_ = session;
      }
      reply(true, nullptr);
      status("connected", plan.demo ? plan.host + " (demo)" : plan.host);
      // Only now: game output before this point would reach a page that has
      // not drawn the terminal yet.
      session->start();
    });
  }

  void write(std::string bytes) { current()->write(std::move(bytes)); }

  void resize(std::uint32_t cols, std::uint32_t rows) { current()->resize(cols, rows); }

  void disconnect() {
    drop_retired();
    std::shared_ptr<nh::Session> session;
    {
      std::lock_guard lock(mutex_);
      session = session_;
    }
    if (session) session->disconnect();
  }

 private:
  std::shared_ptr<nh::Session> current() {
    std::lock_guard lock(mutex_);
    if (!session_) throw std::runtime_error("not connected");
    return session_;
  }

  void status(const char* state, const std::optional<std::string>& message) {
    bridge_.emit("nh://status",
                 {{"state", state}, {"message", message ? json(*message) : json(nullptr)}});
  }

  void on_event(std::uint64_t generation, nh::SessionEvent event) {
    std::unique_lock lock(mutex_);
    if (generation != generation_ || !stream_) return;  // a session already replaced

    if (auto* data = std::get_if<nh::SessionData>(&event)) {
      if (raw_) {
        raw_->write(data->bytes.data(), static_cast<std::streamsize>(data->bytes.size()));
        raw_->flush();
      }
      auto out = stream_->feed(data->bytes);
      if (debug_) debug_->observe(out.items);
      const auto session = session_;
      const auto version = version_;
      lock.unlock();

      if (out.server_version) {
        if (out.version_warning) std::cerr << "[app] " << *out.version_warning << "\n";
        bridge_.emit("nh://server-version",
                     {{"text", out.server_version->text},
                      {"version", out.server_version->version
                                      ? json(*out.server_version->version)
                                      : json(nullptr)},
                      {"warning", out.version_warning ? json(*out.version_warning)
                                                      : json(nullptr)}});
      }
      if (out.reply && session) {
        try {
          session->write(std::move(*out.reply));
        } catch (const nh::Disconnected&) {
        }
      }
      if (out.login) status(out.login->logged_in ? "info" : "error", out.login->message);
      if (!out.items.empty()) {
        json items = json::array();
        for (const auto& item : out.items) items.push_back(to_json(item, version));
        bridge_.emit("nh://stream", items);
      }
      if (out.tiledata_seen) bridge_.emit("nh://tiledata-seen", nullptr);
      return;
    }

    if (const auto* message = std::get_if<nh::SessionStatus>(&event)) {
      const std::string text = message->message;
      lock.unlock();
      status("info", text);
      return;
    }

    // Closed. The session cannot be destroyed here: this can be its own
    // thread, which its destructor joins. It waits in `retired_` instead.
    if (debug_) debug_->summarize();
    debug_.reset();
    debug_file_.reset();
    raw_.reset();
    stream_.reset();
    if (session_) retired_.push_back(std::move(session_));
    const auto reason = std::get<nh::SessionClosed>(event).reason;
    lock.unlock();
    status("closed", reason);
  }

  // Destroys sessions that have ended. Only the UI thread calls it.
  void drop_retired() {
    std::vector<std::shared_ptr<nh::Session>> retired;
    {
      std::lock_guard lock(mutex_);
      retired.swap(retired_);
    }
  }

  Bridge& bridge_;
  std::mutex mutex_;
  std::shared_ptr<nh::Session> session_;
  std::vector<std::shared_ptr<nh::Session>> retired_;
  std::optional<nh::GameStream> stream_;
  nh::NetHackVersion version_ = nh::NetHackVersion::V36;
  std::unique_ptr<std::ofstream> debug_file_;
  std::unique_ptr<nh::TileDebugLog> debug_;
  std::unique_ptr<std::ofstream> raw_;
  std::thread connecting_;
  std::uint64_t generation_ = 0;
  bool busy_ = false;
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

  std::optional<nh::ProfileStore> profiles;
  std::string startup_error;
  try {
    profiles.emplace(open_profiles(sheets));
  } catch (const std::exception& e) {
    // A config file that does not parse must not be overwritten with
    // defaults, so the app stops and says why.
    startup_error = e.what();
    std::cerr << "[app] " << startup_error << "\n";
  }

  webview::webview view(true, nullptr);
  view.set_title("NetHack Tiles Client (C++)");
  view.set_size(1100, 760, WEBVIEW_HINT_NONE);

  Bridge bridge(view);
  SessionRunner session(bridge);
  bridge.command("list_profiles", [&](const json&) { return json(profiles->profiles()); });
  bridge.command("last_used_profile", [&](const json&) {
    const auto& last = profiles->last_used();
    return last ? json(*last) : json(nullptr);
  });
  // Whether a password is on file, so the page can say "saved" without ever
  // reading the secret back.
  bridge.command("has_saved_password", [&](const json& args) {
    return json(profiles->password(args.at("id").get<std::string>()).has_value());
  });
  bridge.command("save_profile", [&](const json& args) {
    auto profile = args.at("profile").get<nh::Profile>();
    const std::string id = profile.id;
    profiles->upsert(std::move(profile));
    // A null password leaves the stored one as it is.
    if (const auto password = args.find("password");
        password != args.end() && password->is_string()) {
      profiles->set_password(id, password->get<std::string>());
    }
    return json(nullptr);
  });
  bridge.command("delete_profile", [&](const json& args) {
    profiles->remove(args.at("id").get<std::string>());
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

  // Resolves once the shell is open, or rejects with the reason: the page
  // shows the game only after a connect that worked.
  bridge.command_async("session_connect", [&](const json& args, Bridge::Reply reply) {
    const json& request = args.at("request");
    const std::string id = request.at("profileId");
    const nh::Profile* found = profiles->get(id);
    if (!found) throw std::runtime_error("no profile with id \"" + id + "\"");
    const nh::Profile profile = *found;
    if (profile.transport == nh::Transport::Local) not_ported("Local play");
    const auto sheet = sheets.find(profile.tileset_id);

    ConnectPlan plan;
    plan.host = profile.host;
    plan.port = profile.port;
    plan.ssh_user = profile.ssh_user;
    plan.version = profile.version;
    // Read whenever a prompt could want it. Whether it is typed is
    // autologin_for's decision.
    plan.login = nh::autologin_for(false, profile.game_user, profiles->password(id));
    plan.tile_count = sheet == sheets.end() ? 0 : sheet->second.manifest().tile_count;
    plan.cols = request.at("cols").get<std::uint32_t>();
    plan.rows = request.at("rows").get<std::uint32_t>();
    if (const char* demo = std::getenv("NH_DEMO"); demo && *demo) plan.demo = true;

    session.connect(std::move(plan), [&view, &profiles, profile, reply](bool ok,
                                                                        const json& value) {
      if (ok) {
        // On the UI thread, which owns the store.
        view.dispatch([&profiles, profile] {
          try {
            profiles->set_last_used(profile.id);
          } catch (const std::exception& e) {
            std::cerr << "[profiles] could not record the last profile: " << e.what() << "\n";
          }
          // A new game starts a new state log. Files from the last one
          // would describe a dungeon the player has left.
          if (profile.state_log_enabled && !profile.state_log_directory.empty()) {
            try {
              nh::statelog::clear(nh::statelog::path_from_utf8(profile.state_log_directory));
            } catch (const std::exception& e) {
              std::cerr << "[app] could not clear the state log at "
                        << profile.state_log_directory << ": " << e.what() << "\n";
            }
          }
        });
      }
      reply(ok, value);
    });
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
    session.resize(args.at("cols").get<std::uint32_t>(), args.at("rows").get<std::uint32_t>());
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

  if (!startup_error.empty()) {
    view.set_html(error_page(startup_error));
    view.run();
    return 1;
  }
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
