#pragma once

// Saved connection profiles.
//
// Port of src-tauri/src/profiles.rs. Profiles live in a TOML file under the
// OS config directory. Passwords never go in that file: they go to the OS
// keychain, keyed by profile id, behind the SecretStore interface, so the
// store's logic can be tested without the real keychain.
//
// The file and the keychain entries are the Rust app's own, in the same
// place and the same format, so either app reads what the other saved.
//
// The stored password is the dgamelaunch account password, not an SSH
// credential: NAO and Hardfought take an SSH connection as a shared game user
// and then ask for the game account inside the terminal.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/glyph.hpp"

namespace nh {

// Service name for passwords in the OS keychain. It matches the bundle
// identifier, which is how a keychain is usually read.
inline constexpr const char* kKeychainService = "io.statico.nethack-tiles";

// The identifier before that. Passwords filed under it are read through and
// copied forward; see WithLegacy.
inline constexpr const char* kLegacyKeychainService = "com.ian.nethack-tiles";

// How a profile reaches its game.
enum class Transport {
  // A public server over SSH, through dgamelaunch.
  Ssh,
  // A NetHack on this machine, in a pseudo-terminal.
  Local,
};

// One saved server connection and its display settings. The JSON and TOML
// keys are the camelCase field names.
struct Profile {
  // Stable id. It is also the keychain account key.
  std::string id;
  // The name in the picker.
  std::string name;
  // SSH when absent, so profiles written before local play existed still load.
  Transport transport = Transport::Ssh;
  // The local NetHack to run. Empty means "find one at connect time", so a
  // profile does not go stale when NetHack is upgraded or moved.
  std::string command;
  std::string host;
  std::uint16_t port = 22;
  // The shared SSH user the server publishes, e.g. `nethack`.
  std::string ssh_user;
  // The dgamelaunch account name, typed at the prompt in the terminal.
  std::string game_user;
  // The release the server runs. It selects the glyph flag layout.
  NetHackVersion version = NetHackVersion::V36;
  std::string tileset_id;
  std::string font_family = "Menlo, DejaVu Sans Mono, Consolas, monospace";
  std::uint32_t font_size = 16;
  // f32 in the Rust app, so the two apps hold and write the same values.
  float scale = 1.0f;
  // Cell height as a multiple of the font size. A tile fills its cell, so
  // this is how tall the tiles are.
  float line_height = 1.0f;
  // Extra pixels of cell width. Terminal cells are much narrower than they
  // are tall, which squashes a square tile; this widens them again.
  float letter_spacing = 0.0f;
  // Draw tiles at a whole multiple of their size, centred in the cell, in
  // place of stretched to fill it.
  bool pixel_perfect = false;
  // Write live game-state snapshots for an LLM to read.
  bool state_log_enabled = false;
  // Where those snapshots go. Empty means "not set".
  std::string state_log_directory;

  // A new profile with the defaults above.
  static Profile make(std::string id, std::string name);

  bool operator==(const Profile&) const = default;
};

void to_json(nlohmann::json& j, const Profile& p);
// Fields with a default may be absent. The others are required, as in the
// Rust app.
void from_json(const nlohmann::json& j, Profile& p);

std::ostream& operator<<(std::ostream& os, const Profile& p);

// The games to offer someone who has never used the app.
//
// `local` is the NetHack found on this machine, with the release it reported.
// A local game is offered only when there is one to run. The public servers
// come first: their leaderboards are the point of the app.
//
// `tileset_id` stays empty: which sheet fits a version is the tileset
// registry's business, and the caller fills it in.
std::vector<Profile> default_profiles(
    const std::optional<std::pair<std::filesystem::path, NetHackVersion>>& local);

// The file on disk.
struct ProfileDocument {
  std::vector<Profile> profiles;
  // The profile to preselect at launch.
  std::optional<std::string> last_used;

  bool operator==(const ProfileDocument&) const = default;
};

class ProfileError : public std::runtime_error {
 public:
  enum class Kind { Read, Write, Parse, NoSuchProfile, NoConfigDir, Secret };

