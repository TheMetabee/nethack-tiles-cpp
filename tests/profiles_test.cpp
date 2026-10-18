// Port of the tests in src-tauri/src/profiles.rs. Same cases, same names,
// plus the formats the C++ and Rust apps share.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/profiles.hpp"

namespace fs = std::filesystem;
using namespace nh;
using Kind = ProfileError::Kind;

namespace {

class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    std::ostringstream name;
    name << "nethack-tiles-test-" << tag << "-" << std::this_thread::get_id();
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

struct Fixture {
  explicit Fixture(const std::string& tag)
      // Nested on purpose: the store must make missing parents.
      : dir(tag), path(dir.path / "config" / "profiles.toml") {}
  TempDir dir;
  fs::path path;
};

ProfileStore store(const fs::path& path) {
  return ProfileStore::load(path, std::make_unique<MemorySecrets>());
}

Profile sample() {
  auto p = Profile::make("nao", "NetHack.alt.org");
  p.host = "nethack.alt.org";
  p.ssh_user = "nethack";
  p.game_user = "username";
  p.tileset_id = "vanilla-3.6.7-16";
  return p;
}

std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// Writes a profile file with one profile on `host`.
void with_host(const fs::path& path, const std::string& host) {
  write(path, "[[profiles]]\nid = \"hf\"\nname = \"Hardfought\"\nhost = \"" + host +
                  "\"\nsshUser = \"nethack\"\ntilesetId = \"vanilla-3.6.7-16\"\n");
}

Kind error_kind(const std::function<void()>& f) {
  try {
    f();
  } catch (const ProfileError& e) {
    return e.kind();
  }
  FAIL("no ProfileError");
  return {};
}

}  // namespace

TEST_CASE("a missing file loads as an empty store") {
  Fixture f("missing");
  const auto s = store(f.path);
  CHECK(s.profiles().empty());
  CHECK_FALSE(s.last_used());
}

TEST_CASE("a first run is told apart from a deliberately emptied store") {
  Fixture f("firstrun");
  CHECK(store(f.path).is_first_run());

  auto s = store(f.path);
  s.upsert(sample());
  s.remove("nao");

  const auto reloaded = store(f.path);
  CHECK(reloaded.profiles().empty());
  // A file exists, so the empty list is a choice.
  CHECK_FALSE(reloaded.is_first_run());
}

TEST_CASE("the default profiles are the two public servers") {
  const auto profiles = default_profiles(std::nullopt);
  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].host == "nethack.alt.org");
  CHECK(profiles[1].host == "us.hardfought.org");
}

TEST_CASE("the default profiles are ready to connect to") {
  for (const auto& p : default_profiles(std::nullopt)) {
    CHECK_FALSE(p.id.empty());
    CHECK_FALSE(p.name.empty());
    CHECK(p.ssh_user == "nethack");  // the shared dgamelaunch user
    CHECK(p.port == 22);
    // No credentials are invented; the player gives those.
    CHECK(p.game_user.empty());
  }
}

TEST_CASE("the hardfought default is a host that answers ssh") {
  // Bare hardfought.org is the website, behind Cloudflare, which does not
  // pass port 22: it resolves but can never take an SSH connection.
  for (const auto& p : default_profiles(std::nullopt)) {
    if (p.host.find("hardfought") == std::string::npos) continue;
    CHECK(p.host == "us.hardfought.org");
  }
}

TEST_CASE("a profile left pointing at the hardfought website is repaired") {
  // Anyone who ran an earlier build has this host saved, and it can never
  // connect. To tell them to edit it themselves is not a fix.
  Fixture f("hardfought-repair");
  with_host(f.path, "hardfought.org");
  CHECK(store(f.path).get("hf")->host == "us.hardfought.org");
}

TEST_CASE("the repair is written back rather than redone every launch") {
  Fixture f("hardfought-repair-persists");
  with_host(f.path, "hardfought.org");
  (void)store(f.path);
  CHECK(read(f.path).find("us.hardfought.org") != std::string::npos);
}

