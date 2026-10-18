// Port of the tests in src-tauri/src/tileset.rs. Same cases, same names,
// plus the manifest JSON the frontend reads.

#include <doctest/doctest.h>

#include <stb_image_write.h>

#include <string>
#include <vector>

#include "core/tileset.hpp"

using namespace nh;
using Rect = std::optional<TileRect>;

namespace {

// Builds a real PNG of the given size, so the tests exercise real decoding.
std::string png_bytes(std::uint32_t width, std::uint32_t height) {
  std::vector<unsigned char> pixels(std::size_t{width} * height * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = 1, pixels[i + 1] = 2, pixels[i + 2] = 3, pixels[i + 3] = 255;
  }
  std::string out;
  stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
      },
      &out, static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
      static_cast<int>(width * 4));
  REQUIRE_FALSE(out.empty());
  return out;
}

// 40 columns of 16px and 1057 tiles give 27 rows, the last one partly empty.
TilesetManifest manifest() {
  return {"test-16", "Test 16x16", NetHackVersion::V36, 16, 16, 40, 1057};
}

Tileset loaded() {
  const auto m = manifest();
  const auto rows = rows_for(m.tile_count, m.columns);
  return Tileset::load(m, png_bytes(m.columns * m.tile_width, rows * m.tile_height));
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> origin(const Rect& r) {
  if (!r) return std::nullopt;
  return std::pair{r->x, r->y};
}

using Origin = std::optional<std::pair<std::uint32_t, std::uint32_t>>;

TilesetError::Kind load_error(TilesetManifest m, std::string png) {
  try {
    Tileset::load(std::move(m), std::move(png));
  } catch (const TilesetError& e) {
    return e.kind();
  }
  FAIL("the sheet loaded");
  return {};
}

}  // namespace

TEST_CASE("rows are rounded up for a partial last row") {
  CHECK(rows_for(1057, 40) == 27);
  CHECK(rows_for(80, 40) == 2);
  CHECK(rows_for(1, 40) == 1);
  CHECK(rows_for(0, 40) == 0);
}

TEST_CASE("first tile is at the origin") {
  CHECK(loaded().tile_rect(0) == Rect(TileRect{0, 0, 16, 16}));
}

TEST_CASE("tiles advance across a row") {
  CHECK(origin(loaded().tile_rect(3)) == Origin({48, 0}));
}

TEST_CASE("tile index wraps to the next row at the column count") {
  const auto ts = loaded();
  CHECK(origin(ts.tile_rect(40)) == Origin({0, 16}));
  CHECK(origin(ts.tile_rect(41)) == Origin({16, 16}));
  CHECK(origin(ts.tile_rect(85)) == Origin({5 * 16, 2 * 16}));
}

TEST_CASE("the last valid tile resolves") {
  CHECK(loaded().tile_rect(1056).has_value());
}

TEST_CASE("an index past the end has no rect") {
  const auto ts = loaded();
  CHECK(ts.tile_rect(1057) == Rect());
  CHECK(ts.tile_rect(99'999) == Rect());
}

TEST_CASE("rows are exposed") {
  CHECK(loaded().rows() == 27);
}

TEST_CASE("a sheet of the wrong size is rejected") {
  const auto m = manifest();
  CHECK(load_error(m, png_bytes(m.columns * m.tile_width, 10)) ==
        TilesetError::Kind::DimensionMismatch);
}

TEST_CASE("a sheet with extra trailing rows is accepted") {
  // Some generators pad the sheet out to a full rectangle.
  const auto m = manifest();
  const auto rows = rows_for(m.tile_count, m.columns);
  CHECK_NOTHROW(Tileset::load(m, png_bytes(m.columns * m.tile_width, (rows + 3) * m.tile_height)));
}

TEST_CASE("non png data is rejected") {
  CHECK(load_error(manifest(), "not a png") == TilesetError::Kind::Decode);
}

// Not in the Rust suite. stb_image reads other formats too, and a sheet must
// be a PNG.
TEST_CASE("an image that is not a png is rejected") {
  std::string bmp;
  const unsigned char pixel[3] = {1, 2, 3};
  stbi_write_bmp_to_func(
      [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
      },
      &bmp, 1, 1, 3, pixel);
  CHECK(load_error({"x", "x", NetHackVersion::V36, 1, 1, 1, 1}, bmp) ==
        TilesetError::Kind::Decode);
}

TEST_CASE("zero geometry is rejected") {
  auto m = manifest();
  m.tile_width = 0;
  CHECK(load_error(m, png_bytes(16, 16)) == TilesetError::Kind::DegenerateGeometry);
}

TEST_CASE("data url is a base64 png") {
  const std::string url = loaded().data_url();
  const std::string prefix = "data:image/png;base64,";
  CHECK(url.rfind(prefix, 0) == 0);
  CHECK(url.size() > prefix.size());
}

TEST_CASE("the manifest survives loading") {
  CHECK(loaded().manifest().id == "test-16");
}

TEST_CASE("base64 matches the standard alphabet and padding") {
  CHECK(base64_encode("") == "");
  CHECK(base64_encode("f") == "Zg==");
  CHECK(base64_encode("fo") == "Zm8=");
  CHECK(base64_encode("foo") == "Zm9v");
  CHECK(base64_encode("\xff\xfe\xfd") == "//79");
}

TEST_CASE("the manifest round-trips through the json the frontend reads") {
  const auto j = nlohmann::json::parse(R"({
    "id": "vanilla-5.0.0-16", "name": "Vanilla", "version": "v50",
    "tileWidth": 16, "tileHeight": 32, "columns": 40, "tileCount": 2304
  })");
  const auto m = j.get<TilesetManifest>();
  CHECK(m == TilesetManifest{"vanilla-5.0.0-16", "Vanilla", NetHackVersion::V50, 16, 32, 40, 2304});
  CHECK(nlohmann::json(m) == j);
}

TEST_CASE("an unknown version in a manifest is refused") {
  const auto j = nlohmann::json::parse(R"({
    "id": "x", "name": "x", "version": "v34",
    "tileWidth": 16, "tileHeight": 16, "columns": 40, "tileCount": 1
  })");
  CHECK_THROWS(j.get<TilesetManifest>());
}
