#include "core/profiles.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>


#ifdef _WIN32
#include <windows.h>
// After windows.h, which it depends on.
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace nh {

namespace {

// Saved hosts that cannot work, and what they should be.
//
// `hardfought.org` shipped as a default in an earlier build. It is the
// website, behind Cloudflare, which does not pass port 22, so it resolves but
// never takes SSH. Anyone who ran that build has it saved.
constexpr std::pair<const char*, const char*> kHostRepairs[] = {
    {"hardfought.org", "us.hardfought.org"},
};

std::string utf8(const fs::path& path) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

const char* transport_name(Transport transport) {
  return transport == Transport::Ssh ? "ssh" : "local";
}

Transport parse_transport(const std::string& text) {
  if (text == "ssh") return Transport::Ssh;
  if (text == "local") return Transport::Local;
  throw std::invalid_argument("unknown transport \"" + text + "\"");
}

NetHackVersion parse_version(const std::string& text) {
  if (text == "v36") return NetHackVersion::V36;
  if (text == "v50") return NetHackVersion::V50;
  throw std::invalid_argument("unknown NetHack version \"" + text + "\"");
}

const char* version_text(NetHackVersion version) {
  return version == NetHackVersion::V36 ? "v36" : "v50";
}

// --- Reading a profile: the same rules for TOML and JSON -------------------

// Reads the fields of one profile from `get`, which returns the field by its
// camelCase key. Required fields are the ones without a serde default in the
// Rust app.
template <typename Source>
Profile read_profile(const Source& source) {
  Profile p;
  p.id = source.template required<std::string>("id");
  p.name = source.template required<std::string>("name");
  if (auto v = source.template optional<std::string>("transport")) p.transport = parse_transport(*v);
  p.command = source.template optional<std::string>("command").value_or("");
  p.host = source.template required<std::string>("host");
  if (auto v = source.template optional<std::int64_t>("port")) {
    if (*v < 0 || *v > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("port " + std::to_string(*v) + " is out of range");
    }
    p.port = static_cast<std::uint16_t>(*v);
  }
  p.ssh_user = source.template required<std::string>("sshUser");
  p.game_user = source.template optional<std::string>("gameUser").value_or("");
  if (auto v = source.template optional<std::string>("version")) p.version = parse_version(*v);
  p.tileset_id = source.template required<std::string>("tilesetId");
  if (auto v = source.template optional<std::string>("fontFamily")) p.font_family = *v;
  if (auto v = source.template optional<std::int64_t>("fontSize")) {
    if (*v < 0 || *v > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("fontSize " + std::to_string(*v) + " is out of range");
    }
    p.font_size = static_cast<std::uint32_t>(*v);
  }
  if (auto v = source.template optional<double>("scale")) p.scale = static_cast<float>(*v);
  if (auto v = source.template optional<double>("lineHeight")) p.line_height = static_cast<float>(*v);
  if (auto v = source.template optional<double>("letterSpacing")) {
    p.letter_spacing = static_cast<float>(*v);
  }
  p.pixel_perfect = source.template optional<bool>("pixelPerfect").value_or(false);
  p.state_log_enabled = source.template optional<bool>("stateLogEnabled").value_or(false);
  p.state_log_directory = source.template optional<std::string>("stateLogDirectory").value_or("");
  return p;
}

struct TomlSource {
  const toml::table& table;

  template <typename T>
  std::optional<T> optional(const char* key) const {
    const toml::node* node = table.get(key);
    if (!node) return std::nullopt;
    if constexpr (std::is_same_v<T, double>) {
      // serde reads a whole number into an f32, so `scale = 2` is fine.
      if (const auto v = node->value<double>()) return *v;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
      if (node->is_integer()) return node->as_integer()->get();
    } else {
      if (const auto v = node->value_exact<T>()) return *v;
    }
    throw std::invalid_argument(std::string("`") + key + "` has the wrong type");
  }

  template <typename T>
  T required(const char* key) const {
    if (auto v = optional<T>(key)) return *v;
    throw std::invalid_argument(std::string("missing field `") + key + "`");
  }
};

struct JsonSource {
  const nlohmann::json& json;

  template <typename T>
  std::optional<T> optional(const char* key) const {
    const auto found = json.find(key);
    if (found == json.end() || found->is_null()) return std::nullopt;
    return found->template get<T>();
  }

  template <typename T>
  T required(const char* key) const {
    if (auto v = optional<T>(key)) return *v;
    throw std::invalid_argument(std::string("missing field `") + key + "`");
  }
};

void write_file(const fs::path& path, const std::string& text) {
  std::error_code error;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), error);
    if (error) {
      throw ProfileError(ProfileError::Kind::Write,
                         "writing " + utf8(path.parent_path()) + ": " + error.message());
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  out.close();
  if (!out) throw ProfileError(ProfileError::Kind::Write, "writing " + utf8(path) + ": the write failed");
}

// The OS config directory, as the Rust `directories` crate finds it for
// ProjectDirs::from(qualifier, organization, "nethack-tiles").
fs::path config_dir(const char* qualifier, const char* organization) {
  constexpr const char* kApplication = "nethack-tiles";
#ifdef _WIN32
  (void)qualifier;
  PWSTR roaming = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
    CoTaskMemFree(roaming);
    throw ProfileError(ProfileError::Kind::NoConfigDir, "could not locate an OS config directory");
  }
  const fs::path base(roaming);
  CoTaskMemFree(roaming);
  return base / organization / kApplication / "config";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    throw ProfileError(ProfileError::Kind::NoConfigDir, "could not locate an OS config directory");
  }
  return fs::path(home) / "Library" / "Application Support" /
         (std::string(qualifier) + "." + organization + "." + kApplication);
