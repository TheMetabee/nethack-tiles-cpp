#pragma once

// An optional diagnostic log of the tile numbers that arrive.
//
// Port of src-tauri/src/debuglog.rs. Environment variables turn it on, so it
// works for one session without a rebuild:
//
//   NHTILES_LOG=/tmp/tiles.log    decoded glyphs and a summary
//   NHTILES_RAW=/tmp/tiles.raw    raw bytes from the server, for replay
//
// The useful part is the pair: each tile number goes next to the character
// NetHack drew for it. `tile=341 ch="@"` says the server thinks tile 341 is
// the hero. That is enough to find an offset between the server's tile order
// and the bundled sheet without a guess.

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "core/demux.hpp"

namespace nh {

// Collects glyph observations and writes them to `out`.
class TileDebugLog {
 public:
  TileDebugLog(std::ostream& out, std::uint32_t tile_count) : out_(out), tile_count_(tile_count) {}

  // Records a batch of demultiplexed items.
  void observe(const std::vector<StreamItem>& items);

  // Writes the picture of the whole session.
  void summarize();

 private:
  struct Pending {
    std::uint32_t tile;
    std::uint32_t flags;
    std::string text;
  };

  void close_glyph();

  std::ostream& out_;
  // Number of tiles in the sheet in use, for the out-of-range count.
  std::uint32_t tile_count_;
  // The glyph that is open, with the characters seen so far.
  std::optional<Pending> pending_;
  std::uint64_t total_ = 0;
  std::map<std::uint32_t, std::uint64_t> distinct_;
  std::map<std::uint32_t, std::uint64_t> out_of_range_;
  // Tile number to the characters it was drawn as.
  std::map<std::uint32_t, std::string> samples_;
};

// Opens the file that the environment variable `var` names, if it is set.
std::unique_ptr<std::ofstream> file_from_env(const char* var);

}  // namespace nh
