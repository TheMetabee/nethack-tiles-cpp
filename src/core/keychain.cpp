#include "core/keychain.hpp"

#ifdef _WIN32
#include <windows.h>
// After windows.h, which it depends on.
#include <wincred.h>
#endif

namespace nh {

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw ProfileError(ProfileError::Kind::Secret, "keychain: " + message);
}

#ifdef _WIN32

std::string system_message(DWORD code) {
  char* text = nullptr;
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
  std::string out = n ? std::string(text, n) : "error " + std::to_string(code);
  LocalFree(text);
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == '.')) {
    out.pop_back();
  }
  return out;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) fail("the text is not valid UTF-8");
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s, std::size_t length) {
  if (length == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, static_cast<int>(length),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) fail("the stored password is not valid UTF-16");
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(length), out.data(), n, nullptr, nullptr);
  return out;
}

// Frees a credential that CredReadW returned.
struct CredentialPtr {
  PCREDENTIALW p = nullptr;
  ~CredentialPtr() {
    if (p) CredFree(p);
  }
};

#endif

}  // namespace

std::string KeychainSecrets::target_name(const std::string& profile_id) const {
  return profile_id + "." + service_;
}

#ifdef _WIN32

bool KeychainSecrets::available() { return true; }

void KeychainSecrets::set_password(const std::string& profile_id, const std::string& password) {
  std::wstring target = widen(target_name(profile_id));
  std::wstring user = widen(profile_id);
  std::wstring secret = widen(password);
  const auto blob_size = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
  if (blob_size > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
    SecureZeroMemory(secret.data(), blob_size);
    fail("the password is longer than " + std::to_string(CRED_MAX_CREDENTIAL_BLOB_SIZE) +
         " bytes in UTF-16");
  }

  // Keep the attributes of an entry that is already there, as the keyring
  // crate does when it replaces a password.
  std::wstring alias, comment;
  {
    CredentialPtr existing;
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &existing.p)) {
      if (existing.p->UserName) user = existing.p->UserName;
      if (existing.p->TargetAlias) alias = existing.p->TargetAlias;
      if (existing.p->Comment) comment = existing.p->Comment;
    }
  }

  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = target.data();
  credential.UserName = user.data();
  credential.TargetAlias = alias.data();
  credential.Comment = comment.data();
  credential.CredentialBlobSize = blob_size;
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
  // The keyring crate's default. It follows the user to other machines in a
  // domain, as Windows' own saved passwords do.
  credential.Persist = CRED_PERSIST_ENTERPRISE;

  const BOOL ok = CredWriteW(&credential, 0);
  const DWORD error = ok ? 0 : GetLastError();
  SecureZeroMemory(secret.data(), blob_size);
  if (!ok) fail(system_message(error));
}

std::optional<std::string> KeychainSecrets::get_password(const std::string& profile_id) {
  const std::wstring target = widen(target_name(profile_id));
  CredentialPtr credential;
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential.p)) {
    const DWORD error = GetLastError();
    if (error == ERROR_NOT_FOUND) return std::nullopt;
    fail(system_message(error));
  }
  // Another program may have written an odd number of bytes, which is not
  // UTF-16.
  const DWORD size = credential.p->CredentialBlobSize;
  if (size % 2 != 0) fail("the stored password is not valid UTF-16");
  std::string password =
      narrow(reinterpret_cast<const wchar_t*>(credential.p->CredentialBlob), size / 2);
  if (size) SecureZeroMemory(credential.p->CredentialBlob, size);
  return password;
}

void KeychainSecrets::delete_password(const std::string& profile_id) {
  const std::wstring target = widen(target_name(profile_id));
  if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
    const DWORD error = GetLastError();
    if (error != ERROR_NOT_FOUND) fail(system_message(error));
  }
}

#else

bool KeychainSecrets::available() { return false; }

void KeychainSecrets::set_password(const std::string&, const std::string&) {
  fail("the keychain on this platform is not ported yet");
}

std::optional<std::string> KeychainSecrets::get_password(const std::string&) {
  fail("the keychain on this platform is not ported yet");
}

void KeychainSecrets::delete_password(const std::string&) {
  fail("the keychain on this platform is not ported yet");
}

#endif

}  // namespace nh