#else
  (void)qualifier;
  (void)organization;
  // XDG names only the application, so the old identifier is the same place.
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && fs::path(xdg).is_absolute()) {
    return fs::path(xdg) / kApplication;
  }
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    throw ProfileError(ProfileError::Kind::NoConfigDir, "could not locate an OS config directory");
  }
  return fs::path(home) / ".config" / kApplication;
#endif
}

}  // namespace

Profile Profile::make(std::string id, std::string name) {
  Profile p;
  p.id = std::move(id);
  p.name = std::move(name);
  return p;
}

void to_json(nlohmann::json& j, const Profile& p) {
  j = {
      {"id", p.id},
      {"name", p.name},
      {"transport", transport_name(p.transport)},
      {"command", p.command},
      {"host", p.host},
      {"port", p.port},
      {"sshUser", p.ssh_user},
      {"gameUser", p.game_user},
      {"version", version_text(p.version)},
      {"tilesetId", p.tileset_id},
      {"fontFamily", p.font_family},
      {"fontSize", p.font_size},
      {"scale", p.scale},
      {"lineHeight", p.line_height},
      {"letterSpacing", p.letter_spacing},
      {"pixelPerfect", p.pixel_perfect},
      {"stateLogEnabled", p.state_log_enabled},
      {"stateLogDirectory", p.state_log_directory},
  };
}

void from_json(const nlohmann::json& j, Profile& p) { p = read_profile(JsonSource{j}); }

std::ostream& operator<<(std::ostream& os, const Profile& p) {
  return os << nlohmann::json(p).dump();
}

std::vector<Profile> default_profiles(
    const std::optional<std::pair<fs::path, NetHackVersion>>& local) {
  // NAO's play menu offers 5.0.0 and nothing else.
  auto nao = Profile::make("nethack-alt-org", "NetHack.alt.org");
  nao.host = "nethack.alt.org";
  nao.ssh_user = "nethack";
  nao.version = NetHackVersion::V50;

  // Hardfought's SSH is on regional hosts, not the bare domain, which is the
  // website behind Cloudflare. `eu.` and `au.` work the same; accounts are
  // made on the US host and copied to the others.
  auto hardfought = Profile::make("hardfought-org", "Hardfought (US)");
  hardfought.host = "us.hardfought.org";
  hardfought.ssh_user = "nethack";
  hardfought.version = NetHackVersion::V36;

  std::vector<Profile> profiles{nao, hardfought};
  if (local) {
    auto game = Profile::make("local-nethack", "This computer");
    game.transport = Transport::Local;
    game.command = utf8(local->first);
    game.version = local->second;
    profiles.push_back(game);
  }
  return profiles;
}

