// Port of the tests in src-tauri/src/ssh.rs, plus the host key decisions
// that russh made inside its handler and that are a pure function here.

#include <doctest/doctest.h>

#include <string>

#include "net/ssh.hpp"

using namespace nh::ssh;
using Kind = SshError::Kind;

TEST_CASE("dgamelaunch defaults try no credential then the shared password") {
  const auto c = Config::dgamelaunch("nethack.alt.org", 22, "nethack");
  CHECK(c.user == "nethack");
  REQUIRE(c.auth.size() == 2);
  CHECK(std::holds_alternative<AuthNone>(c.auth[0]));
  REQUIRE(std::holds_alternative<AuthPassword>(c.auth[1]));
  CHECK(std::get<AuthPassword>(c.auth[1]).password == "nethack");
}

TEST_CASE("the default host key policy is trust on first use not accept any") {
  // A default of AcceptAny would turn off protection from a man in the
  // middle without a word.
  CHECK(Config{}.host_key_policy == HostKeyPolicy::TrustOnFirstUse);
  CHECK(Config::dgamelaunch("h", 22, "u").host_key_policy == HostKeyPolicy::TrustOnFirstUse);
}

TEST_CASE("connecting is bounded so a stalled handshake cannot hang the ui") {
  const auto c = Config::dgamelaunch("h", 22, "u");
  CHECK(c.connect_timeout > std::chrono::seconds(0));
  CHECK(c.connect_timeout <= std::chrono::seconds(60));
}

TEST_CASE("an unreadable known_hosts is not reported as a missing entry") {
  // To send someone to look for a missing line in a file that was never
  // opened wastes their afternoon.
  const std::string rendered =
      SshError::host_key_uncheckable("nethack.alt.org", 22, "Permission denied").what();
  CHECK(rendered.find("could not check") != std::string::npos);
  CHECK(rendered.find("Permission denied") != std::string::npos);
}

// --- Host key decisions -------------------------------------------------------

TEST_CASE("a key that matches the record is accepted under every policy") {
  for (const auto policy :
       {HostKeyPolicy::Strict, HostKeyPolicy::TrustOnFirstUse, HostKeyPolicy::AcceptAny}) {
    CHECK(decide_host_key(KnownHost::Ok, policy) ==
          HostKeyDecision{HostKeyAction::Accept, std::nullopt});
  }
}

TEST_CASE("trust on first use records an unknown host") {
  CHECK(decide_host_key(KnownHost::Unknown, HostKeyPolicy::TrustOnFirstUse) ==
        HostKeyDecision{HostKeyAction::Learn, std::nullopt});
  // No known_hosts file yet is the same as no entry: the first connection
  // ever makes the file.
  CHECK(decide_host_key(KnownHost::NotFound, HostKeyPolicy::TrustOnFirstUse) ==
        HostKeyDecision{HostKeyAction::Learn, std::nullopt});
}

TEST_CASE("trust on first use refuses a changed key") {
  CHECK(decide_host_key(KnownHost::Changed, HostKeyPolicy::TrustOnFirstUse) ==
        HostKeyDecision{HostKeyAction::Reject, Kind::HostKeyChanged});
}

TEST_CASE("a host on record only with another key type is treated as changed") {
  // russh refused any key that differed from the record. To learn the new
  // type here would let a man in the middle in by offering a type the record
  // does not have.
  CHECK(decide_host_key(KnownHost::Other, HostKeyPolicy::TrustOnFirstUse) ==
        HostKeyDecision{HostKeyAction::Reject, Kind::HostKeyChanged});
}

TEST_CASE("strict refuses an unknown host") {
  CHECK(decide_host_key(KnownHost::Unknown, HostKeyPolicy::Strict) ==
        HostKeyDecision{HostKeyAction::Reject, Kind::HostKeyUnknown});
  CHECK(decide_host_key(KnownHost::NotFound, HostKeyPolicy::Strict) ==
        HostKeyDecision{HostKeyAction::Reject, Kind::HostKeyUnknown});
}

TEST_CASE("a known_hosts that cannot be read refuses the connection") {
  CHECK(decide_host_key(KnownHost::Error, HostKeyPolicy::TrustOnFirstUse) ==
        HostKeyDecision{HostKeyAction::Reject, Kind::HostKeyUncheckable});
}

TEST_CASE("accept any accepts even a changed key and records nothing") {
  for (const auto known : {KnownHost::Changed, KnownHost::Other, KnownHost::Unknown,
                           KnownHost::NotFound, KnownHost::Error}) {
    CHECK(decide_host_key(known, HostKeyPolicy::AcceptAny) ==
          HostKeyDecision{HostKeyAction::Accept, std::nullopt});
  }
}

TEST_CASE("the error messages match the rust app") {
  CHECK(std::string(SshError::host_key_unknown("h", 22).what()) ==
        "the host key for h:22 is not in ~/.ssh/known_hosts");
  CHECK(std::string(SshError::connect_timeout("h", 22, std::chrono::seconds(20)).what()) ==
        "h:22 did not answer within 20s");
  CHECK(std::string(SshError::auth_failed().what()) ==
        "the server rejected every configured authentication method");
  CHECK(std::string(SshError::host_key_changed("h", 22).what()).find("has changed") !=
        std::string::npos);
}
