#include "core/tileset.hpp"

#include <stb_image.h>

#include <memory>

namespace nh {

void to_json(nlohmann::json& j, NetHackVersion version) {
  j = version == NetHackVersion::V36 ? "v36" : "v50";
}

void from_json(const nlohmann::json& j, NetHackVersion& version) {
  const auto text = j.get<std::string>();
  if (text == "v36") {
    version = NetHackVersion::V36;
  } else if (text == "v50") {
    version = NetHackVersion::V50;
  } else {
    throw std::invalid_argument("unknown NetHack version \"" + text + "\"");
  }
}

void to_json(nlohmann::json& j, const TilesetManifest& m) {
  j = {{"id", m.id},
       {"name", m.name},
       {"version", m.version},
       {"tileWidth", m.tile_width},
       {"tileHeight", m.tile_height},
       {"columns", m.columns},
       {"tileCount", m.tile_count}};
}

void from_json(const nlohmann::json& j, TilesetManifest& m) {
  j.at("id").get_to(m.id);
  j.at("name").get_to(m.name);
  j.at("version").get_to(m.version);
  j.at("tileWidth").get_to(m.tile_width);
  j.at("tileHeight").get_to(m.tile_height);
  j.at("columns").get_to(m.columns);
  j.at("tileCount").get_to(m.tile_count);
}

std::ostream& operator<<(std::ostream& os, const TileRect& r) {
  return os << "TileRect{" << r.x << ", " << r.y << ", " << r.width << ", " << r.height << "}";
}

std::uint32_t rows_for(std::uint32_t tile_count, std::uint32_t columns) {
  return tile_count / columns + (tile_count % columns != 0 ? 1 : 0);
}

Tileset Tileset::load(TilesetManifest manifest, std::string png) {
  if (manifest.tile_width == 0 || manifest.tile_height == 0 || manifest.columns == 0) {
    throw TilesetError(TilesetError::Kind::DegenerateGeometry,
                       "manifest has a zero tile_width, tile_height or columns");
  }

  // stb_image is built for PNG only, but it would still take another format
  // if that changed. A sheet must be a PNG.
  constexpr std::string_view kSignature = "\x89PNG\r\n\x1a\n";
  if (png.compare(0, kSignature.size(), kSignature) != 0) {
    throw TilesetError(TilesetError::Kind::Decode, "tile sheet is not a readable PNG");
  }
  // Decode all of it, not only the header, so a truncated file is refused
  // here and not later in the webview.
  int width = 0, height = 0, channels = 0;
  const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()),
                            static_cast<int>(png.size()), &width, &height, &channels, 0),
      &stbi_image_free);
  if (!pixels) {
    throw TilesetError(TilesetError::Kind::Decode,
                       std::string("tile sheet is not a readable PNG: ") + stbi_failure_reason());
  }

  const auto actual_width = static_cast<std::uint32_t>(width);
  const auto actual_height = static_cast<std::uint32_t>(height);
  const auto rows = rows_for(manifest.tile_count, manifest.columns);
  const auto expected_width = manifest.columns * manifest.tile_width;
  const auto expected_height = rows * manifest.tile_height;

  // The width must match exactly, or every index past the first row is
  // wrong. Extra height is allowed: some generators pad the sheet.
  if (actual_width != expected_width || actual_height < expected_height) {
    throw TilesetError(
        TilesetError::Kind::DimensionMismatch,
        "tile sheet is " + std::to_string(actual_width) + "x" + std::to_string(actual_height) +
            " but the manifest describes " + std::to_string(expected_width) + "x" +
            std::to_string(expected_height) + " (" + std::to_string(manifest.columns) +
            " columns of " + std::to_string(manifest.tile_width) + "x" +
            std::to_string(manifest.tile_height) + ", " + std::to_string(rows) + " rows)");
  }

  return Tileset(std::move(manifest), std::move(png), rows);
}

// A none here means the sheet does not match the server's NetHack version.
// The caller draws a placeholder and warns the player.
std::optional<TileRect> Tileset::tile_rect(std::uint32_t index) const {
  if (index >= manifest_.tile_count) return std::nullopt;
  return TileRect{(index % manifest_.columns) * manifest_.tile_width,
                  (index / manifest_.columns) * manifest_.tile_height, manifest_.tile_width,
                  manifest_.tile_height};
}

std::string Tileset::data_url() const { return "data:image/png;base64," + base64_encode(png_); }

std::string base64_encode(std::string_view bytes) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto byte = [&](std::size_t i) -> std::uint32_t {
    return static_cast<unsigned char>(bytes[i]);
  };
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const auto n = byte(i) << 16 | byte(i + 1) << 8 | byte(i + 2);
    out += {kTable[n >> 18 & 63], kTable[n >> 12 & 63], kTable[n >> 6 & 63], kTable[n & 63]};
  }
  if (const std::size_t rest = bytes.size() - i; rest > 0) {
    const auto n = byte(i) << 16 | (rest > 1 ? byte(i + 1) << 8 : 0);
    out += kTable[n >> 18 & 63];
    out += kTable[n >> 12 & 63];
    out += rest > 1 ? kTable[n >> 6 & 63] : '=';
    out += '=';
  }
  return out;
}

}  // namespace nh
