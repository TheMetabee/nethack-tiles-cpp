#include "core/tilesrc.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace nh {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

std::string_view trim_start(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  return s;
}

// Rust's `{:?}` for a string: the text in double quotes. Not called
// `quoted`, for the reason in debuglog.cpp.
std::string debug_quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

[[noreturn]] void fail(TileSourceError::Kind kind, const std::string& message) {
  throw TileSourceError(kind, message);
}

[[noreturn]] void ragged(std::size_t line, const std::string& name, std::size_t got,
                         std::size_t expected) {
  fail(TileSourceError::Kind::RaggedRow,
       "line " + std::to_string(line) + ": tile " + debug_quoted(name) + " has a row of " +
           std::to_string(got) + " pixels, expected " + std::to_string(expected));
}

[[noreturn]] void wrong_height(std::size_t line, const std::string& name, std::size_t got,
                               std::size_t expected) {
  fail(TileSourceError::Kind::WrongHeight,
       "line " + std::to_string(line) + ": tile " + debug_quoted(name) + " has " +
           std::to_string(got) + " rows, expected " + std::to_string(expected));
}

struct OpenTile {
  std::string name;
  std::vector<std::vector<Rgba>> rows;
};

}  // namespace

std::vector<TileImage> parse_tile_file(std::string_view text) {
  std::map<char, Rgba> colors;
  std::vector<TileImage> tiles;

  // Set by the first complete tile. Every later tile must agree.
  std::optional<std::pair<std::size_t, std::size_t>> dimensions;

  // The tile being read, if the parser is inside a `{ ... }` block.
  std::optional<OpenTile> open;
  std::optional<std::string> pending_name;

  std::size_t line = 0;
  for (std::size_t from = 0; from < text.size();) {
    const std::size_t end = std::min(text.find('\n', from), text.size());
    const std::string_view trimmed = trim(text.substr(from, end - from));
    from = end + 1;
    ++line;

    if (open) {
      if (trimmed == "}") {
        OpenTile tile = std::move(*open);
        open.reset();
        const std::size_t height = tile.rows.size();
        const std::size_t width = tile.rows.empty() ? 0 : tile.rows.front().size();
        if (!dimensions) {
          dimensions = {width, height};
        } else {
          if (height != dimensions->second) wrong_height(line, tile.name, height, dimensions->second);
          if (width != dimensions->first) ragged(line, tile.name, width, dimensions->first);
        }
        TileImage image{std::move(tile.name), static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height), {}};
        image.pixels.reserve(width * height);
        for (const auto& row : tile.rows) image.pixels.insert(image.pixels.end(), row.begin(), row.end());
        tiles.push_back(std::move(image));
        continue;
      }

      if (trimmed.empty()) continue;

      std::vector<Rgba> row;
      row.reserve(trimmed.size());
      for (const char ch : trimmed) {
        const auto color = colors.find(ch);
        if (color == colors.end()) {
          fail(TileSourceError::Kind::UnknownColor, "line " + std::to_string(line) +
                                                        ": character '" + std::string(1, ch) +
                                                        "' is not in the colour map");
        }
        row.push_back(color->second);
      }
      // Compare with the first row of this tile, then with the tile width
      // once one is set.
      std::optional<std::size_t> expected;
      if (!open->rows.empty()) {
        expected = open->rows.front().size();
      } else if (dimensions) {
        expected = dimensions->first;
      }
      if (expected && row.size() != *expected) ragged(line, open->name, row.size(), *expected);
      open->rows.push_back(std::move(row));
      continue;
    }

    if (trimmed.empty()) continue;

    if (trimmed == "{") {
      open = OpenTile{pending_name.value_or(""), {}};
      pending_name.reset();
      continue;
    }

    if (auto name = tile_header_name(trimmed)) {
      pending_name = std::move(name);
      continue;
    }

    if (trimmed.front() == '#') continue;  // an ordinary comment

    if (const auto definition = color_definition(trimmed)) {
      colors[definition->first] = definition->second;
      continue;
    }

    fail(TileSourceError::Kind::Unexpected, "line " + std::to_string(line) + ": unexpected " +
                                                debug_quoted(trimmed) + " outside a tile block");
  }

  if (open) {
    fail(TileSourceError::Kind::Unterminated, "tile " + debug_quoted(open->name) + " is never closed");
  }
  if (tiles.empty()) fail(TileSourceError::Kind::Empty, "no tiles found");
  return tiles;
}