  ProfileError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

// Throws ProfileError(Parse) with the parser's reason. `source` names the
// file in the message.
ProfileDocument parse_profiles(std::string_view toml, const std::string& source);
std::string format_profiles(const ProfileDocument& doc);

// Somewhere to keep passwords that is not the config file. Throws
// ProfileError(Secret).
class SecretStore {
 public:
  virtual ~SecretStore() = default;
  virtual void set_password(const std::string& profile_id, const std::string& password) = 0;
  virtual std::optional<std::string> get_password(const std::string& profile_id) = 0;
  virtual void delete_password(const std::string& profile_id) = 0;
};

// A secret store in memory, for tests and for a machine with no keychain.
class MemorySecrets final : public SecretStore {
 public:
  MemorySecrets() : entries_(std::make_shared<std::map<std::string, std::string>>()) {}

  // A second view of the same entries, to look at what a store that was
  // handed away was told.
  std::unique_ptr<MemorySecrets> clone_handle() const;

  void set_password(const std::string& profile_id, const std::string& password) override;
  std::optional<std::string> get_password(const std::string& profile_id) override;
  void delete_password(const std::string& profile_id) override;

 private:
  std::shared_ptr<std::map<std::string, std::string>> entries_;
};

// A secret store that still sees what an earlier version of the app saved.
//
// The bundle identifier moved from `com.ian` to `io.statico`, and the
// keychain files passwords under it. A lookup that misses falls through to
// the old service and copies what it finds forward, so nobody types their
// passwords again.
class WithLegacy final : public SecretStore {
 public:
  WithLegacy(std::unique_ptr<SecretStore> primary, std::unique_ptr<SecretStore> legacy)
      : primary_(std::move(primary)), legacy_(std::move(legacy)) {}

  void set_password(const std::string& profile_id, const std::string& password) override;
  std::optional<std::string> get_password(const std::string& profile_id) override;
  void delete_password(const std::string& profile_id) override;

 private:
  std::unique_ptr<SecretStore> primary_;
  std::unique_ptr<SecretStore> legacy_;
};

// Profiles on disk, and the secret store their passwords live in.
class ProfileStore {
 public:
  // The config file under the app's bundle identifier. Throws
  // ProfileError(NoConfigDir).
  static std::filesystem::path default_path();
  // Where the file was under the previous bundle identifier.
  static std::filesystem::path legacy_path();

  // Copies the profiles saved under the old identifier to the current place,
  // once, if nothing is saved there yet. A copy, not a move: if this version
  // is a mistake, the previous one still finds its own file.
  static void adopt_legacy(const std::filesystem::path& current,
                           const std::filesystem::path& legacy);

  // Loads `path`. A missing file is a first run with no profiles.
  static ProfileStore load(std::filesystem::path path, std::unique_ptr<SecretStore> secrets);

  // True when no config file existed, so nothing has been chosen yet. This is
  // not "no profiles": someone who deleted every profile has made a choice,
  // and must not get the defaults back.
  bool is_first_run() const { return first_run_; }

  const std::filesystem::path& path() const { return path_; }
  const std::vector<Profile>& profiles() const { return doc_.profiles; }
  const std::optional<std::string>& last_used() const { return doc_.last_used; }
  const Profile* get(const std::string& id) const;

  // Inserts `profile`, or replaces the one with the same id in its place.
  void upsert(Profile profile);
  // Removes a profile and its stored password.
  void remove(const std::string& id);
  // Records `id` as the profile to preselect at the next launch.
  void set_last_used(const std::string& id);

  void set_password(const std::string& id, const std::string& password);
  std::optional<std::string> password(const std::string& id);

 private:
  ProfileStore(std::filesystem::path path, ProfileDocument doc,
               std::unique_ptr<SecretStore> secrets, bool first_run)
      : path_(std::move(path)), doc_(std::move(doc)), secrets_(std::move(secrets)),
        first_run_(first_run) {}

  void repair_hosts();
  void save() const;

  std::filesystem::path path_;
  ProfileDocument doc_;
  std::unique_ptr<SecretStore> secrets_;
  bool first_run_;
};

}  // namespace nh
