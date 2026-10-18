// The OS keychain. These tests use the real Credential Manager under a test
// service name, and remove what they write.

#include <doctest/doctest.h>

#include <string>
#include <thread>

#include "core/keychain.hpp"

using namespace nh;

#ifdef _WIN32
namespace {

// Unique per run, so two runs at once cannot see each other's entries.
std::string test_service() {
  std::ostringstream name;
  name << "test-nethack-tiles-cpp-" << std::this_thread::get_id() << "-"
       << std::chrono::steady_clock::now().time_since_epoch().count();
  return name.str();
}

}  // namespace
#endif

TEST_CASE("the target name is the one the rust keyring crate makes") {
  // windows-native-keyring-store `Store::new()`: "<user>.<service>".
  CHECK(KeychainSecrets(kKeychainService).target_name("nethack-alt-org") ==
        "nethack-alt-org.io.statico.nethack-tiles");
}

#ifdef _WIN32

TEST_CASE("a password survives a round trip through credential manager") {
  REQUIRE(KeychainSecrets::available());
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "hunter2");
  CHECK(secrets.get_password("profile") == std::optional<std::string>("hunter2"));
  secrets.delete_password("profile");
  CHECK_FALSE(secrets.get_password("profile"));
}

TEST_CASE("a password outside ascii survives the utf-16 encoding") {
  KeychainSecrets secrets(test_service());
  const std::string password = "p\xc3\xa4ss \xe2\x82\xac \xf0\x9f\x90\x89";  // "päss € 🐉"
  secrets.set_password("profile", password);
  CHECK(secrets.get_password("profile") == std::optional<std::string>(password));
  secrets.delete_password("profile");
}

TEST_CASE("an entry that does not exist reads as no password") {
  KeychainSecrets secrets(test_service());
  CHECK_FALSE(secrets.get_password("ghost"));
}

TEST_CASE("deleting an entry that does not exist is not an error") {
  KeychainSecrets secrets(test_service());
  CHECK_NOTHROW(secrets.delete_password("ghost"));
}

TEST_CASE("setting a password again replaces it") {
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "one");
  secrets.set_password("profile", "two");
  CHECK(secrets.get_password("profile") == std::optional<std::string>("two"));
  secrets.delete_password("profile");
}

TEST_CASE("an empty password is stored, not taken as no password") {
  // The Rust app stores whatever string it gets, an empty one too.
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "");
  CHECK(secrets.get_password("profile") == std::optional<std::string>(""));
  secrets.delete_password("profile");
}

#endif
