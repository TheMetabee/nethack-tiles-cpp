// Port of the tests in src-tauri/src/demux.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <sstream>

#include "core/demux.hpp"

namespace nh {
// Lets doctest print a whole stream when a comparison fails.
std::ostream& operator<<(std::ostream& os, const std::vector<StreamItem>& items) {
  os << "[";
  for (std::size_t i = 0; i < items.size(); ++i) os << (i ? ", " : "") << items[i];
  return os << "]";
}
}  // namespace nh

using namespace nh;
using namespace std::string_literals;
using Items = std::vector<StreamItem>;

namespace {

Items demux(std::string_view input) { return Demuxer{}.feed(input); }
StreamItem text(std::string s) { return Text{std::move(s), true}; }
StreamItem ctrl(std::string s) { return Text{std::move(s), false}; }
StreamItem event(TileEvent e) { return Event{e}; }

// Splitting a chunk can split a text run into two items. That is correct, so
// compare streams with adjacent runs of the same kind merged.
Items coalesce(const Items& items) {
  Items out;
  for (const auto& item : items) {
    if (!out.empty()) {
      auto* prev = std::get_if<Text>(&out.back());
      const auto* next = std::get_if<Text>(&item);
      if (prev && next && prev->prints == next->prints) {
        prev->bytes += next->bytes;
        continue;
      }
    }
    out.push_back(item);
  }
  return out;
}

std::size_t text_bytes(const Items& items, bool printing_only) {
  std::size_t n = 0;
  for (const auto& item : items) {
    if (const auto* t = std::get_if<Text>(&item); t && (!printing_only || t->prints)) {
      n += t->bytes.size();
    }
  }
  return n;
}

}  // namespace

TEST_CASE("plain text passes through unchanged") {
  CHECK(demux("Hello NetHack") == Items{text("Hello NetHack")});
}

TEST_CASE("empty input produces no items") { CHECK(demux("") == Items{}); }

TEST_CASE("glyph start carries tile index and flags") {
  CHECK(demux("\x1b[1;0;344;16z") == Items{event(GlyphStart{344, 16})});
}

TEST_CASE("glyph start without flags defaults to zero") {
  CHECK(demux("\x1b[1;0;12z") == Items{event(GlyphStart{12, 0})});
}

TEST_CASE("glyph end is decoded") { CHECK(demux("\x1b[1;1z") == Items{event(GlyphEnd{})}); }

TEST_CASE("select window carries the window id") {
  CHECK(demux("\x1b[1;2;3z") == Items{event(SelectWindow{3})});
}

TEST_CASE("select window may omit the window id") {
  // tty_nhgetch emits this form to force the next select to re-transmit.
  CHECK(demux("\x1b[1;2z") == Items{event(SelectWindow{std::nullopt})});
}

TEST_CASE("frame sync is decoded") { CHECK(demux("\x1b[1;3z") == Items{event(FrameSync{})}); }

TEST_CASE("sound cue is decoded") { CHECK(demux("\x1b[1;4;7z") == Items{event(Sound{7})}); }

TEST_CASE("tile codes are ordered with the surrounding text") {
  CHECK(demux("before\x1b[1;0;5;0z@\x1b[1;1zafter") ==
        Items{text("before"), event(GlyphStart{5, 0}), text("@"), event(GlyphEnd{}),
              text("after")});
}

TEST_CASE("printable text is split from the escape sequences around it") {
  CHECK(demux("abc\x1b[2Jdef") == Items{text("abc"), ctrl("\x1b[2J"), text("def")});
}

TEST_CASE("control codes are not printing text") {
  // A carriage return moves the cursor; it does not fill a cell.
  CHECK(demux("hi\r\nthere") == Items{text("hi"), ctrl("\r\n"), text("there")});
}

TEST_CASE("SGR sequence starting with param one is not a tile code") {
  // ESC[1;31m (bold red) shares the leading 1 with the tile protocol.
  CHECK(demux("\x1b[1;31mred") == Items{ctrl("\x1b[1;31m"), text("red")});
}

TEST_CASE("ordinary CSI sequences pass through") {
  for (const char* seq : {"\x1b[2J", "\x1b[H", "\x1b[10;20H", "\x1b[?25l", "\x1b[m"}) {
    CAPTURE(seq);
    CHECK(demux(seq) == Items{ctrl(seq)});
  }
}

TEST_CASE("CSI ending in z without leading one passes through") {
  CHECK(demux("\x1b[2;0;5z") == Items{ctrl("\x1b[2;0;5z")});
}

