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
// The macOS Keychain and the Linux Secret Service are not ported yet.

#include <string>

#include "core/profiles.hpp"

namespace nh {

class KeychainSecrets final : public SecretStore {
 public:
  explicit KeychainSecrets(std::string service) : service_(std::move(service)) {}

  // True when this platform's keychain is ported. Without it, every call
  // throws ProfileError(Secret).
  static bool available();

  // The Credential Manager target name for a profile.
  std::string target_name(const std::string& profile_id) const;

  void set_password(const std::string& profile_id, const std::string& password) override;
  std::optional<std::string> get_password(const std::string& profile_id) override;
  void delete_password(const std::string& profile_id) override;

 private:
  std::string service_;
};

}  // namespace nh
