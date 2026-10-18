// Port of the tests in src-tauri/src/debuglog.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <sstream>
#include <vector>

#include "core/debuglog.hpp"

using namespace nh;
using Items = std::vector<StreamItem>;

namespace {

StreamItem text(std::string s) { return Text{std::move(s), true}; }
StreamItem glyph(std::uint32_t tile, std::uint32_t flags) { return Event{GlyphStart{tile, flags}}; }
const StreamItem kEnd = Event{GlyphEnd{}};

std::string log_to_string(const Items& items, std::uint32_t tile_count, bool summary) {
  std::ostringstream out;
  TileDebugLog log(out, tile_count);
  log.observe(items);
  if (summary) log.summarize();
  return out.str();
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a glyph is logged with the character it was drawn as") {
  const auto out = log_to_string({glyph(341, 0), text("@"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, "tile=341"));
  CHECK(has(out, R"(ch="@")"));
}

TEST_CASE("colour codes around the character are not logged as the character") {
  const auto out = log_to_string({glyph(12, 0), text("\x1b[1;31md\x1b[0m"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, R"(ch="d")"));
}

TEST_CASE("flags are logged in hex") {
  const auto out = log_to_string({glyph(7, 0x10), text("d"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, "flags=0x0010"));
}

TEST_CASE("text outside a glyph is not attributed to one") {
  const auto out =
      log_to_string({text("message line"), glyph(5, 0), text("<"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, R"(ch="<")"));
  CHECK_FALSE(has(out, "message"));
}

TEST_CASE("an index past the sheet is flagged") {
  const auto out = log_to_string({glyph(9000, 0), text("?"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, "OUT-OF-RANGE"));
}

TEST_CASE("an index inside the sheet is not flagged") {
  const auto out = log_to_string({glyph(100, 0), text("d"), kEnd}, 1515, false);
  INFO(out);
  CHECK_FALSE(has(out, "OUT-OF-RANGE"));
}

TEST_CASE("a glyph left open by the next glyph is still recorded") {
  // NetHack always closes glyphs, but a dropped byte must not swallow every
  // later observation.
  const auto out =
      log_to_string({glyph(1, 0), text("a"), glyph(2, 0), text("b"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, "tile=1 "));
  CHECK(has(out, "tile=2 "));
}

TEST_CASE("the summary reports the index range and out of range count") {
  const auto out = log_to_string({glyph(10, 0), text("a"), kEnd, glyph(4000, 0), text("b"), kEnd,
                                  glyph(4000, 0), text("b"), kEnd},
                                 1515, true);
  INFO(out);
  CHECK(has(out, "glyphs=3"));
  CHECK(has(out, "distinct=2"));
  CHECK(has(out, "min=Some(10)"));
  CHECK(has(out, "max=Some(4000)"));
  CHECK(has(out, "out_of_range=2"));
}

TEST_CASE("the summary lists the most frequent indices with a sample character") {
  Items items;
  for (int i = 0; i < 5; ++i) items.insert(items.end(), {glyph(2000, 0), text("?"), kEnd});
  items.insert(items.end(), {glyph(3, 0), text("@"), kEnd});
  const auto out = log_to_string(items, 1515, true);
  INFO(out);
  CHECK(has(out, "tile=2000"));
  CHECK(has(out, "seen=5"));
  CHECK(has(out, R"(ch="?")"));
}

TEST_CASE("frame boundaries are marked") {
  const auto out = log_to_string({glyph(1, 0), text("a"), kEnd, Event{FrameSync{}}}, 1515, false);
  INFO(out);
  CHECK(has(out, "--- frame ---"));
}

// Not in the Rust suite: the exact line shape, which a reader of old logs
// from the Rust app expects.
TEST_CASE("a glyph line has the same layout as the rust log") {
  const auto out = log_to_string({glyph(341, 0x10), text("@"), kEnd}, 1515, false);
  CHECK(out == "tile=341   flags=0x0010 ch=\"@\"\n");
}

TEST_CASE("an empty summary says none for the range") {
  const auto out = log_to_string({}, 1515, true);
  INFO(out);
  CHECK(has(out, "glyphs=0 distinct=0 min=None max=None sheet_tile_count=1515"));
}

TEST_CASE("a cp437 byte in a glyph is logged as its latin-1 character") {
  // The Rust log reads each byte as a char, so 0xB0 comes out as U+00B0.
  const auto out = log_to_string({glyph(1, 0), text("\xb0"), kEnd}, 1515, false);
  INFO(out);
  CHECK(has(out, "ch=\"\xc2\xb0\""));
}
