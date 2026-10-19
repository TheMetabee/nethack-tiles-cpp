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
  commands_[name] = [handler = std::move(handler)](const json& args, Reply reply) {
    reply(true, handler(args));
  };
}

void Bridge::command_async(const std::string& name, AsyncCommand handler) {
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
    // The page passes (command, argsJson), which webview sends as a JSON
    // array. argsJson is the arguments as plain-ASCII JSON text (see
    // asciiJson in backend.ts): webview's own parser drops a call that
    // holds a raw DEL, which is what Backspace sends.
    const json call = json::parse(request);
    name = call.at(0).get<std::string>();
    json args = json::object();
    if (call.size() > 1 && call[1].is_string()) {
      args = json::parse(call[1].get<std::string>());
    } else if (call.size() > 1 && call[1].is_object()) {
      args = call[1];
    }
    if (!args.is_object()) throw std::runtime_error("the arguments are not a JSON object");

    const auto found = commands_.find(name);
    if (found == commands_.end()) throw std::runtime_error("unknown command " + name);
    found->second(args, [this, id, name](bool ok, const json& value) {
      if (!ok) {
        std::cerr << "[command] " << name << " failed: " << value.get<std::string>() << "\n";
      }
      // Resolve on the UI thread, in order with any events sent before it.
      view_.dispatch([this, id, ok, result = value.dump(-1, ' ', true)] {
        view_.resolve(id, ok ? 0 : 1, result);
      });
    });
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
