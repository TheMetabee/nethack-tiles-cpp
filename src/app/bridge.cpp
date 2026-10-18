#include "app/bridge.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace nh::app {

Bridge::Bridge(webview::webview& view) : view_(view) {
  view_.bind(
      "__nh_invoke",
      [this](const std::string& id, const std::string& request, void*) { invoke(id, request); },
      nullptr);
}

void Bridge::command(const std::string& name, Command handler) {
  commands_[name] = std::move(handler);
}

void Bridge::emit(const std::string& event, const json& payload) {
  // ensure_ascii keeps the script plain ASCII whatever the payload holds.
  std::string js = "window.__nh_emit && window.__nh_emit(" + json(event).dump() + "," +
                   payload.dump(-1, ' ', true) + ")";
  view_.dispatch([this, js = std::move(js)] { view_.eval(js); });
}

void Bridge::invoke(const std::string& id, const std::string& request) {
  std::string name;
  try {
    // The page passes (command, args), which webview sends as a JSON array.
    const json call = json::parse(request);
    name = call.at(0).get<std::string>();
    const json args = call.size() > 1 && call[1].is_object() ? call[1] : json::object();

    const auto found = commands_.find(name);
    if (found == commands_.end()) throw std::runtime_error("unknown command " + name);
    view_.resolve(id, 0, found->second(args).dump(-1, ' ', true));
  } catch (const std::exception& error) {
    std::cerr << "[command] " << (name.empty() ? "?" : name) << " failed: " << error.what()
              << "\n";
    view_.resolve(id, 1, json(error.what()).dump(-1, ' ', true));
  }
}

std::string latin1_to_utf8(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    if (b < 0x80) {
      out.push_back(c);
    } else {
      out.push_back(static_cast<char>(0xc0 | (b >> 6)));
      out.push_back(static_cast<char>(0x80 | (b & 0x3f)));
    }
  }
  return out;
}

}  // namespace nh::app