ProfileDocument parse_profiles(std::string_view text, const std::string& source) {
  try {
    const toml::table root = toml::parse(text, source);
    ProfileDocument doc;
    if (const toml::node* last = root.get("lastUsed")) {
      const auto value = last->value_exact<std::string>();
      if (!value) throw std::invalid_argument("`lastUsed` has the wrong type");
      doc.last_used = *value;
    }
    if (const toml::node* list = root.get("profiles")) {
      const toml::array* profiles = list->as_array();
      if (!profiles) throw std::invalid_argument("`profiles` is not a list of tables");
      for (const toml::node& entry : *profiles) {
        const toml::table* table = entry.as_table();
        if (!table) throw std::invalid_argument("`profiles` is not a list of tables");
        doc.profiles.push_back(read_profile(TomlSource{*table}));
      }
    }
    return doc;
  } catch (const toml::parse_error& e) {
    std::ostringstream reason;
    reason << e.description() << " at line " << e.source().begin.line << ", column "
           << e.source().begin.column;
    throw ProfileError(ProfileError::Kind::Parse,
                       source + " is not valid profile TOML: " + reason.str());
  } catch (const std::invalid_argument& e) {
    throw ProfileError(ProfileError::Kind::Parse, source + " is not valid profile TOML: " + e.what());
  }
}

// --- Writing: the layout of the Rust app's `toml::to_string_pretty` -----------
//
// The file is shared with the Rust app, so this writes it byte for byte as
// that app does: the same key order, blank lines and string styles. Then a
// switch from one app to the other never shows a change that is not real.
// toml++ reads the file, but its writer sorts the keys.

namespace {

// How toml_writer 1.1.2 (`string.rs`) picks a string style: plain "..." if
// nothing needs an escape, then '...', then the multi-line forms, and
// escaped "..." last.
struct StringMetrics {
  int max_single_quotes = 0;
  int max_double_quotes = 0;
  bool escape_codes = false;  // a control character other than tab and newline
  bool backslash = false;
  bool newline = false;

