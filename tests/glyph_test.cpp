// Port of the tests in src-tauri/src/glyph.rs. Same cases, same names.

#include <doctest/doctest.h>

#include "core/glyph.hpp"

using nh::GlyphFlags;
using nh::NetHackVersion::V36;
using nh::NetHackVersion::V50;

TEST_CASE("zero decodes to no flags") {
  CHECK(GlyphFlags::decode(0, V36) == GlyphFlags{});
  CHECK(GlyphFlags::decode(0, V50) == GlyphFlags{});
}

TEST_CASE("pet bit differs between versions") {
  // The bug this whole module exists to prevent: 0x08 is MG_PET on 3.6 but
  // MG_DETECT on 5.0.
  CHECK(GlyphFlags::decode(0x08, V36).pet);
  CHECK_FALSE(GlyphFlags::decode(0x08, V36).detected);

  CHECK(GlyphFlags::decode(0x08, V50).detected);
  CHECK_FALSE(GlyphFlags::decode(0x08, V50).pet);
  CHECK(GlyphFlags::decode(0x10, V50).pet);
}

TEST_CASE("decodes every v36 flag") {
  CHECK(GlyphFlags::decode(0x01, V36).corpse);
  CHECK(GlyphFlags::decode(0x02, V36).invisible);
  CHECK(GlyphFlags::decode(0x04, V36).detected);
  CHECK(GlyphFlags::decode(0x08, V36).pet);
  CHECK(GlyphFlags::decode(0x10, V36).ridden);
  CHECK(GlyphFlags::decode(0x20, V36).statue);
  CHECK(GlyphFlags::decode(0x40, V36).objpile);
  CHECK(GlyphFlags::decode(0x80, V36).bw_lava);
}

TEST_CASE("decodes every v50 flag") {
  CHECK(GlyphFlags::decode(0x0001, V50).hero);
  CHECK(GlyphFlags::decode(0x0002, V50).corpse);
  CHECK(GlyphFlags::decode(0x0004, V50).invisible);
  CHECK(GlyphFlags::decode(0x0008, V50).detected);
  CHECK(GlyphFlags::decode(0x0010, V50).pet);
  CHECK(GlyphFlags::decode(0x0020, V50).ridden);
  CHECK(GlyphFlags::decode(0x0040, V50).statue);
  CHECK(GlyphFlags::decode(0x0080, V50).objpile);
  CHECK(GlyphFlags::decode(0x0100, V50).bw_lava);
  CHECK(GlyphFlags::decode(0x2000, V50).female);
}

TEST_CASE("v50 reports the unexplored and nothing markers") {
  // 5.0 sends a real glyph for every cell the hero has not seen, and its tile
  // is a solid opaque black square. The overlay must know not to paint it, so
  // these two bits cannot be dropped.
  CHECK(GlyphFlags::decode(0x0800, V50).unexplored);
  CHECK(GlyphFlags::decode(0x0400, V50).nothing);
  CHECK_FALSE(GlyphFlags::decode(0x0800, V50).nothing);
  CHECK_FALSE(GlyphFlags::decode(0x0400, V50).unexplored);
}

TEST_CASE("v36 has no unexplored or nothing marker") {
  // 3.6 stops at MG_BW_LAVA. Those bit positions mean nothing there, so
  // decoding must not invent a marker that hides real terrain.
  const auto all_bits = GlyphFlags::decode(0xffff'ffff, V36);
  CHECK_FALSE(all_bits.unexplored);
  CHECK_FALSE(all_bits.nothing);
}

TEST_CASE("v36 never reports hero or female") {
  // Those bits mean other things on 3.6; decoding must not invent them.
  const auto all_bits = GlyphFlags::decode(0xffff'ffff, V36);
  CHECK_FALSE(all_bits.hero);
  CHECK_FALSE(all_bits.female);
}

TEST_CASE("combined flags decode together") {
  // A ridden female pet on 5.0: MG_PET | MG_RIDDEN | MG_FEMALE.
  const auto f = GlyphFlags::decode(0x0010 | 0x0020 | 0x2000, V50);
  CHECK((f.pet && f.ridden && f.female));
  CHECK_FALSE((f.corpse || f.statue || f.detected));
}

TEST_CASE("unknown high bits are ignored") {
  CHECK(GlyphFlags::decode(1u << 30, V36) == GlyphFlags{});
}
