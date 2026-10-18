#include "core/glyph.hpp"

namespace nh {

GlyphFlags GlyphFlags::decode(std::uint32_t raw, NetHackVersion version) {
  const auto has = [raw](std::uint32_t bit) { return (raw & bit) != 0; };
  GlyphFlags f;
  if (version == NetHackVersion::V36) {
    // 3.6 stops at MG_BW_LAVA. It has no hero, female, unexplored or nothing
    // marker, and it never draws an unexplored cell as a glyph.
    f.corpse = has(0x01);
    f.invisible = has(0x02);
    f.detected = has(0x04);
    f.pet = has(0x08);
    f.ridden = has(0x10);
    f.statue = has(0x20);
    f.objpile = has(0x40);
    f.bw_lava = has(0x80);
  } else {
    f.hero = has(0x0001);
    f.corpse = has(0x0002);
    f.invisible = has(0x0004);
    f.detected = has(0x0008);
    f.pet = has(0x0010);
    f.ridden = has(0x0020);
    f.statue = has(0x0040);
    f.objpile = has(0x0080);
    f.bw_lava = has(0x0100);
    f.nothing = has(0x0400);
    f.unexplored = has(0x0800);
    f.female = has(0x2000);
  }
  return f;
}

std::ostream& operator<<(std::ostream& os, NetHackVersion version) {
  return os << (version == NetHackVersion::V36 ? "V36" : "V50");
}

std::ostream& operator<<(std::ostream& os, const GlyphFlags& f) {
  os << "GlyphFlags{";
  const char* sep = "";
  const auto put = [&](bool on, const char* name) {
    if (on) {
      os << sep << name;
      sep = " ";
    }
  };
  put(f.hero, "hero");
  put(f.corpse, "corpse");
  put(f.invisible, "invisible");
  put(f.detected, "detected");
  put(f.pet, "pet");
  put(f.ridden, "ridden");
  put(f.statue, "statue");
  put(f.objpile, "objpile");
  put(f.bw_lava, "bw_lava");
  put(f.unexplored, "unexplored");
  put(f.nothing, "nothing");
  put(f.female, "female");
  return os << "}";
}

}  // namespace nh
