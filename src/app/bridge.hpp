#pragma once

// The command and event channel between the frontend and the C++ backend.
// It replaces Tauri's `invoke` and `emit`.
//
// Commands: the frontend calls `window.__nh_invoke(command, args)`. The
// handler registered under `command` runs on the UI thread and returns JSON,
// or throws to reject the frontend's promise with the message.
//
// Events: `emit` may be called from any thread. It posts to the UI thread,
// which calls `window.__nh_emit(event, payload)` in the page.

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <webview/webview.h>

namespace nh::app {

using json = nlohmann::json;

class Bridge {
 public:
  using Command = std::function<json(const json& args)>;
  // Settles the page's promise: resolves it with `value`, or rejects it with
  // the message in `value` when `ok` is false. Safe from any thread, once.
  using Reply = std::function<void(bool ok, const json& value)>;
  using AsyncCommand = std::function<void(const json& args, Reply reply)>;

  explicit Bridge(webview::webview& view);

  void command(const std::string& name, Command handler);
  // For work too slow for the UI thread. The handler starts it and returns;
  // the work calls `reply` when it is done. If the handler throws, the
  // promise is rejected at once.
  void command_async(const std::string& name, AsyncCommand handler);
  void emit(const std::string& event, const json& payload);

 private:
  void invoke(const std::string& id, const std::string& request);

  webview::webview& view_;
  std::map<std::string, AsyncCommand> commands_;
};

// The stream is bytes, not UTF-8. JSON carries it as a string whose code
// points are the byte values (latin-1), and the frontend reverses that with
// `latin1ToBytes`.
std::string latin1_to_utf8(std::string_view bytes);

}  // namespace nh::app
