#include "core/demux.hpp"

#include <charconv>
#include <iomanip>

namespace nh {
namespace {

constexpr char kEsc = 0x1b;

// CSI sequences end on a byte in 0x40..0x7E.
bool is_csi_final(unsigned char b) { return b >= 0x40 && b <= 0x7e; }

// True for bytes a terminal draws into a cell. Bytes above 0x7F are the line
// drawing characters of NetHack's IBM and DEC graphics modes.
bool prints(unsigned char b) { return (b >= 0x20 && b < 0x7f) || b >= 0x80; }

// Splits `;`-separated numbers. An empty or invalid parameter is nullopt
// ("use the default"), as in ECMA-48.
std::vector<std::optional<std::int64_t>> parse_params(std::string_view body) {
  std::vector<std::optional<std::int64_t>> params;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = body.find(';', start);
    const std::string_view part =
        body.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
    if (!part.empty() && ec == std::errc{} && ptr == part.data() + part.size()) {
      params.emplace_back(value);
    } else {
      params.emplace_back(std::nullopt);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return params;
}

std::uint32_t to_u32(std::int64_t v) { return static_cast<std::uint32_t>(v < 0 ? 0 : v); }

}  // namespace

std::vector<StreamItem> Demuxer::feed(std::string_view bytes) {
  std::vector<StreamItem> out;
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    switch (state_) {
      case State::Ground:
        if (c == kEsc) {
          state_ = State::Esc;
        } else {
          push({&c, 1}, prints(b), out);
        }
        break;

      case State::Esc:
        if (c == '[') {
          csi_.clear();
          state_ = State::Csi;
        } else if (c == ']') {
          push("\x1b]", false, out);
          osc_len_ = 0;
          state_ = State::Osc;
        } else if (c == kEsc) {
          // ESC ESC: the first is literal; stay armed for the second.
          push({&c, 1}, false, out);
        } else if (b >= 0x20 && b <= 0x2f) {
          // An intermediate byte means one more byte follows, as in ESC ( B.
          const char seq[] = {kEsc, c};
          push({seq, 2}, false, out);
          state_ = State::EscIntermediate;
        } else {
          const char seq[] = {kEsc, c};
          push({seq, 2}, false, out);
          state_ = State::Ground;
        }
        break;

      case State::EscIntermediate:
        push({&c, 1}, false, out);
        if (!(b >= 0x20 && b <= 0x2f)) state_ = State::Ground;
        break;

      case State::Osc:
        push({&c, 1}, false, out);
        ++osc_len_;
        if (c == 0x07) {
          state_ = State::Ground;
        } else if (c == kEsc) {
          state_ = State::OscEsc;
        } else if (osc_len_ > kMaxOscLen) {
          // The terminator was lost. Recover as text rather than treat the
          // rest of the session as one long title.
          state_ = State::Ground;
        }
        break;

      case State::OscEsc:
        push({&c, 1}, false, out);
        state_ = c == '\\' ? State::Ground : State::Osc;
        break;

      case State::Csi:
        if (csi_.size() >= kMaxCsiLen) {
          // Runaway sequence: pass it through as text instead of buffering
          // without limit.
          const std::string csi = std::move(csi_);
          csi_.clear();
          push("\x1b[", false, out);
          push(csi, false, out);
          push({&c, 1}, false, out);
          state_ = State::Ground;
          break;
        }
        csi_.push_back(c);
        if (is_csi_final(b)) {
          const std::string csi = std::move(csi_);
          csi_.clear();
          finish_csi(csi, out);
          state_ = State::Ground;
        }
        break;
    }
  }
  flush_text(out);
  return out;
}

void Demuxer::finish_csi(const std::string& csi, std::vector<StreamItem>& out) {
  const char final_byte = csi.back();
  const auto params = parse_params(std::string_view(csi).substr(0, csi.size() - 1));

  const bool is_tile_code = final_byte == 'z' && !params.empty() && params[0] == 1;
  if (!is_tile_code) {
    push("\x1b[", false, out);
    push(csi, false, out);
    return;
  }

  saw_tile_data_ = true;
  const auto param = [&](std::size_t i) -> std::optional<std::int64_t> {
    return i < params.size() ? params[i] : std::nullopt;
  };

  std::optional<TileEvent> event;
  switch (param(1).value_or(-1)) {
    case 0:
      if (const auto tile = param(2)) {
        event = GlyphStart{to_u32(*tile), to_u32(param(3).value_or(0))};
      }
      break;
    case 1: event = GlyphEnd{}; break;
    case 2: event = SelectWindow{param(2)}; break;
    case 3: event = FrameSync{}; break;
    case 4: event = Sound{param(2)}; break;
    default:
      // Unknown or absent subcode: consume it so it never reaches the
      // terminal, but emit nothing.
      break;
  }
  if (event) {
    flush_text(out);
    out.emplace_back(Event{*event});
  }
}

void Demuxer::push(std::string_view bytes, bool prints, std::vector<StreamItem>& out) {
  if (!text_.empty() && text_prints_ != prints) flush_text(out);
  text_prints_ = prints;
  text_.append(bytes);
}

void Demuxer::flush_text(std::vector<StreamItem>& out) {
  if (!text_.empty()) {
    out.emplace_back(Text{std::move(text_), text_prints_});
    text_.clear();
  }
}

namespace {

void write_escaped(std::ostream& os, std::string_view bytes) {
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    if (b >= 0x20 && b < 0x7f && c != '\\') {
      os << c;
    } else {
      os << "\\x" << std::hex << std::setw(2) << std::setfill('0') << int{b} << std::dec;
    }
  }
}

void write_opt(std::ostream& os, const std::optional<std::int64_t>& v) {
  if (v) {
    os << *v;
  } else {
    os << "none";
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const StreamItem& item) {
  if (const auto* text = std::get_if<Text>(&item)) {
    os << (text->prints ? "text(\"" : "ctrl(\"");
    write_escaped(os, text->bytes);
    return os << "\")";
  }
  const auto& event = std::get<Event>(item).event;
  if (const auto* g = std::get_if<GlyphStart>(&event)) {
    return os << "GlyphStart{" << g->tile << ", " << g->flags << "}";
  }
  if (std::holds_alternative<GlyphEnd>(event)) return os << "GlyphEnd";
  if (const auto* w = std::get_if<SelectWindow>(&event)) {
    os << "SelectWindow{";
    write_opt(os, w->winid);
    return os << "}";
  }
  if (std::holds_alternative<FrameSync>(event)) return os << "FrameSync";
  os << "Sound{";
  write_opt(os, std::get<Sound>(event).id);
  return os << "}";
}

}  // namespace nh