TEST_CASE("a regional hardfought host is left alone") {
  Fixture f("hardfought-eu");
  with_host(f.path, "eu.hardfought.org");
  CHECK(store(f.path).get("hf")->host == "eu.hardfought.org");
}

TEST_CASE("an unrelated host is never rewritten") {
  Fixture f("host-untouched");
  with_host(f.path, "nethack.alt.org");
  CHECK(store(f.path).get("hf")->host == "nethack.alt.org");
}

TEST_CASE("a first run writes no file just by being loaded") {
  // The repair must not make a config file where there was none, or the
  // next launch would no longer look like a first run.
  Fixture f("repair-no-file");
  (void)store(f.path);
  CHECK_FALSE(fs::exists(f.path));
}

TEST_CASE("a machine with nethack installed also gets a local profile") {
  const auto profiles =
      default_profiles(std::pair{fs::path("/opt/homebrew/bin/nethack"), NetHackVersion::V36});
  const Profile* local = nullptr;
  for (const auto& p : profiles) {
    if (p.transport == Transport::Local) local = &p;
  }
  REQUIRE(local);
  CHECK(local->command == "/opt/homebrew/bin/nethack");
  CHECK(local->version == NetHackVersion::V36);
}

TEST_CASE("a local profile needs no host ssh user or login") {
  const auto profiles =
      default_profiles(std::pair{fs::path("/usr/games/nethack"), NetHackVersion::V50});
  const auto& p = profiles.back();
  REQUIRE(p.transport == Transport::Local);
  CHECK(p.host.empty());  // a local game is not on a host
  CHECK(p.ssh_user.empty());
  CHECK(p.game_user.empty());
}

TEST_CASE("the public servers come before the local game") {
  // The point of the app is the public leaderboards. A local game is the
  // fallback, so it must not be what Connect does by default.
  const auto profiles =
      default_profiles(std::pair{fs::path("/usr/games/nethack"), NetHackVersion::V36});
  CHECK(profiles.front().transport == Transport::Ssh);
  CHECK(profiles.back().transport == Transport::Local);
}

TEST_CASE("a profile saved before local play existed still loads as an ssh one") {
  Fixture f("transport-default");
  write(f.path,
        "[[profiles]]\nid = \"old\"\nname = \"Old\"\nhost = \"nethack.alt.org\"\n"
        "sshUser = \"nethack\"\ntilesetId = \"vanilla-3.6.7-16\"\n");
  const auto s = store(f.path);
  const auto* p = s.get("old");
  REQUIRE(p);
  CHECK(p->transport == Transport::Ssh);
  CHECK(p->command.empty());
}

TEST_CASE("a local profile survives a reload") {
  Fixture f("local-roundtrip");
  auto s = store(f.path);
  auto local = Profile::make("local", "This Mac");
  local.transport = Transport::Local;
  local.command = "/opt/homebrew/bin/nethack";
  s.upsert(local);
  REQUIRE(store(f.path).get("local"));
  CHECK(*store(f.path).get("local") == local);
}

TEST_CASE("an upserted profile survives a reload") {
  Fixture f("roundtrip");
  auto s = store(f.path);
  s.upsert(sample());
  CHECK(store(f.path).profiles() == std::vector{sample()});
}

TEST_CASE("upsert replaces a profile with the same id instead of duplicating") {
  Fixture f("replace");
  auto s = store(f.path);
  s.upsert(sample());
  auto renamed = sample();
  renamed.name = "Renamed";
  s.upsert(renamed);
  REQUIRE(s.profiles().size() == 1);
  CHECK(s.profiles()[0].name == "Renamed");
}

