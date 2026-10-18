// Port of the tests in src-tauri/src/banner.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <string>

#include "core/banner.hpp"

using nh::ServerVersion;
using nh::VersionWatch;
using nh::NetHackVersion::V36;
using nh::NetHackVersion::V50;

namespace {

// Captured from us.hardfought.org, playing "v) NetHack 5.0.0-hdf".
constexpr const char* kHardfought50 =
    "\x1b[H\x1b[2J\x1b[H\x1b[5;1HNetHack, Copyright 1985-2026\r"
    "\x1b[6;1H         By Stichting Mathematisch Centrum and M. Stephenson.\r"
    "\x1b[7;1H         Version 5.0.0-0 Unix post-release, built Jul 10 2026 22:34:55.\r"
    "\x1b[8;1H         See license for details.\r\x1b[9;1H\x1b[12;1H";

// The same line as 3.6.7 prints it. One `Sprintf` in `util/makedefs.c`
// builds it for both releases.
constexpr const char* kNao367 =
    "\x1b[7;1H         Version 3.6.7 Unix post-release, built Feb 10 2023 19:15:47.\r";

using Found = std::optional<ServerVersion>;

}  // namespace

TEST_CASE("the version hardfought prints is read as 5.0") {
  CHECK(VersionWatch{}.observe(kHardfought50) == Found(ServerVersion{"5.0.0-0", V50}));
}

TEST_CASE("the version a 3.6.7 server prints is read as 3.6") {
  CHECK(VersionWatch{}.observe(kNao367) == Found(ServerVersion{"3.6.7", V36}));
}

TEST_CASE("three seven uses the five zero tile ordering") {
  // 3.7 is where the ordering changed; 5.0 is the same lineage.
  const auto found = VersionWatch{}.observe("         Version 3.7.0 Unix, built x.");
  REQUIRE(found);
  CHECK(found->version == V50);
}

TEST_CASE("a release with no bundled sheet is named but not guessed") {
  // Hardfought still serves 3.4.3 and 1.3d. A guessed sheet for those would
  // draw the wrong-picture mess this module exists to catch.
  const auto found = VersionWatch{}.observe(
      "         Version 3.4.3 Unix post-release, built Jan 1 2020 00:00:00.");
  CHECK(found == Found(ServerVersion{"3.4.3", std::nullopt}));
}

TEST_CASE("a banner split across two reads is still read") {
  // The banner arrives in whatever pieces the network hands over.
  VersionWatch watch;
  CHECK(watch.observe("\x1b[7;1H         Vers") == Found());
  CHECK(watch.observe("ion 5.0.0-0 Unix post-release, built Jul 10 2026 22:34:55.\r") ==
        Found(ServerVersion{"5.0.0-0", V50}));
}

TEST_CASE("the version is reported once not on every read") {
  VersionWatch watch;
  CHECK(watch.observe(kHardfought50).has_value());
  CHECK(watch.observe(kHardfought50) == Found());
}

TEST_CASE("ordinary game text is not mistaken for a banner") {
  VersionWatch watch;
  CHECK(watch.observe("You see here a version of a scroll.") == Found());
  CHECK(watch.observe("Velkommen username, the dwarven Valkyrie!") == Found());
}

TEST_CASE("the buffer does not grow without bound") {
  // A session is megabytes of map redraws. Only the tail can be kept.
  VersionWatch watch;
  for (int i = 0; i < 500; ++i) watch.observe(std::string(1000, 'x'));
  CHECK(watch.buffered() < 4096);
}

// Not in the Rust suite. A token that ends where the read ends may be cut
// short, so it must wait for the next read.
TEST_CASE("a version at the very end of a read waits for the rest") {
  VersionWatch watch;
  CHECK(watch.observe("         Version 5.0") == Found());
  CHECK(watch.observe(".0-0 Unix") == Found(ServerVersion{"5.0.0-0", V50}));
}
