#pragma once

// Parser for NetHack's text tile sources (`win/share/*.txt`).
//
// Port of src-tauri/src/tilesrc.rs. Those files define the vanilla ("Amiga")
// 16x16 tileset. Each starts with a colour map and then a run of tile blocks:
//
//   . = (71, 108, 108)
//   A = (0, 0, 0)
//   # tile 0 (giant ant)
//   {
//     ................
//     .......JAJKKA...
//     ...
//   }
//
// Tile numbers are positional. `monsters.txt`, `objects.txt` and
// `other.txt` joined in that order give the order the server compiles into
// `glyph2tile`, which is what `AVTC_GLYPH_START` indexes.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nh {

using Rgba = std::array<std::uint8_t, 4>;

// One parsed tile: RGBA pixels in row-major order.
struct TileImage {
  // The name from the `# tile N (name)` header.
  std::string name;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // `width * height` pixels.
  std::vector<Rgba> pixels;

  Rgba pixel(std::uint32_t x, std::uint32_t y) const { return pixels[y * width + x]; }

  bool operator==(const TileImage&) const = default;
};

class TileSourceError : public std::runtime_error {
 public:
  enum class Kind { BadColor, UnknownColor, RaggedRow, WrongHeight, Unexpected, Unterminated, Empty };

  TileSourceError(Kind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

// Parses one tile source file. Throws TileSourceError.
//
// The first block sets the tile size. Every later block must match it, which
// catches a truncated or badly edited source file.
std::vector<TileImage> parse_tile_file(std::string_view text);

// Builds the statue block: one grey copy of every monster tile.
//
// NetHack 3.6 is compiled with `STATUES_LOOK_LIKE_MONSTERS`, so a statue is
// not the monster's own tile and not one generic statue object. `tilemap.c`
// walks the monster list a second time and numbers a separate block of grey
// tiles after everything in the three tile files. A sheet that stops at
// `other.txt` has no tile at any index a statue names.
//
// The weights are the usual luma coefficients, so a bright monster makes a
// bright statue and the shape stays readable.
std::vector<TileImage> statue_tiles(std::span<const TileImage> monsters);

// An RGBA image, 4 bytes per pixel, row-major.
struct RgbaImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> bytes;

  Rgba pixel(std::uint32_t x, std::uint32_t y) const;
};

// Lays tiles out row-major in one sheet. Throws TileSourceError.
//
// The layout must match `Tileset::tile_rect`: tile `n` is at column
// `n % columns`, row `n / columns`. Cells after the last tile stay fully
// transparent.
RgbaImage compose_sheet(std::span<const TileImage> tiles, std::uint32_t columns);

// The image as PNG file bytes.
std::string encode_png(const RgbaImage& image);

// Parses `# tile 12 (giant ant)` into its name.
std::optional<std::string> tile_header_name(std::string_view line);

// Parses `A = (0, 182, 255)` into its character and colour.
std::optional<std::pair<char, Rgba>> color_definition(std::string_view line);

}  // namespace nh