TEST_CASE("upsert preserves the order of existing profiles") {
  Fixture f("order");
  auto s = store(f.path);
  s.upsert(Profile::make("a", "A"));
  s.upsert(Profile::make("b", "B"));
  s.upsert(Profile::make("a", "A2"));
  REQUIRE(s.profiles().size() == 2);
  CHECK(s.profiles()[0].id == "a");
  CHECK(s.profiles()[1].id == "b");
}

TEST_CASE("the config file never contains the password") {
  Fixture f("nopassword");
  auto s = store(f.path);
  s.upsert(sample());
  s.set_password("nao", "hunter2");
  CHECK(read(f.path).find("hunter2") == std::string::npos);
  // It is still in the secret store.
  CHECK(s.password("nao") == std::optional<std::string>("hunter2"));
}

TEST_CASE("removing a profile also deletes its password") {
  Fixture f("remove");
  auto s = store(f.path);
  s.upsert(sample());
  s.set_password("nao", "hunter2");
  s.remove("nao");
  CHECK(s.profiles().empty());
  CHECK_FALSE(s.password("nao"));
  CHECK(store(f.path).profiles().empty());
}

TEST_CASE("removing an unknown profile is an error") {
  Fixture f("remove-unknown");
  auto s = store(f.path);
  CHECK(error_kind([&] { s.remove("ghost"); }) == Kind::NoSuchProfile);
}

TEST_CASE("removing a profile clears last used if it pointed there") {
  Fixture f("remove-last-used");
  auto s = store(f.path);
  s.upsert(sample());
  s.set_last_used("nao");
  s.remove("nao");
  CHECK_FALSE(s.last_used());
}

TEST_CASE("last used survives a reload") {
  Fixture f("last-used");
  auto s = store(f.path);
  s.upsert(sample());
  s.set_last_used("nao");
  CHECK(store(f.path).last_used() == std::optional<std::string>("nao"));
}

TEST_CASE("last used must name an existing profile") {
  Fixture f("last-used-unknown");
  auto s = store(f.path);
  CHECK(error_kind([&] { s.set_last_used("ghost"); }) == Kind::NoSuchProfile);
}

TEST_CASE("get finds a profile by id") {
  Fixture f("get");
  auto s = store(f.path);
  s.upsert(sample());
  REQUIRE(s.get("nao"));
  CHECK(s.get("nao")->host == "nethack.alt.org");
  CHECK_FALSE(s.get("ghost"));
}

TEST_CASE("malformed toml is reported with its path") {
  Fixture f("malformed");
  write(f.path, "this is not = = toml");
  try {
    store(f.path);
    FAIL("malformed TOML loaded");
  } catch (const ProfileError& e) {
    CHECK(e.kind() == Kind::Parse);
    CHECK(std::string(e.what()).find("profiles.toml") != std::string::npos);
  }
}

TEST_CASE("optional fields fall back to defaults when absent") {
  Fixture f("defaults");
  write(f.path,
        "[[profiles]]\nid = \"min\"\nname = \"Minimal\"\nhost = \"example.org\"\n"
        "sshUser = \"nethack\"\ntilesetId = \"vanilla-3.6.7-16\"\n");
  const auto s = store(f.path);
  const auto* p = s.get("min");
  REQUIRE(p);
  CHECK(p->port == 22);
  CHECK(p->font_size == 16);
  CHECK(p->scale == 1.0);
  CHECK(p->line_height == 1.0);
  CHECK(p->letter_spacing == 0.0);
  CHECK_FALSE(p->pixel_perfect);
  CHECK_FALSE(p->state_log_enabled);
  CHECK(p->state_log_directory.empty());
  CHECK(p->version == NetHackVersion::V36);
}

TEST_CASE("display settings survive a reload") {
  // These are tuned live while playing, so they must stick.
  Fixture f("display");
  auto s = store(f.path);
  auto p = sample();
  p.font_size = 22;
  p.line_height = 1.4f;
  p.letter_spacing = 6.5f;
  p.pixel_perfect = true;
  s.upsert(p);
  const auto reloaded = store(f.path);
  const auto* q = reloaded.get("nao");
  REQUIRE(q);
  CHECK(q->font_size == 22);
  CHECK(q->line_height == 1.4f);
  CHECK(q->letter_spacing == 6.5f);
  CHECK(q->pixel_perfect);
}