  explicit StringMetrics(std::string_view s) {
    int singles = 0, doubles = 0;
    for (const char c : s) {
      const auto b = static_cast<unsigned char>(c);
      singles = b == '\'' ? singles + 1 : 0;
      doubles = b == '"' ? doubles + 1 : 0;
      max_single_quotes = std::max(max_single_quotes, singles);
      max_double_quotes = std::max(max_double_quotes, doubles);
      if (b == '\\') {
        backslash = true;
      } else if (b == '\n') {
        newline = true;
      } else if (b != '\t' && (b <= 0x1f || b == 0x7f)) {
        escape_codes = true;
      }
    }
  }
};

// The body of a "..." or """...""" string, with toml_writer's escapes.
std::string escape_basic(std::string_view s, bool multi_line) {
  const int max_quotes = multi_line ? 2 : 0;
  std::string out;
  int quotes = 0;
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    if (b == '"') {
      if (++quotes > max_quotes) {
        out += "\\\"";
        continue;
      }
      out += c;
      continue;
    }
    quotes = 0;
    switch (b) {
      case 0x08: out += "\\b"; break;
      case 0x09: out += "\\t"; break;
      case 0x0a: out += multi_line ? "\n" : "\\n"; break;
      case 0x0c: out += "\\f"; break;
      case 0x0d: out += "\\r"; break;
      case 0x5c: out += "\\\\"; break;
      default:
        if (b <= 0x1f || b == 0x7f) {
          char code[8];
          std::snprintf(code, sizeof code, "\\u%04X", b);
          out += code;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string toml_string(std::string_view s) {
  const StringMetrics m(s);
  // Multi-line strings start with a line break when the text has one.
  const std::string lead = m.newline ? "\n" : "";
  if (!m.escape_codes && !m.backslash && m.max_double_quotes == 0 && !m.newline) {
    // Still escaped: a tab needs no escape here, but toml_writer gives it one.
    return "\"" + escape_basic(s, false) + "\"";
  }
  if (!m.escape_codes && m.max_single_quotes == 0 && !m.newline) {
    return "'" + std::string(s) + "'";
  }
  if (!m.escape_codes && !m.backslash && m.max_double_quotes <= 2) {
    return "\"\"\"" + lead + escape_basic(s, true) + "\"\"\"";
  }
  if (!m.escape_codes && m.max_single_quotes <= 2) {
    return "'''" + lead + std::string(s) + "'''";
  }
  if (m.newline) return "\"\"\"" + lead + escape_basic(s, true) + "\"\"\"";
  return "\"" + escape_basic(s, false) + "\"";
}

// An f32 as Rust's `Display` writes it, which the toml crate uses: the
// shortest digits that read back as the same f32, never an exponent, and
// `.0` on a whole number. So 3e10 is "30000000000.0", not the exact
// "30000001024.0", and 1e-7 is "0.0000001".
std::string toml_float(float f) {
  if (std::isnan(f)) return "nan";
  if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
  // The shortest digits, in scientific form: d.ddde[+-]xx.
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof buffer, f, std::chars_format::scientific);
  const std::string text(buffer, end);
  const auto e = text.find('e');
  std::string mantissa = text.substr(0, e);
  const int exponent = std::stoi(text.substr(e + 1));

  std::string sign;
  if (mantissa.front() == '-') {
    sign = "-";
    mantissa.erase(0, 1);
  }
  std::string digits = mantissa;
  digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());

  // The decimal point goes after `point` digits; pad with zeros either side.
  const int point = exponent + 1;
  std::string out;
  if (point <= 0) {
    out = "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
  } else if (point >= static_cast<int>(digits.size())) {
    out = digits + std::string(static_cast<std::size_t>(point) - digits.size(), '0') + ".0";
  } else {
    out = digits.substr(0, static_cast<std::size_t>(point)) + "." +
          digits.substr(static_cast<std::size_t>(point));
  }
  return sign + out;
}

}  // namespace

std::string format_profiles(const ProfileDocument& doc) {
  std::string out;
  const auto line = [&out](const char* key, const std::string& value) {
    out += key;
    out += " = ";
    out += value;
    out += "\n";
  };
  const auto boolean = [](bool v) { return std::string(v ? "true" : "false"); };

  // Keys go in field order, so an empty list comes first. A list of profiles
  // is written as tables, and tables follow every plain key. `lastUsed` is
  // left out when empty, as the Rust app does.
  if (doc.profiles.empty()) {
    line("profiles", "[]");
    if (doc.last_used) line("lastUsed", toml_string(*doc.last_used));
    return out;
  }
  if (doc.last_used) line("lastUsed", toml_string(*doc.last_used));
  for (const auto& p : doc.profiles) {
    if (!out.empty()) out += "\n";
    out += "[[profiles]]\n";
    line("id", toml_string(p.id));
    line("name", toml_string(p.name));
    line("transport", toml_string(transport_name(p.transport)));
    line("command", toml_string(p.command));
    line("host", toml_string(p.host));
    line("port", std::to_string(p.port));
    line("sshUser", toml_string(p.ssh_user));
    line("gameUser", toml_string(p.game_user));
    line("version", toml_string(version_text(p.version)));
    line("tilesetId", toml_string(p.tileset_id));
    line("fontFamily", toml_string(p.font_family));
    line("fontSize", std::to_string(p.font_size));
    line("scale", toml_float(p.scale));
    line("lineHeight", toml_float(p.line_height));
    line("letterSpacing", toml_float(p.letter_spacing));
    line("pixelPerfect", boolean(p.pixel_perfect));
    line("stateLogEnabled", boolean(p.state_log_enabled));
    line("stateLogDirectory", toml_string(p.state_log_directory));
  }
  return out;
}

// --- Secret stores ---------------------------------------------------------------

std::unique_ptr<MemorySecrets> MemorySecrets::clone_handle() const {
  auto other = std::make_unique<MemorySecrets>();
  other->entries_ = entries_;
  return other;
}

void MemorySecrets::set_password(const std::string& profile_id, const std::string& password) {
  (*entries_)[profile_id] = password;
}

std::optional<std::string> MemorySecrets::get_password(const std::string& profile_id) {
  const auto found = entries_->find(profile_id);
  if (found == entries_->end()) return std::nullopt;
  return found->second;
}

void MemorySecrets::delete_password(const std::string& profile_id) { entries_->erase(profile_id); }

void WithLegacy::set_password(const std::string& profile_id, const std::string& password) {
  primary_->set_password(profile_id, password);
}

std::optional<std::string> WithLegacy::get_password(const std::string& profile_id) {
  if (auto password = primary_->get_password(profile_id)) return password;
  auto password = legacy_->get_password(profile_id);
  if (!password) return std::nullopt;
  // Best effort: a keychain that refuses the write still lets the player in
  // this time, which beats a refusal to connect.
  try {
    primary_->set_password(profile_id, *password);
  } catch (const ProfileError& e) {
    std::cerr << "[profiles] could not copy " << profile_id
              << "'s password to the current keychain entry: " << e.what() << "\n";
  }
  return password;
}

void WithLegacy::delete_password(const std::string& profile_id) {
  primary_->delete_password(profile_id);
  legacy_->delete_password(profile_id);
}

// --- The store ----------------------------------------------------------------

fs::path ProfileStore::default_path() { return config_dir("io", "statico") / "profiles.toml"; }

fs::path ProfileStore::legacy_path() { return config_dir("com", "ian") / "profiles.toml"; }

void ProfileStore::adopt_legacy(const fs::path& current, const fs::path& legacy) {
  if (fs::exists(current) || !fs::exists(legacy)) return;
  std::error_code error;
  if (current.has_parent_path()) fs::create_directories(current.parent_path(), error);
  if (!error) fs::copy_file(legacy, current, error);
  if (error) {
    throw ProfileError(ProfileError::Kind::Write, "writing " + utf8(current) + ": " + error.message());
  }
  std::cerr << "[profiles] adopted the profiles saved at " << utf8(legacy)
            << " under the previous app identifier\n";
}

ProfileStore ProfileStore::load(fs::path path, std::unique_ptr<SecretStore> secrets) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::error_code error;
    if (!fs::exists(path, error) && !error) {
      return ProfileStore(std::move(path), {}, std::move(secrets), true);
    }
    throw ProfileError(ProfileError::Kind::Read, "reading " + utf8(path) + ": cannot open the file");
  }
  std::ostringstream text;
  text << in.rdbuf();
  ProfileStore store(path, parse_profiles(text.str(), utf8(path)), std::move(secrets), false);
  store.repair_hosts();
  return store;
}

