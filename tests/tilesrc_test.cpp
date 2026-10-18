// Port of the tests in src-tauri/src/tilesrc.rs. Same cases, same names.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/tileset.hpp"
#include "core/tilesrc.hpp"

using namespace nh;
using Kind = TileSourceError::Kind;

namespace {

constexpr const char* kTwoTiles =
    ". = (71, 108, 108)\n"
    "A = (0, 0, 0)\n"
    "B = (255, 0, 0)\n"
    "# tile 0 (giant ant)\n"
    "{\n"
    "  ....\n"
    "  .AA.\n"
    "  .BB.\n"
    "  ....\n"
    "}\n"
    "# tile 1 (killer bee)\n"
    "{\n"
    "  BBBB\n"
    "  BAAB\n"
    "  BAAB\n"
    "  BBBB\n"
    "}\n";

using Color = std::optional<std::pair<char, Rgba>>;
using Name = std::optional<std::string>;

Kind parse_error(std::string_view src) {
  try {
    parse_tile_file(src);
  } catch (const TileSourceError& e) {
    return e.kind();
  }
  FAIL("the source parsed");
  return {};
}

// A solid tile of one colour, for layout checks.
TileImage solid(std::string name, Rgba color) {
  return {std::move(name), 4, 4, std::vector<Rgba>(16, color)};
}

}  // namespace

TEST_CASE("parses a colour definition") {
  CHECK(color_definition("A = (0, 182, 255)") == Color({'A', {0, 182, 255, 255}}));
  CHECK(color_definition("U = (205,205,205)") == Color({'U', {205, 205, 205, 255}}));
  CHECK(color_definition(". = (71, 108, 108)") == Color({'.', {71, 108, 108, 255}}));
  CHECK(color_definition("not a colour") == Color());
  CHECK(color_definition("AB = (1, 2, 3)") == Color());
  CHECK(color_definition("A = (1, 2)") == Color());
}

// Not in the Rust suite: the limits of a channel value.
TEST_CASE("a colour channel outside 0 to 255 is not a colour") {
  CHECK(color_definition("A = (256, 0, 0)") == Color());
  CHECK(color_definition("A = (-1, 0, 0)") == Color());
  CHECK(color_definition("A = (1, 2, 3, 4)") == Color());
  CHECK(color_definition("A = (1, , 3)") == Color());
}

TEST_CASE("parses a tile header") {
  CHECK(tile_header_name("# tile 0 (giant ant)") == Name("giant ant"));
  CHECK(tile_header_name("# tile 314 (lit corridor)") == Name("lit corridor"));
  CHECK(tile_header_name("# not a tile") == Name());
}

TEST_CASE("parses every tile in order") {
  const auto tiles = parse_tile_file(kTwoTiles);
  REQUIRE(tiles.size() == 2);
  CHECK(tiles[0].name == "giant ant");
  CHECK(tiles[1].name == "killer bee");
}

TEST_CASE("tile dimensions come from the source") {
  const auto tiles = parse_tile_file(kTwoTiles);
  CHECK(tiles[0].width == 4);
  CHECK(tiles[0].height == 4);
  CHECK(tiles[0].pixels.size() == 16);
}

TEST_CASE("pixels take their colour from the colour map") {
  const auto ant = parse_tile_file(kTwoTiles)[0];
  CHECK(ant.pixel(0, 0) == Rgba{71, 108, 108, 255});  // '.'
  CHECK(ant.pixel(1, 1) == Rgba{0, 0, 0, 255});       // 'A'
  CHECK(ant.pixel(1, 2) == Rgba{255, 0, 0, 255});     // 'B'
}

TEST_CASE("pixels are stored row major") {
  const auto bee = parse_tile_file(kTwoTiles)[1];
  // Row 1 is B A A B, so (0,1) differs from (1,1).
  CHECK(bee.pixel(0, 1) == Rgba{255, 0, 0, 255});
  CHECK(bee.pixel(1, 1) == Rgba{0, 0, 0, 255});
}

TEST_CASE("an unknown colour character is rejected") {
  const char* src = ". = (1, 2, 3)\n# tile 0 (x)\n{\n  .?\n  ..\n}\n";
  CHECK(parse_error(src) == Kind::UnknownColor);
  try {
    parse_tile_file(src);
  } catch (const TileSourceError& e) {
    CHECK(std::string(e.what()) == "line 4: character '?' is not in the colour map");
  }
}

TEST_CASE("a ragged row is rejected") {
  // A short row must not be padded without a word.
  CHECK(parse_error(". = (1, 2, 3)\n# tile 0 (x)\n{\n  ....\n  ..\n  ....\n  ....\n}\n") ==
        Kind::RaggedRow);
}

TEST_CASE("a tile of the wrong height is rejected") {
  // The second tile has 3 rows where the first set 4.
  const std::string src =
      std::string(kTwoTiles) + "# tile 2 (short)\n{\n  ....\n  ....\n  ....\n}\n";
  CHECK(parse_error(src) == Kind::WrongHeight);
}

TEST_CASE("an unterminated tile is rejected") {
  CHECK(parse_error(". = (1, 2, 3)\n# tile 0 (x)\n{\n  ..\n") == Kind::Unterminated);
}

TEST_CASE("a file with no tiles is rejected") {
  CHECK(parse_error(". = (1, 2, 3)\n") == Kind::Empty);
}