TEST_CASE("state log settings survive a reload") {
  Fixture f("state-log");
  auto s = store(f.path);
  auto p = sample();
  p.state_log_enabled = true;
  p.state_log_directory = "/tmp/nh-state";
  s.upsert(p);
  const auto reloaded = store(f.path);
  const auto* q = reloaded.get("nao");
  REQUIRE(q);
  CHECK(q->state_log_enabled);
  CHECK(q->state_log_directory == "/tmp/nh-state");
}

TEST_CASE("saving creates missing parent directories") {
  Fixture f("mkdir");
  CHECK_FALSE(fs::exists(f.path.parent_path()));
  auto s = store(f.path);
  s.upsert(sample());
  CHECK(fs::exists(f.path));
}

// The app identifier changed from com.ian to io.statico, which moves the
// config directory and the keychain service. Someone already playing must
// not find their servers gone.

TEST_CASE("profiles saved under the old identifier are adopted") {
  TempDir dir("adopt");
  const auto legacy = dir.path / "com.ian" / "profiles.toml";
  const auto current = dir.path / "io.statico" / "profiles.toml";
  store(legacy).upsert(sample());

  ProfileStore::adopt_legacy(current, legacy);

  REQUIRE(store(current).get("nao"));
  CHECK(store(current).get("nao")->host == "nethack.alt.org");
  CHECK(fs::exists(legacy));  // the old file stays; it is not moved
}

TEST_CASE("adopting never overwrites profiles already saved under the new one") {
  TempDir dir("adopt-noclobber");
  const auto legacy = dir.path / "com.ian" / "profiles.toml";
  const auto current = dir.path / "io.statico" / "profiles.toml";
  store(legacy).upsert(sample());
  auto hdf = Profile::make("hdf", "Hardfought");
  hdf.host = "eu.hardfought.org";
  store(current).upsert(hdf);

  ProfileStore::adopt_legacy(current, legacy);

  const auto s = store(current);
  CHECK(s.get("hdf"));
  CHECK_FALSE(s.get("nao"));
}

TEST_CASE("adopting with nothing to adopt is not an error") {
  TempDir dir("adopt-empty");
  const auto legacy = dir.path / "com.ian" / "profiles.toml";
  const auto current = dir.path / "io.statico" / "profiles.toml";
  ProfileStore::adopt_legacy(current, legacy);
  CHECK_FALSE(fs::exists(current));  // no file made out of nothing
}

TEST_CASE("a password from the old keychain service is still found") {
  auto legacy = std::make_unique<MemorySecrets>();
  legacy->set_password("nao", "hunter2");
  WithLegacy secrets(std::make_unique<MemorySecrets>(), std::move(legacy));
  CHECK(secrets.get_password("nao") == std::optional<std::string>("hunter2"));
}

TEST_CASE("a password found in the old service is copied to the new one") {
  // Otherwise every launch reads through to a service the app no longer
  // uses, and to uninstall the old app loses it.
  auto legacy = std::make_unique<MemorySecrets>();
  legacy->set_password("nao", "hunter2");
  auto primary = std::make_unique<MemorySecrets>();
  auto seen = primary->clone_handle();
  WithLegacy secrets(std::move(primary), std::move(legacy));

  secrets.get_password("nao");

  CHECK(seen->get_password("nao") == std::optional<std::string>("hunter2"));
}

TEST_CASE("the new service wins when both have a password") {
  auto legacy = std::make_unique<MemorySecrets>();
  legacy->set_password("nao", "old");
  auto primary = std::make_unique<MemorySecrets>();
  primary->set_password("nao", "new");
  WithLegacy secrets(std::move(primary), std::move(legacy));
  CHECK(secrets.get_password("nao") == std::optional<std::string>("new"));
}

