#pragma once

// A live game session, whatever carries it.
//
// Port of src-tauri/src/session.rs. NetHack over SSH and NetHack on this
// machine differ only in how the bytes reach a pseudo-terminal. Everything
// above (the demultiplexer, the tile overlay, the status bar) wants two
// things from either: a stream of bytes back, and a place to send keys and
// window sizes. That shape is here, so neither transport knows the other.

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace nh {

// Raw bytes from the game, for the demultiplexer.
struct SessionData {
  std::string bytes;
};

// Progress or a warning for the status bar.
struct SessionStatus {
  std::string message;
};

// The session ended. `reason` is empty for a clean exit.
struct SessionClosed {
  std::optional<std::string> reason;
};

using SessionEvent = std::variant<SessionData, SessionStatus, SessionClosed>;

// Receives the events. A transport calls it on its own thread.
using SessionEvents = std::function<void(SessionEvent)>;

class Disconnected : public std::runtime_error {
 public:
  Disconnected() : std::runtime_error("the session is no longer connected") {}
};

// A running session. Each call only queues work for the transport's thread,
// so all of them are safe from any thread. Destroying a session disconnects
// it and waits for its thread.
class Session {
 public:
  virtual ~Session() = default;

  // Starts the flow of events. A transport holds back game output until this
  // call, so the caller can finish its own setup first.
  virtual void start() = 0;

  // Sends keys to the game. Throws Disconnected once the session has ended.
  virtual void write(std::string bytes) = 0;

  // Tells the game the terminal changed size. Throws Disconnected.
  virtual void resize(std::uint32_t cols, std::uint32_t rows) = 0;

  virtual void disconnect() = 0;

  virtual bool is_connected() const = 0;
};

}  // namespace nh
