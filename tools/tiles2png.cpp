// Builds a tile sheet PNG and its manifest from NetHack's `win/share/*.txt`
// files. Port of src-tauri/examples/tiles2png.rs.
//
//   tiles2png --id vanilla-3.6.7-16 --name "Vanilla 16x16 (NetHack 3.6.7)"
//             --version v36 --columns 40 --out-dir tiles
//             monsters.txt objects.txt other.txt
//
// List the inputs in the order NetHack's `tilemap.c` walks them: monsters,
// objects, other. Tile numbers are positional, and that order is what the
// server compiles into `glyph2tile`.
//
// This is a developer tool. It must stay out of the installed app.

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tileset.hpp"
#include "core/tilesrc.hpp"

namespace fs = std::filesystem;

namespace {

struct Args {
  std::string id;
  std::string name;
  nh::NetHackVersion version;
  std::uint32_t columns;
  fs::path out_dir;
  std::vector<fs::path> inputs;
};

constexpr const char* kUsage =
    "usage: tiles2png --id ID --name NAME --version v36|v50 [--columns N] "
    "--out-dir DIR <monsters.txt> <objects.txt> <other.txt>";

Args parse_args(int argc, char** argv) {
  std::optional<std::string> id, name;
  std::optional<nh::NetHackVersion> version;
  std::uint32_t columns = 40;
  std::optional<fs::path> out_dir;
  std::vector<fs::path> inputs;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
      return argv[++i];
    };
    if (arg == "--id") {
      id = value();
    } else if (arg == "--name") {
      name = value();
    } else if (arg == "--out-dir") {
      out_dir = fs::path(value());
    } else if (arg == "--columns") {
      const std::string text = value();
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), columns);
      if (error != std::errc() || end != text.data() + text.size() || columns == 0) {
        throw std::runtime_error("--columns must be a positive integer");
      }
    } else if (arg == "--version") {
      const std::string text = value();
      if (text == "v36" || text == "3.6") {
        version = nh::NetHackVersion::V36;
      } else if (text == "v50" || text == "5.0" || text == "3.7") {
        version = nh::NetHackVersion::V50;
      } else {
        throw std::runtime_error("unknown --version \"" + text + "\"");
      }
    } else if (arg == "-h" || arg == "--help") {
      throw std::runtime_error(kUsage);
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("unknown flag " + arg);
    } else {
      inputs.emplace_back(arg);
    }
  }

  if (inputs.empty()) throw std::runtime_error("at least one tile source file is required");
  if (!id) throw std::runtime_error("--id is required");
  if (!name) throw std::runtime_error("--name is required");
  if (!version) throw std::runtime_error("--version is required");
  if (!out_dir) throw std::runtime_error("--out-dir is required");
  return {*id, *name, *version, columns, *out_dir, std::move(inputs)};
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("reading " + path.string() + ": cannot open the file");
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_file(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  out.close();
  if (!out) throw std::runtime_error("writing " + path.string() + ": the write failed");
}

void run(int argc, char** argv) {
  const Args args = parse_args(argc, argv);

  std::vector<nh::TileImage> tiles;
  std::size_t monster_count = 0;
  for (std::size_t i = 0; i < args.inputs.size(); ++i) {
    const auto& path = args.inputs[i];
    std::vector<nh::TileImage> parsed;
    try {
      parsed = nh::parse_tile_file(read_file(path));
    } catch (const nh::TileSourceError& e) {
      throw std::runtime_error("parsing " + path.string() + ": " + e.what());
    }
    std::cout << path.string() << ": " << parsed.size() << " tiles\n";
    // The first file is monsters.txt, by the argument order this tool
    // documents, and the statue block is a second pass over it.
    if (i == 0) monster_count = parsed.size();
    tiles.insert(tiles.end(), parsed.begin(), parsed.end());
  }

  // Statues are numbered after every tile in the three files, so they must
  // be made here, or a statue names an index past the end of the sheet. See
  // `statue_tiles`.
  const auto statues = nh::statue_tiles(std::span(tiles).first(monster_count));
  std::cout << "statues: " << statues.size() << " tiles\n";
  tiles.insert(tiles.end(), statues.begin(), statues.end());

  const auto sheet = nh::compose_sheet(tiles, args.columns);
  const nh::TilesetManifest manifest{args.id,          args.name,
                                     args.version,     tiles[0].width,
                                     tiles[0].height,  args.columns,
                                     static_cast<std::uint32_t>(tiles.size())};

  std::error_code error;
  fs::create_directories(args.out_dir, error);
  if (error) {
    throw std::runtime_error("creating " + args.out_dir.string() + ": " + error.message());
  }
  const fs::path png_path = args.out_dir / (manifest.id + ".png");
  const fs::path manifest_path = args.out_dir / (manifest.id + ".json");

  write_file(png_path, nh::encode_png(sheet));
  // Keys in field order, as the Rust tool wrote them, so a rebuilt manifest
  // has no diff against the one in assets/.
  const nlohmann::ordered_json ordered = {
      {"id", manifest.id},
      {"name", manifest.name},
      {"version", nlohmann::json(manifest.version)},
      {"tileWidth", manifest.tile_width},
      {"tileHeight", manifest.tile_height},
      {"columns", manifest.columns},
      {"tileCount", manifest.tile_count},
  };
  write_file(manifest_path, ordered.dump(2));

  std::cout << "wrote " << png_path.string() << " (" << sheet.width << "x" << sheet.height << ", "
            << manifest.tile_count << " tiles, " << manifest.columns << " columns)\n"
            << "wrote " << manifest_path.string() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    run(argc, argv);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "tiles2png: " << e.what() << "\n";
    return 1;
  }
}