TEST_CASE("unknown tile subcode is consumed, not leaked") {
  // No event is emitted, so the surrounding text stays one run.
  CHECK(demux("a\x1b[1;9;5zb") == Items{text("ab")});
}

TEST_CASE("glyph start without a tile index is ignored") { CHECK(demux("\x1b[1;0z") == Items{}); }

TEST_CASE("escape sequence split across chunks is reassembled") {
  Demuxer d;
  CHECK(d.feed("map\x1b[1;0;") == Items{text("map")});
  CHECK(d.feed("344;16") == Items{});
  CHECK(d.feed("z@") == Items{event(GlyphStart{344, 16}), text("@")});
}

TEST_CASE("sequence split at every possible offset yields the same items") {
  const std::string input = "a\x1b[1;2;3zb\x1b[1;0;7;4z@\x1b[1;1z\x1b[1;3zc";
  const Items whole = demux(input);
  for (std::size_t split = 1; split < input.size(); ++split) {
    CAPTURE(split);
    Demuxer d;
    Items items = d.feed(std::string_view(input).substr(0, split));
    const Items rest = d.feed(std::string_view(input).substr(split));
    items.insert(items.end(), rest.begin(), rest.end());
    CHECK(coalesce(items) == whole);
  }
}

TEST_CASE("a charset selection sequence is non-printing to its last byte") {
  // ESC ( B is three bytes. Ending at ESC ( would make the B look like a
  // character written to a cell.
  CHECK(demux("\x1b(Btext") == Items{ctrl("\x1b(B"), text("text")});
}

TEST_CASE("repeated escapes pass through") {
  CHECK(demux("\x1b\x1b(B") == Items{ctrl("\x1b\x1b(B")});
}

TEST_CASE("an operating system command is non-printing including its payload") {
  // dgamelaunch sets the window title this way on connect.
  CHECK(demux("\x1b]2;nethack.alt.org\x07menu") ==
        Items{ctrl("\x1b]2;nethack.alt.org\x07"), text("menu")});
}

TEST_CASE("an operating system command may end with a string terminator") {
  CHECK(demux("\x1b]0;title\x1b\\x") == Items{ctrl("\x1b]0;title\x1b\\"), text("x")});
}

TEST_CASE("an unterminated operating system command does not swallow the session") {
  const std::string input = "\x1b]0;" + std::string(kMaxOscLen + 20, 'a');
  CHECK(text_bytes(demux(input), true) > 0);
}

TEST_CASE("high bytes survive the round trip and count as printing") {
  // DEC/IBM graphics line drawing uses bytes above 0x7F; each fills a cell.
  CHECK(demux("\xc4\xb3\xda") == Items{text("\xc4\xb3\xda")});
}

TEST_CASE("runaway CSI is flushed as text rather than buffered forever") {
  const std::string input = "\x1b[" + std::string(kMaxCsiLen + 10, '1');
  CHECK(text_bytes(demux(input), false) == input.size());
}

TEST_CASE("saw_tile_data reports whether the server sent tile codes") {
  Demuxer d;
  d.feed("plain ascii \x1b[2J");
  CHECK_FALSE(d.saw_tile_data());
  d.feed("\x1b[1;3z");
  CHECK(d.saw_tile_data());
}

TEST_CASE("a realistic map row decodes to alternating glyphs and text") {
  // Cursor move, three glyphs, then the frame sync NetHack sends when it
  // starts waiting for a key.
  const std::string input =
      "\x1b[1;2;3z\x1b[3;1H"
      "\x1b[1;0;2378;0z-\x1b[1;1z"
      "\x1b[1;0;2379;0z|\x1b[1;1z"
      "\x1b[1;0;337;16z\x1b[1;31md\x1b[0m\x1b[1;1z"
      "\x1b[1;3z";
  CHECK(demux(input) == Items{
                            event(SelectWindow{3}),
                            ctrl("\x1b[3;1H"),
                            event(GlyphStart{2378, 0}),
                            text("-"),
                            event(GlyphEnd{}),
                            event(GlyphStart{2379, 0}),
                            text("|"),
                            event(GlyphEnd{}),
                            event(GlyphStart{337, 16}),
                            ctrl("\x1b[1;31m"),
                            text("d"),
                            ctrl("\x1b[0m"),
                            event(GlyphEnd{}),
                            event(FrameSync{}),
                        });
}
