#pragma once

// Decodes the `MG_*` bitmask carried by `AVTC_GLYPH_START`.
//
// Port of src-tauri/src/glyph.rs. The bit values are version-dependent.
// NetHack 5.0 inserted `MG_HERO` at bit 0 and moved every later flag up one
// bit, so a mask decoded with the wrong version turns pets into detected
// monsters:
//
// | flag         | 3.6.x  | 5.0     |
// |--------------|--------|---------|
// | `MG_HERO`    | --     | `0x0001`|
// | `MG_CORPSE`  | `0x01` | `0x0002`|
// | `MG_INVIS`   | `0x02` | `0x0004`|
// | `MG_DETECT`  | `0x04` | `0x0008`|
// | `MG_PET`     | `0x08` | `0x0010`|
// | `MG_RIDDEN`  | `0x10` | `0x0020`|
// | `MG_STATUE`  | `0x20` | `0x0040`|
// | `MG_OBJPILE` | `0x40` | `0x0080`|
// | `MG_BW_LAVA` | `0x80` | `0x0100`|
// | `MG_NOTHING` | --     | `0x0400`|
// | `MG_UNEXPL`  | --     | `0x0800`|
// | `MG_FEMALE`  | --     | `0x2000`|
//
// Sources: `include/hack.h` (NetHack-3.6.7_Released) and `include/display.h`
// (master).

#include <cstdint>
#include <ostream>

namespace nh {

// Which NetHack release the server runs, for flag decoding.
enum class NetHackVersion {
  // NetHack 3.6.x (3.6.6, 3.6.7: what NAO and Hardfought serve today).
  V36,
  // NetHack 3.7 / 5.0.
  V50,
};

// The decoded per-glyph flags the overlay can act on.
struct GlyphFlags {
  // The glyph is the hero (5.0 only; always false on 3.6).
  bool hero = false;
  bool corpse = false;
  bool invisible = false;
  bool detected = false;
  // Draw the pet marker (a heart) over the tile.
  bool pet = false;
  bool ridden = false;
  bool statue = false;
  // More than one stack of objects on this square.
  bool objpile = false;
  // Lava that should be highlighted because it renders like water.
  bool bw_lava = false;
  // The hero has never seen this cell (5.0 only). Its tile is a solid black
  // square that would cover every unvisited part of the map, so the overlay
  // leaves these cells alone.
  bool unexplored = false;
  // The cell is known to hold nothing worth drawing (5.0 only). Same
  // treatment as `unexplored`.
  bool nothing = false;
  // The monster or statue is female (5.0 only; always false on 3.6).
  bool female = false;

  // Decodes `raw` with the bit layout of `version`.
  static GlyphFlags decode(std::uint32_t raw, NetHackVersion version);

  bool operator==(const GlyphFlags&) const = default;
};

std::ostream& operator<<(std::ostream& os, NetHackVersion version);
std::ostream& operator<<(std::ostream& os, const GlyphFlags& flags);

}  // namespace nh
