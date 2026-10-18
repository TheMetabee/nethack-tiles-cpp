#pragma once

// Tile sheet loading and the map from a tile index to a rectangle.
//
// Port of src-tauri/src/tileset.rs. A tileset is a PNG grid plus a manifest
// that gives its geometry. The tile index in `AVTC_GLYPH_START` is
// `glyph2tile[glyph]`, a direct index into this grid in row-major order.
//
// The order is compiled into the server binary and changes between NetHack
// releases, so each tileset records the version it was built for.

#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/glyph.hpp"

namespace nh {

// Geometry and origin of a tile sheet. The JSON form is the camelCase
// manifest the frontend reads.
struct TilesetManifest {
  // Stable identifier that profiles use, e.g. `vanilla-3.6.7-16`.
  std::string id;
  // The name in the picker.
  std::string name;
  // The NetHack release this sheet's tile order comes from.
  NetHackVersion version = NetHackVersion::V36;
  std::uint32_t tile_width = 0;
  std::uint32_t tile_height = 0;
  // Tiles per row in the sheet.
  std::uint32_t columns = 0;
  // Number of tiles that carry an image. The last row can be partly filled.
  std::uint32_t tile_count = 0;

  bool operator==(const TilesetManifest&) const = default;
};

void to_json(nlohmann::json& j, NetHackVersion version);
void from_json(const nlohmann::json& j, NetHackVersion& version);
void to_json(nlohmann::json& j, const TilesetManifest& m);
void from_json(const nlohmann::json& j, TilesetManifest& m);

// Where a tile is in the sheet, in pixels.
struct TileRect {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  bool operator==(const TileRect&) const = default;
};

std::ostream& operator<<(std::ostream& os, const TileRect& r);

class TilesetError : public std::runtime_error {
 public:
  enum class Kind { Decode, DegenerateGeometry, DimensionMismatch };

  TilesetError(Kind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

// Number of rows that hold `tile_count` tiles at `columns` per row.
std::uint32_t rows_for(std::uint32_t tile_count, std::uint32_t columns);

// A loaded tile sheet that matches its manifest.
class Tileset {
 public:
  // Checks `png` against `manifest`. Throws TilesetError if they disagree.
  static Tileset load(TilesetManifest manifest, std::string png);

  const TilesetManifest& manifest() const { return manifest_; }

  // Number of tile rows in the sheet.
  std::uint32_t rows() const { return rows_; }

  // The rectangle for `index`, or none if the sheet has no such tile.
  std::optional<TileRect> tile_rect(std::uint32_t index) const;

  // The sheet as a `data:` URL that the webview can use as an image source.
  std::string data_url() const;

 private:
  Tileset(TilesetManifest manifest, std::string png, std::uint32_t rows)
      : manifest_(std::move(manifest)), png_(std::move(png)), rows_(rows) {}

  TilesetManifest manifest_;
  std::string png_;
  std::uint32_t rows_;
};

std::string base64_encode(std::string_view bytes);

}  // namespace nh