std::vector<TileImage> statue_tiles(std::span<const TileImage> monsters) {
  std::vector<TileImage> statues;
  statues.reserve(monsters.size());
  for (const auto& monster : monsters) {
    TileImage statue{"statue of " + monster.name, monster.width, monster.height, {}};
    statue.pixels.reserve(monster.pixels.size());
    for (const auto& [r, g, b, a] : monster.pixels) {
      const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
      const auto grey = static_cast<std::uint8_t>(std::clamp(std::round(luma), 0.0f, 255.0f));
      // Alpha is copied, not made grey: the transparent pixels are the
      // silhouette.
      statue.pixels.push_back({grey, grey, grey, a});
    }
    statues.push_back(std::move(statue));
  }
  return statues;
}

Rgba RgbaImage::pixel(std::uint32_t x, std::uint32_t y) const {
  const std::size_t at = (std::size_t{y} * width + x) * 4;
  return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}

RgbaImage compose_sheet(std::span<const TileImage> tiles, std::uint32_t columns) {
  if (tiles.empty() || columns == 0) fail(TileSourceError::Kind::Empty, "no tiles found");
  const std::uint32_t tw = tiles.front().width, th = tiles.front().height;

  for (const auto& tile : tiles) {
    if (tile.width != tw) ragged(0, tile.name, tile.width, tw);
    if (tile.height != th) wrong_height(0, tile.name, tile.height, th);
  }

  const auto count = static_cast<std::uint32_t>(tiles.size());
  const std::uint32_t rows = count / columns + (count % columns != 0 ? 1 : 0);
  RgbaImage sheet{columns * tw, rows * th, {}};
  // Zero is fully transparent, which is what the unused cells must be.
  sheet.bytes.assign(std::size_t{sheet.width} * sheet.height * 4, 0);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto& tile = tiles[index];
    const std::uint32_t ox = index % columns * tw, oy = index / columns * th;
    for (std::uint32_t y = 0; y < th; ++y) {
      for (std::uint32_t x = 0; x < tw; ++x) {
        const Rgba p = tile.pixel(x, y);
        std::copy(p.begin(), p.end(),
                  sheet.bytes.begin() +
                      static_cast<std::ptrdiff_t>((std::size_t{oy + y} * sheet.width + ox + x) * 4));
      }
    }
  }
  return sheet;
}

std::string encode_png(const RgbaImage& image) {
  std::string out;
  const int ok = stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
      },
      &out, static_cast<int>(image.width), static_cast<int>(image.height), 4, image.bytes.data(),
      static_cast<int>(image.width * 4));
  if (!ok) throw std::runtime_error("the PNG encoder failed");
  return out;
}

std::optional<std::string> tile_header_name(std::string_view line) {
  line = trim(line);
  if (line.empty() || line.front() != '#') return std::nullopt;
  std::string_view rest = trim_start(line.substr(1));
  if (rest.substr(0, 4) != "tile") return std::nullopt;
  rest = trim_start(rest.substr(4));
  const std::size_t open = rest.find('(');
  const std::size_t close = rest.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
    return std::nullopt;
  }
  return std::string(rest.substr(open + 1, close - open - 1));
}

std::optional<std::pair<char, Rgba>> color_definition(std::string_view line) {
  const std::size_t equals = line.find('=');
  if (equals == std::string_view::npos) return std::nullopt;
  const std::string_view lhs = trim(line.substr(0, equals));
  if (lhs.size() != 1) return std::nullopt;

  std::string_view rhs = trim(line.substr(equals + 1));
  if (rhs.size() < 2 || rhs.front() != '(' || rhs.back() != ')') return std::nullopt;
  rhs = rhs.substr(1, rhs.size() - 2);

  Rgba color{0, 0, 0, 255};
  std::size_t channel = 0;
  for (std::size_t from = 0;; ++channel) {
    const std::size_t comma = rhs.find(',', from);
    const std::string_view part = trim(rhs.substr(from, comma - from));
    if (channel >= 3) return std::nullopt;
    unsigned value = 0;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    if (part.empty() || error != std::errc() || end != part.data() + part.size() || value > 255) {
      return std::nullopt;
    }
    color[channel] = static_cast<std::uint8_t>(value);
    if (comma == std::string_view::npos) break;
    from = comma + 1;
  }
  if (channel != 2) return std::nullopt;
  return std::pair{lhs.front(), color};
}

}  // namespace nh