// Not in the Rust suite, where `lines()` drops a `\r` before each `\n`. A
// checkout with CRLF endings must still parse.
TEST_CASE("crlf line endings parse like lf") {
  std::string crlf;
  for (const char c : std::string_view(kTwoTiles)) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  CHECK(parse_tile_file(crlf) == parse_tile_file(kTwoTiles));
}

// Not in the Rust suite: text outside a block that is not a colour, a
// header, or a comment.
TEST_CASE("stray text outside a tile is rejected") {
  CHECK(parse_error(". = (1, 2, 3)\nhello\n") == Kind::Unexpected);
}

TEST_CASE("statues are one grey copy of every monster") {
  std::vector<TileImage> monsters;
  for (int i = 0; i < 3; ++i) monsters.push_back(solid(std::to_string(i), {10, 200, 40, 255}));
  const auto statues = statue_tiles(monsters);
  CHECK(statues.size() == 3);  // the statue block matches the monster block
  CHECK(statues[0].pixels[0][0] == statues[0].pixels[0][1]);
  CHECK(statues[0].pixels[0][1] == statues[0].pixels[0][2]);
}

// Not in the Rust suite: the grey value itself, so a change of weights or
// rounding shows up.
TEST_CASE("a statue pixel is the rounded luma of its monster pixel") {
  // 0.299 * 10 + 0.587 * 200 + 0.114 * 40 = 124.95, which rounds to 125.
  const auto statues = statue_tiles(std::vector{solid("x", {10, 200, 40, 255})});
  CHECK(statues[0].pixels[0] == Rgba{125, 125, 125, 255});
  // White stays white: the sum must not round past 255.
  CHECK(statue_tiles(std::vector{solid("w", {255, 255, 255, 255})})[0].pixels[0] ==
        Rgba{255, 255, 255, 255});
}

TEST_CASE("a statue keeps the shape of its monster") {
  // Transparent pixels must stay transparent, or every statue becomes a grey
  // square, not a grey monster.
  auto monster = solid("gnome", {10, 200, 40, 255});
  monster.pixels[0] = {0, 0, 0, 0};
  const auto statues = statue_tiles(std::span(&monster, 1));
  CHECK(statues[0].pixels[0][3] == 0);  // a hole stays a hole
  CHECK(statues[0].pixels[1][3] == 255);
}

TEST_CASE("a statue is named after its monster") {
  const auto statues = statue_tiles(std::vector{solid("gnome lord", {1, 2, 3, 255})});
  CHECK(statues[0].name == "statue of gnome lord");
}

TEST_CASE("the sheet is columns wide and tall enough for every tile") {
  std::vector<TileImage> tiles;
  for (int i = 0; i < 5; ++i) tiles.push_back(solid(std::to_string(i), {1, 2, 3, 255}));
  const auto sheet = compose_sheet(tiles, 3);
  // 5 tiles at 3 per row need 2 rows.
  CHECK(sheet.width == 3 * 4);
  CHECK(sheet.height == 2 * 4);
}

TEST_CASE("tiles are placed where tile rect says they are") {
  const Rgba red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255};
  const auto sheet =
      compose_sheet(std::vector{solid("r", red), solid("g", green), solid("b", blue)}, 2);
  // Index 0 is at (col 0, row 0), 1 at (col 1, row 0), 2 at (col 0, row 1).
  CHECK(sheet.pixel(1, 1) == red);
  CHECK(sheet.pixel(4 + 1, 1) == green);
  CHECK(sheet.pixel(1, 4 + 1) == blue);
}

TEST_CASE("unused cells in the last row are transparent") {
  const auto sheet = compose_sheet(std::vector{solid("a", {255, 0, 0, 255})}, 2);
  CHECK(sheet.pixel(4 + 1, 1) == Rgba{0, 0, 0, 0});
}

TEST_CASE("composing no tiles is an error") {
  try {
    compose_sheet({}, 4);
    FAIL("an empty sheet was composed");
  } catch (const TileSourceError& e) {
    CHECK(e.kind() == Kind::Empty);
  }
}

// Not in the Rust suite: a tile of another size in the list.
TEST_CASE("composing tiles of two sizes is an error") {
  auto wide = solid("wide", {1, 2, 3, 255});
  wide.width = 8;
  wide.pixels.resize(32);
  CHECK_THROWS_AS(compose_sheet(std::vector{solid("a", {1, 2, 3, 255}), wide}, 2),
                  TileSourceError);
}

TEST_CASE("a sheet composed from a parsed file round trips through the tileset loader") {
  const auto tiles = parse_tile_file(kTwoTiles);
  const std::uint32_t columns = 2;
  const auto png = encode_png(compose_sheet(tiles, columns));

  const TilesetManifest manifest{"generated",    "Generated", NetHackVersion::V36,
                                 tiles[0].width, tiles[0].height, columns,
                                 static_cast<std::uint32_t>(tiles.size())};
  // A sheet just composed must satisfy the loader.
  const auto loaded = Tileset::load(manifest, png);
  CHECK(loaded.tile_rect(1).has_value());
  CHECK_FALSE(loaded.tile_rect(2).has_value());
}

TEST_CASE("blank lines and comments between tiles are tolerated") {
  const char* src =
      ". = (1, 2, 3)\n"
      "\n"
      "# a stray comment\n"
      "\n"
      "# tile 0 (x)\n"
      "{\n"
      "  ..\n"
      "  ..\n"
      "}\n"
      "\n";
  CHECK(parse_tile_file(src).size() == 1);
}