TEST_CASE("deleting a password clears it from both services") {
  auto legacy = std::make_unique<MemorySecrets>();
  legacy->set_password("nao", "hunter2");
  auto seen = legacy->clone_handle();
  WithLegacy secrets(std::make_unique<MemorySecrets>(), std::move(legacy));
  secrets.delete_password("nao");
  CHECK_FALSE(seen->get_password("nao"));
}

// --- Formats shared with the Rust app. Not in the Rust suite. ----------------

// The file the Rust app wrote on a real machine, with every field, as
// `toml::to_string_pretty` lays it out.
constexpr const char* kRustFile = R"toml(lastUsed = "nethack-alt-org"

[[profiles]]
id = "nethack-alt-org"
name = "NetHack.alt.org"
transport = "ssh"
command = ""
host = "nethack.alt.org"
port = 22
sshUser = "nethack"
gameUser = ""
version = "v50"
tilesetId = "vanilla-5.0.0-16"
fontFamily = "Menlo, DejaVu Sans Mono, Consolas, monospace"
fontSize = 16
scale = 1.0
lineHeight = 1.0
letterSpacing = 6.0
pixelPerfect = true
stateLogEnabled = false
stateLogDirectory = ""

[[profiles]]
id = "hardfought-org"
name = "Hardfought (US)"
transport = "ssh"
command = ""
host = "us.hardfought.org"
port = 22
sshUser = "nethack"
gameUser = ""
version = "v36"
tilesetId = "vanilla-3.6.7-16"
fontFamily = "Menlo, DejaVu Sans Mono, Consolas, monospace"
fontSize = 16
scale = 1.0
lineHeight = 1.0
letterSpacing = 0.0
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = ""
)toml";

TEST_CASE("the file the rust app writes is read field for field") {
  const auto doc = parse_profiles(kRustFile, "profiles.toml");
  CHECK(doc.last_used == std::optional<std::string>("nethack-alt-org"));
  REQUIRE(doc.profiles.size() == 2);
  const auto& nao = doc.profiles[0];
  CHECK(nao.version == NetHackVersion::V50);
  CHECK(nao.tileset_id == "vanilla-5.0.0-16");
  CHECK(nao.letter_spacing == 6.0);
  CHECK(nao.pixel_perfect);
  CHECK(doc.profiles[1].host == "us.hardfought.org");
}

TEST_CASE("a file this app writes reads back as the same document") {
  const auto doc = parse_profiles(kRustFile, "profiles.toml");
  CHECK(parse_profiles(format_profiles(doc), "again") == doc);
}

TEST_CASE("the written file uses the rust app's keys and value types") {
  // The Rust app must read what this app writes: camelCase keys, versions
  // as "v36", floats as floats even when they are whole numbers.
  const auto text = format_profiles(parse_profiles(kRustFile, "profiles.toml"));
  for (const char* expected :
       {"lastUsed = \"nethack-alt-org\"", "[[profiles]]", "sshUser = \"nethack\"",
        "version = \"v50\"", "transport = \"ssh\"", "letterSpacing = 6.0", "scale = 1.0",
        "fontSize = 16", "pixelPerfect = true", "stateLogDirectory = \"\""}) {
    INFO(expected);
    CHECK(text.find(expected) != std::string::npos);
  }
}

TEST_CASE("a document with no last used profile writes no key for it") {
  // The Rust app skips the key when it is empty, and reads its absence.
  ProfileDocument doc;
  doc.profiles.push_back(sample());
  CHECK(format_profiles(doc).find("lastUsed") == std::string::npos);
}

