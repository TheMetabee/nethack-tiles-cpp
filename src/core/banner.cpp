#include "core/banner.hpp"

#include <charconv>
#include <cctype>
#include <vector>

#include "core/autologin.hpp"

namespace nh {

namespace {

// The word in front of the number. It is capitalised, which keeps game
// messages that use the word from a match.
constexpr std::string_view kMarker = "Version ";

// How much text to hold across reads. The banner line is under 80 columns,
// and a session is megabytes of map redraws, so only a tail is kept.
constexpr std::size_t kTailLimit = 512;

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::optional<unsigned> parse_number(std::string_view s) {
  unsigned value = 0;
  const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (s.empty() || error != std::errc() || end != s.data() + s.size()) return std::nullopt;
  return value;
}

// Reads `3.6.7` or `5.0.0-0` and says which tile ordering it uses.
//
// The outer optional is "this is a version at all"; the inner one is "this
// app has a sheet for it". A release with no sheet is still worth naming,
// because the name tells the player why the tiles look wrong.
std::optional<std::optional<NetHackVersion>> parse_token(std::string_view token) {
  const std::string_view number = token.substr(0, token.find('-'));
  std::vector<std::string_view> parts;
  for (std::size_t from = 0;;) {
    const std::size_t dot = number.find('.', from);
    parts.push_back(number.substr(from, dot - from));
    if (dot == std::string_view::npos) break;
    from = dot + 1;
  }
  if (parts.size() < 2) return std::nullopt;
  // Every component must be a number, or this is prose.
  std::vector<unsigned> values;
  for (const auto part : parts) {
    const auto value = parse_number(part);
    if (!value) return std::nullopt;
    values.push_back(*value);
  }

  const unsigned major = values[0], minor = values[1];
  if (major == 3 && minor == 6) return std::optional(NetHackVersion::V36);
  // 3.7 is where the tile ordering changed, and 5.0 is that same lineage
  // renamed.
  if ((major == 3 && minor == 7) || (major == 5 && minor == 0)) {
    return std::optional(NetHackVersion::V50);
  }
  return std::optional<NetHackVersion>();
}

// Finds a complete version token after the marker word.
std::optional<ServerVersion> find_version(std::string_view text) {
  for (std::size_t from = 0;;) {
    const std::size_t at = text.find(kMarker, from);
    if (at == std::string_view::npos) return std::nullopt;
    from = at + kMarker.size();

    // "Version" must start a word, not end one, so a message that reads
    // "...SubVersion 1.2 " does not count.
    if (at > 0 && !is_space(text[at - 1])) continue;

    // Something must follow the token before it can be trusted: a read that
    // stops after "Version 5.0" would otherwise give 5.0 when the real text
    // is 5.0.0-0.
    const std::string_view rest = text.substr(from);
    std::size_t end = 0;
    while (end < rest.size() && !is_space(rest[end])) ++end;
    if (end == rest.size()) continue;

    const std::string_view token = rest.substr(0, end);
    if (const auto version = parse_token(token)) {
      return ServerVersion{std::string(token), *version};
    }
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const ServerVersion& found) {
  os << "ServerVersion{\"" << found.text << "\", ";
  if (found.version) {
    os << *found.version;
  } else {
    os << "none";
  }
  return os << "}";
}

std::optional<ServerVersion> VersionWatch::observe(std::string_view chunk) {
  if (done_) return std::nullopt;

  // The banner is written with a cursor address in front of it, so the escape
  // codes must come out before the text reads as a line.
  tail_ += strip_ansi(chunk);

  if (auto found = find_version(tail_)) {
    done_ = true;
    tail_.clear();
    return found;
  }

  // Keep only what a split banner could still need.
  if (tail_.size() > kTailLimit) tail_.erase(0, tail_.size() - kTailLimit);
  return std::nullopt;
}

}  // namespace nh
