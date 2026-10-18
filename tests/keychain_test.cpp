// The OS keychain. The round trips use the real one under a test service
// name, and remove what they write:
//
//   Windows  Credential Manager, always.
//   Linux    the Secret Service, only when NH_TEST_SECRET_SERVICE=1, so a
//            plain test run on a desktop shows no keyring prompts. CI sets
//            it inside a D-Bus session of its own with an unlocked keyring.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/keychain.hpp"

#ifdef __linux__
#include <libsecret/secret.h>
#endif

using namespace nh;

namespace {

[[maybe_unused]] bool real_keychain() {
#ifdef _WIN32
  return true;
#elif defined(__linux__)
  const char* opt_in = std::getenv("NH_TEST_SECRET_SERVICE");
  return opt_in && std::string(opt_in) == "1";
#else
  return false;
#endif
}

// Unique per run, so two runs at once cannot see each other's entries.
[[maybe_unused]] std::string test_service() {
  std::ostringstream name;
  name << "test-nethack-tiles-cpp-" << std::this_thread::get_id() << "-"
       << std::chrono::steady_clock::now().time_since_epoch().count();
  return name.str();
}

}  // namespace

// Skips the rest of a test case where the real keychain is not in use.
#define REQUIRE_REAL_KEYCHAIN()                                              \
  if (!real_keychain()) {                                                    \
    MESSAGE("the real keychain is not in use here; set NH_TEST_SECRET_SERVICE=1 on Linux"); \
    return;                                                                  \
  }

TEST_CASE("the target name is the one the rust keyring crate makes") {
  // windows-native-keyring-store `Store::new()`: "<user>.<service>".
  CHECK(KeychainSecrets(kKeychainService).target_name("nethack-alt-org") ==
        "nethack-alt-org.io.statico.nethack-tiles");
}

TEST_CASE("the item label is the one the rust keyring crate makes") {
  // zbus-secret-service-keyring-store: "keyring:{user}@{service}".
  CHECK(KeychainSecrets(kKeychainService).item_label("nethack-alt-org") ==
        "keyring:nethack-alt-org@io.statico.nethack-tiles");
}

#if defined(_WIN32) || defined(__linux__)

TEST_CASE("a password survives a round trip through the keychain") {
  REQUIRE_REAL_KEYCHAIN();
  REQUIRE(KeychainSecrets::available());
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "hunter2");
  CHECK(secrets.get_password("profile") == std::optional<std::string>("hunter2"));
  secrets.delete_password("profile");
  CHECK_FALSE(secrets.get_password("profile"));
}

TEST_CASE("a password outside ascii survives the encoding") {
  REQUIRE_REAL_KEYCHAIN();
  KeychainSecrets secrets(test_service());
  const std::string password = "p\xc3\xa4ss \xe2\x82\xac \xf0\x9f\x90\x89";  // "päss € 🐉"
  secrets.set_password("profile", password);
  CHECK(secrets.get_password("profile") == std::optional<std::string>(password));
  secrets.delete_password("profile");
}

TEST_CASE("an entry that does not exist reads as no password") {
  REQUIRE_REAL_KEYCHAIN();
  KeychainSecrets secrets(test_service());
  CHECK_FALSE(secrets.get_password("ghost"));
}

TEST_CASE("deleting an entry that does not exist is not an error") {
  REQUIRE_REAL_KEYCHAIN();
  KeychainSecrets secrets(test_service());
  CHECK_NOTHROW(secrets.delete_password("ghost"));
}

TEST_CASE("setting a password again replaces it") {
  REQUIRE_REAL_KEYCHAIN();
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "one");
  secrets.set_password("profile", "two");
  CHECK(secrets.get_password("profile") == std::optional<std::string>("two"));
  secrets.delete_password("profile");
}

TEST_CASE("an empty password is stored, not taken as no password") {
  // The Rust app stores whatever string it gets, an empty one too.
  REQUIRE_REAL_KEYCHAIN();
  KeychainSecrets secrets(test_service());
  secrets.set_password("profile", "");
  CHECK(secrets.get_password("profile") == std::optional<std::string>(""));
  secrets.delete_password("profile");
}