// Rewrites saved hosts that are known not to work. Only exact matches are
// changed, so a host the player typed is never second-guessed. A failed
// write is logged, not thrown: the repair is already in memory, and to
// refuse to start over it would be worse than to redo it next launch.
void ProfileStore::repair_hosts() {
  std::string repaired;
  for (auto& profile : doc_.profiles) {
    for (const auto& [bad, fixed] : kHostRepairs) {
      if (profile.host != bad) continue;
      repaired += (repaired.empty() ? "" : ", ") + profile.host + " -> " + fixed;
      profile.host = fixed;
    }
  }
  if (repaired.empty()) return;
  std::cerr << "[profiles] repaired unreachable profile hosts: " << repaired << "\n";
  try {
    save();
  } catch (const ProfileError& e) {
    std::cerr << "[profiles] could not write the repaired hosts back: " << e.what() << "\n";
  }
}

const Profile* ProfileStore::get(const std::string& id) const {
  for (const auto& p : doc_.profiles) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

void ProfileStore::upsert(Profile profile) {
  const auto existing = std::find_if(doc_.profiles.begin(), doc_.profiles.end(),
                                     [&](const Profile& p) { return p.id == profile.id; });
  if (existing != doc_.profiles.end()) {
    *existing = std::move(profile);
  } else {
    doc_.profiles.push_back(std::move(profile));
  }
  save();
}

void ProfileStore::remove(const std::string& id) {
  const auto before = doc_.profiles.size();
  std::erase_if(doc_.profiles, [&](const Profile& p) { return p.id == id; });
  if (doc_.profiles.size() == before) {
    throw ProfileError(ProfileError::Kind::NoSuchProfile, "no profile with id \"" + id + "\"");
  }
  if (doc_.last_used == id) doc_.last_used.reset();
  secrets_->delete_password(id);
  save();
}

void ProfileStore::set_last_used(const std::string& id) {
  if (!get(id)) {
    throw ProfileError(ProfileError::Kind::NoSuchProfile, "no profile with id \"" + id + "\"");
  }
  doc_.last_used = id;
  save();
}

void ProfileStore::set_password(const std::string& id, const std::string& password) {
  secrets_->set_password(id, password);
}

std::optional<std::string> ProfileStore::password(const std::string& id) {
  return secrets_->get_password(id);
}

void ProfileStore::save() const { write_file(path_, format_profiles(doc_)); }

}  // namespace nh