TEST_CASE("a whole number written for a float setting is read") {
  // A hand-edited file says `scale = 2`. serde reads that into an f32.
  Fixture f("int-float");
  write(f.path,
        "[[profiles]]\nid = \"x\"\nname = \"X\"\nhost = \"h\"\nsshUser = \"u\"\n"
        "tilesetId = \"t\"\nscale = 2\n");
  CHECK(store(f.path).get("x")->scale == 2.0);
}

TEST_CASE("a profile without a required field is refused") {
  // id, name, host, sshUser and tilesetId have no default in the Rust app.
  CHECK(error_kind([] {
          parse_profiles("[[profiles]]\nid = \"x\"\nname = \"X\"\nhost = \"h\"\n", "f");
        }) == Kind::Parse);
}

TEST_CASE("an unknown version or transport is refused") {
  const std::string base =
      "[[profiles]]\nid = \"x\"\nname = \"X\"\nhost = \"h\"\nsshUser = \"u\"\ntilesetId = \"t\"\n";
  CHECK(error_kind([&] { parse_profiles(base + "version = \"v34\"\n", "f"); }) == Kind::Parse);
  CHECK(error_kind([&] { parse_profiles(base + "transport = \"telnet\"\n", "f"); }) ==
        Kind::Parse);
}

TEST_CASE("a port outside 0 to 65535 is refused") {
  CHECK(error_kind([] {
          parse_profiles(
              "[[profiles]]\nid = \"x\"\nname = \"X\"\nhost = \"h\"\nsshUser = \"u\"\n"
              "tilesetId = \"t\"\nport = 70000\n",
              "f");
        }) == Kind::Parse);
}

TEST_CASE("a profile round trips through the json the page sends") {
  auto p = sample();
  p.transport = Transport::Local;
  p.version = NetHackVersion::V50;
  p.letter_spacing = 6.5f;
  const nlohmann::json j = p;
  CHECK(j.at("sshUser") == "nethack");
  CHECK(j.at("tilesetId") == "vanilla-3.6.7-16");
  CHECK(j.at("transport") == "local");
  CHECK(j.at("version") == "v50");
  CHECK(j.get<Profile>() == p);
}

TEST_CASE("the json from the page may leave out fields that have a default") {
  const auto p = nlohmann::json::parse(
                     R"({"id":"x","name":"X","host":"h","sshUser":"u","tilesetId":"t"})")
                     .get<Profile>();
  CHECK(p.port == 22);
  CHECK(p.font_size == 16);
}

TEST_CASE("the default path is the one the rust app uses") {
  const auto path = ProfileStore::default_path().generic_string();
#ifdef _WIN32
  // directories::ProjectDirs::from("io", "statico", "nethack-tiles"):
  // %APPDATA%\statico\nethack-tiles\config.
  CHECK(path.ends_with("/statico/nethack-tiles/config/profiles.toml"));
  CHECK(ProfileStore::legacy_path().generic_string().ends_with(
      "/ian/nethack-tiles/config/profiles.toml"));
#elif defined(__APPLE__)
  CHECK(path.ends_with("/Library/Application Support/io.statico.nethack-tiles/profiles.toml"));
#else
  CHECK(path.ends_with("/nethack-tiles/profiles.toml"));
#endif
}

TEST_CASE("a float setting is written in its shortest form") {
  // The Rust app writes `lineHeight = 1.4`, not 1.3999999999999999. A file
  // that both apps rewrite must not gain digits on each save.
  ProfileDocument doc;
  auto p = sample();
  p.line_height = 1.4f;
  p.letter_spacing = 0.1f;
  doc.profiles.push_back(p);
  const auto text = format_profiles(doc);
  INFO(text);
  CHECK(text.find("lineHeight = 1.4\n") != std::string::npos);
  CHECK(text.find("letterSpacing = 0.1\n") != std::string::npos);
}

TEST_CASE("the file the rust app wrote is written back byte for byte") {
  // The two apps share the file. A save from this one must not reorder keys
  // or change the layout.
  CHECK(format_profiles(parse_profiles(kRustFile, "profiles.toml")) == kRustFile);
}