#endif

#ifdef __linux__

namespace {

// The attributes the Rust store searches by, and nothing else: no
// xdg:schema, as the Rust store's items have none.
GHashTable* rust_attributes(const std::string& service, const std::string& user) {
  GHashTable* attributes = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attributes, const_cast<char*>("service"), const_cast<char*>(service.c_str()));
  g_hash_table_insert(attributes, const_cast<char*>("username"), const_cast<char*>(user.c_str()));
  return attributes;
}

// Creates an item as zbus-secret-service-keyring-store does: CreateItem in
// the default collection, its two attributes, its label, the password as
// text/plain.
void create_rust_item(const std::string& service, const std::string& user,
                      const std::string& password) {
  GError* error = nullptr;
  SecretService* service_proxy =
      secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, nullptr, &error);
  REQUIRE_MESSAGE(service_proxy, std::string(error ? error->message : "no Secret Service"));
  SecretCollection* collection = secret_collection_for_alias_sync(
      service_proxy, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, nullptr, &error);
  g_object_unref(service_proxy);
  REQUIRE_MESSAGE(collection, std::string(error ? error->message : "no default collection"));
  GHashTable* attributes = rust_attributes(service, user);
  SecretValue* value = secret_value_new(password.c_str(), -1, "text/plain");
  const std::string label = "keyring:" + user + "@" + service;
  SecretItem* item = secret_item_create_sync(collection, nullptr, attributes, label.c_str(), value,
                                             SECRET_ITEM_CREATE_NONE, nullptr, &error);
  REQUIRE_MESSAGE(item, std::string(error ? error->message : "no item"));
  g_object_unref(item);
  secret_value_unref(value);
  g_hash_table_unref(attributes);
  g_object_unref(collection);
}

// Finds the items the Rust store would find for this service and user.
std::vector<SecretItem*> rust_search(const std::string& service, const std::string& user) {
  GError* error = nullptr;
  GHashTable* attributes = rust_attributes(service, user);
  GList* found = secret_service_search_sync(
      nullptr, nullptr, attributes,
      static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK |
                                     SECRET_SEARCH_LOAD_SECRETS),
      nullptr, &error);
  g_hash_table_unref(attributes);
  std::vector<SecretItem*> items;
  for (GList* l = found; l; l = l->next) items.push_back(static_cast<SecretItem*>(l->data));
  g_list_free(found);
  return items;
}

void unref_all(const std::vector<SecretItem*>& items) {
  for (auto* item : items) g_object_unref(item);
}

}  // namespace

TEST_CASE("a password the rust app saved is read") {
  REQUIRE_REAL_KEYCHAIN();
  const std::string service = test_service();
  create_rust_item(service, "profile", "from rust");
  KeychainSecrets secrets(service);
  CHECK(secrets.get_password("profile") == std::optional<std::string>("from rust"));
  secrets.delete_password("profile");
  const auto left = rust_search(service, "profile");
  CHECK(left.empty());
  unref_all(left);
}

TEST_CASE("saving over a rust item leaves the one item the rust app can read") {
  // The Rust store refuses a search that finds two items. A plain libsecret
  // store would add a second one, beside the item without xdg:schema.
  REQUIRE_REAL_KEYCHAIN();
  const std::string service = test_service();
  create_rust_item(service, "profile", "old");
  KeychainSecrets(service).set_password("profile", "new");

  const auto items = rust_search(service, "profile");
  REQUIRE(items.size() == 1);
  SecretValue* value = secret_item_get_secret(items[0]);
  REQUIRE(value);
  CHECK(std::string(secret_value_get_text(value)) == "new");
  CHECK(std::string(secret_value_get_content_type(value)) == "text/plain");
  secret_value_unref(value);
  gchar* label = secret_item_get_label(items[0]);
  CHECK(std::string(label) == "keyring:profile@" + service);
  g_free(label);
  unref_all(items);
  KeychainSecrets(service).delete_password("profile");
}

#endif
