#pragma once

// Demultiplexes a NetHack tty stream into ordinary terminal bytes and
// `vt_tiledata` tile events.
//
// Port of src-tauri/src/demux.rs. NetHack compiled with TTY_TILES_ESCCODES
// writes every tile code as `ESC [ 1 ; <sub> [ ; <c> [ ; <d> ] ] z`. Any other
// CSI sequence, including the common SGR `ESC [ 1 ; 31 m`, passes through.
//
// Items come out in the order NetHack produced them. NetHack moves the cursor
// before it emits GlyphStart, so the owner of the terminal state finds the
// target cell by reading the cursor once the preceding text is processed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nh {

// `ESC [ 1 ; 0 ; <tile> [ ; <flags> ] z`: the next character stands for `tile`.
// `flags` is NetHack's version-dependent MG_* bitmask.
struct GlyphStart {
  std::uint32_t tile = 0;
  std::uint32_t flags = 0;
  bool operator==(const GlyphStart&) const = default;
};

// `ESC [ 1 ; 1 z`: end of the glyph opened by GlyphStart.
struct GlyphEnd {
  bool operator==(const GlyphEnd&) const = default;
};

// `ESC [ 1 ; 2 [ ; <winid> ] z`: later output belongs to window slot `winid`.
// tty_nhgetch sends it without a parameter to force a re-select.
struct SelectWindow {
  std::optional<std::int64_t> winid;
  bool operator==(const SelectWindow&) const = default;
};

// `ESC [ 1 ; 3 z`: NetHack flushed a frame and waits for input.
struct FrameSync {
  bool operator==(const FrameSync&) const = default;
};

// `ESC [ 1 ; 4 ; <id> z`: NetHack 5.0 sound cue. Decoded so it never reaches
// the terminal.
struct Sound {
  std::optional<std::int64_t> id;
  bool operator==(const Sound&) const = default;
};

using TileEvent = std::variant<GlyphStart, GlyphEnd, SelectWindow, FrameSync, Sound>;

// Bytes for the terminal emulator, unchanged.
//
// `prints` is true when the bytes put characters in cells, false for escape
// sequences and control codes. One item never mixes the two kinds, so the
// characters of a printing item fill the `bytes.size()` cells that end at the
// cursor once the item is processed.
struct Text {
  std::string bytes;
  bool prints = false;
  bool operator==(const Text&) const = default;
};

struct Event {
  TileEvent event;
  bool operator==(const Event&) const = default;
};

using StreamItem = std::variant<Text, Event>;

// Longest CSI sequence buffered before it is flushed as text.
inline constexpr std::size_t kMaxCsiLen = 64;
// Longest OSC payload buffered before the terminator is taken as lost.
inline constexpr std::size_t kMaxOscLen = 512;

// Incremental demultiplexer. A sequence split across chunks is held and
// emitted once complete, so callers may feed chunks of any size.
class Demuxer {
 public:
  std::vector<StreamItem> feed(std::string_view bytes);

  // True once a tile escape code was decoded. Without one, the player's
  // .nethackrc is missing OPTIONS=vt_tiledata.
  bool saw_tile_data() const { return saw_tile_data_; }

 private:
  enum class State { Ground, Esc, Csi, EscIntermediate, Osc, OscEsc };

  void finish_csi(const std::string& csi, std::vector<StreamItem>& out);
  void push(std::string_view bytes, bool prints, std::vector<StreamItem>& out);
  void flush_text(std::vector<StreamItem>& out);

  State state_ = State::Ground;
  std::string csi_;
  std::string text_;
  bool text_prints_ = false;
  std::size_t osc_len_ = 0;
  bool saw_tile_data_ = false;
};

std::ostream& operator<<(std::ostream& os, const StreamItem& item);

}  // namespace nh
