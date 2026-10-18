#include "core/debuglog.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numeric>

#include "core/autologin.hpp"

namespace nh {

namespace {

// Keeps the characters a terminal shows and drops escape sequences and
// control bytes, so the sample is the glyph itself. Each byte is read as a
// latin-1 character and written as UTF-8, as the Rust log did.
std::string printable(std::string_view bytes) {
  std::string out;
  for (const char c : strip_ansi(bytes)) {
    const auto b = static_cast<unsigned char>(c);
    if (b < 0x20 || (b >= 0x7f && b < 0xa0)) continue;
    if (b < 0x80) {
      out += c;
    } else {
      out += static_cast<char>(0xc0 | b >> 6);
      out += static_cast<char>(0x80 | (b & 0x3f));
    }
  }
  return out;
}

// The text in double quotes, as Rust's `{:?}` writes it. Not called
// `quoted`: with <iomanip> in view, as libc++ has it, a call on a
// std::string finds std::quoted by argument lookup and takes it.
std::string debug_quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string some_or_none(const std::map<std::uint32_t, std::uint64_t>& m, bool first) {
  if (m.empty()) return "None";
  return std::format("Some({})", first ? m.begin()->first : m.rbegin()->first);
}

}  // namespace

void TileDebugLog::observe(const std::vector<StreamItem>& items) {
  for (const auto& item : items) {
    if (const auto* text = std::get_if<Text>(&item)) {
      if (pending_) pending_->text += printable(text->bytes);
      continue;
    }
    const auto& event = std::get<Event>(item).event;
    if (const auto* start = std::get_if<GlyphStart>(&event)) {
      // A glyph that never closed: flush it, not lose it.
      close_glyph();
      pending_ = Pending{start->tile, start->flags, {}};
    } else if (std::holds_alternative<GlyphEnd>(event)) {
      close_glyph();
    } else if (std::holds_alternative<FrameSync>(event)) {
      close_glyph();
      out_ << "--- frame ---\n";
    }
  }
  out_.flush();
}

void TileDebugLog::close_glyph() {
  if (!pending_) return;
  const Pending glyph = std::move(*pending_);
  pending_.reset();

  const bool outside = glyph.tile >= tile_count_;
  ++total_;
  ++distinct_[glyph.tile];
  if (outside) ++out_of_range_[glyph.tile];
  if (!glyph.text.empty()) samples_.try_emplace(glyph.tile, glyph.text);

  out_ << std::format("tile={:<5} flags=0x{:04x} ch={}{}\n", glyph.tile, glyph.flags,
                      debug_quoted(glyph.text), outside ? " OUT-OF-RANGE" : "");
}

void TileDebugLog::summarize() {
  close_glyph();
  const auto out_of_range_total = std::accumulate(
      out_of_range_.begin(), out_of_range_.end(), std::uint64_t{0},
      [](std::uint64_t sum, const auto& entry) { return sum + entry.second; });

  out_ << "\n===== summary =====\n";
  out_ << std::format("glyphs={} distinct={} min={} max={} sheet_tile_count={}\n", total_,
                      distinct_.size(), some_or_none(distinct_, true),
                      some_or_none(distinct_, false), tile_count_);
  out_ << std::format("out_of_range={} ({} distinct indices)\n", out_of_range_total,
                      out_of_range_.size());

  out_ << "\n-- most frequent indices --\n";
  std::vector<std::pair<std::uint64_t, std::uint32_t>> by_count;
  for (const auto& [tile, count] : distinct_) by_count.emplace_back(count, tile);
  std::sort(by_count.begin(), by_count.end(), std::greater<>());
  for (std::size_t i = 0; i < by_count.size() && i < 25; ++i) {
    const auto [count, tile] = by_count[i];
    const auto sample = samples_.find(tile);
    out_ << std::format("  tile={:<5} seen={:<6} ch={}{}\n", tile, count,
                        debug_quoted(sample == samples_.end() ? "" : sample->second),
                        tile >= tile_count_ ? " OUT-OF-RANGE" : "");
  }
  out_.flush();
}

std::unique_ptr<std::ofstream> file_from_env(const char* var) {
  const char* path = std::getenv(var);
  if (!path || !*path) return nullptr;
  auto file = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
  if (!*file) {
    std::cerr << "[debug] could not open " << path << "\n";
    return nullptr;
  }
  std::cerr << "[debug] writing " << var << " to " << path << "\n";
  return file;
}

}  // namespace nh
