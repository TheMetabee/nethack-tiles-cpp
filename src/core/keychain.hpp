#pragma once

// The OS keychain, as a SecretStore.
//
// It reads and writes the entries the Rust app's `keyring` crate made, so a
// password saved by either app works in the other. On Windows that is a
// generic credential in Credential Manager:
//
//   target name  "<profile id>.<service>", e.g.
//                "nethack-alt-org.io.statico.nethack-tiles"
//   user name    the profile id
//   secret       the password in UTF-16LE, with no terminator
//   persistence  CRED_PERSIST_ENTERPRISE
//
// Source: windows-native-keyring-store 1.1.0, the store keyring 4.1.6 uses in
// its default `v1` mode (`Store::new()`: no prefix or suffix, "." between).
//
// On Linux it is an item in the Secret Service's default collection (the
// login keyring of GNOME, KDE and others), through libsecret:
//
//   attributes   service = <service>, username = <profile id>
//   label        "keyring:<profile id>@<service>"
//   secret       the password as UTF-8 text (text/plain)
//
// Source: zbus-secret-service-keyring-store 1.0.0. That store finds an item
// by those two attributes and refuses a search that finds two, so a save
// here first removes every item that matches them. libsecret also adds an
// `xdg:schema` attribute when it stores; the Rust store ignores it.
//
// The macOS Keychain is not ported yet.

#include <string>

#include "core/profiles.hpp"

namespace nh {

class KeychainSecrets final : public SecretStore {
 public:
  explicit KeychainSecrets(std::string service) : service_(std::move(service)) {}

  // True when this platform's keychain is ported. Without it, every call
  // throws ProfileError(Secret).
  static bool available();

  // The Credential Manager target name for a profile (Windows).
  std::string target_name(const std::string& profile_id) const;

  // The Secret Service item label for a profile (Linux), as the keyring
  // settings of GNOME and KDE show it.
  std::string item_label(const std::string& profile_id) const;

  void set_password(const std::string& profile_id, const std::string& password) override;
  std::optional<std::string> get_password(const std::string& profile_id) override;
  void delete_password(const std::string& profile_id) override;

 private:
  std::string service_;
};

}  // namespace nh
