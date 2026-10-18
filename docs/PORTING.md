# Porting plan: Rust backend to C++

Source: `nethack-tiles-client` v0.1.10 (Tauri 2). The goal is no Rust. The
React + xterm.js frontend stays.

## Approach

[webview/webview](https://github.com/webview/webview) opens the window and
hosts the frontend in the system webview: WebView2 on Windows, WKWebView on
macOS, WebKitGTK on Linux. That is the part of Tauri the app uses. The rest
of Tauri is replaced piece by piece below.

The frontend talks to the backend only through `frontend/src/lib/backend.ts`.
It keeps the Tauri app's function names, command names and event names, so
no component changed except for its import line. `ProfileForm.tsx` calls
`pickFolder` instead of the Tauri dialog plugin.

## Status

Proven end to end in the spike (Windows):

- The window loads the frontend from the Vite dev server.
- Commands: tilesets and default profiles come from C++ and the server list
  renders.
- Events: connecting starts a demo session. Its stream goes through the
  ported demuxer, reaches xterm.js as `nh://stream` events, and the tile
  overlay draws a room with the 3.6 sheet. Keys come back as `session_write`.
  A resize makes the demo redraw for the new size, as NetHack does.
- Every pure module is ported: `demux.rs`, `glyph.rs`, `banner.rs`,
  `autologin.rs`, `tileset.rs`, `tilesrc.rs`, `statelog.rs` and
  `debuglog.rs`, with all their tests and a few more (130 cases).
- `tiles2png` rebuilds both bundled sheets from NetHack's sources with no
  pixel changed and the same manifests. See Notes for the 5.0 source.
- Custom tilesets and the state log work in the app. The state log was
  checked end to end: the frontend writes its files through C++ into a
  directory with a non-ASCII name.
- Native folder picker (Windows) and opening links in the browser.
- A Release build serves the built frontend from disk with no Vite
  (Windows). Checked end to end with the demo session.

## Module map

Effort: **S** is a mechanical port, **M** needs a library choice or a
redesign.

### Backend (`src-tauri/src`)

| Rust module | Lines | C++ plan | Effort | Status |
|---|---|---|---|---|
| `demux.rs` | 666 | `core/demux` | S | done |
| `glyph.rs` | 197 | `core/glyph`; `main.cpp` uses it for the stream | S | done |
| `tileset.rs` | 290 | `core/tileset`, PNG check with stb_image. Bundled and custom sheets | S | done |
| `tilesrc.rs` | 542 | `core/tilesrc`, and `tools/tiles2png` in place of the Rust example | S | done |
| `banner.rs` | 265 | `core/banner`; not yet wired to a session | S | done |
| `autologin.rs` | 499 | `core/autologin`; waits for SSH | S | done |
| `statelog.rs` | 185 | `core/statelog`; `state_log_write` and the clear on connect | S | done |
| `debuglog.rs` | 304 | `core/debuglog`; the demo session feeds it (`NHTILES_LOG`, `NHTILES_RAW`) | S | done |
| `session.rs` | 85 | Transport interface: a worker thread, a command queue, events through `Bridge::emit` | S | shape proven by the demo |
| `ssh.rs` | 446 | libssh on the worker thread; it has known_hosts trust-on-first-use | M | |
| `profiles.rs` | 1101 | TOML storage (toml++) and a keychain layer: Windows Credential Manager, macOS Keychain, Linux libsecret | M | defaults only, in memory |
| `local.rs` | 702 | `forkpty` on Unix; Windows stays unsupported (ConPTY later) | M | |
| `app.rs`, `lib.rs`, `icon.rs` | ~1200 | `src/app/` | | most commands stubbed |

### Tauri itself

| Tauri piece | C++ replacement | Status |
|---|---|---|
| Window and webview | webview/webview 0.12.0 | done |
| `invoke` and `emit` | `Bridge` (`bind`, `resolve`, `dispatch`, `eval`) and nlohmann/json | done |
| Dialog plugin | `IFileOpenDialog` on Windows; NSOpenPanel and GTK later | Windows done |
| Opener plugin | `ShellExecuteW`, `open`, `xdg-open` | done |
| Serving the built frontend | WebView2 `SetVirtualHostNameToFolderMapping`; WKURLSchemeHandler and a WebKitGTK URI scheme later | Windows done |
| Bundler, notarisation | CPack (WiX, DragNDrop, DEB) and the same notarisation steps | |
| WebView2 runtime | Ships with Windows 10 and 11; the installer adds Microsoft's bootstrapper for the rest | |

## Next steps

1. SSH with libssh behind the session interface the demo already uses. Then
   auto-login.
2. Profile storage and the keychain.
3. The local Unix transport.
4. Packaging and CI.

## Notes

- The frontend's own resize fix (overflow clipping and the 200 ms settle)
  came across with the copy. The spike's log shows one resize per connect.
- Commands run on the UI thread. Anything slow (the SSH connect) must run on
  a worker thread and resolve later, or the window freezes.
- Every target builds with `/utf-8`. Without it MSVC reads a source file in
  the ANSI code page, and a non-ASCII literal changes without a warning.
- Paths from the page are UTF-8. Make them with
  `nh::statelog::path_from_utf8`, not `fs::path(std::string)`, which reads
  the ANSI code page on Windows.
- The bundled 5.0 sheet comes from NetHack source at `18845b6f6` ("match
  magic object tiles with mundane counterparts"), not from the
  `NetHack-5.0.0_Released` tag. The tag gives 3 other pixels in two tiles:
  the magic flute and the drum of earthquake. To rebuild the sheet, use the
  later `objects.txt`.
- `tiles2png` is a developer tool. Keep it out of any installer, as the Rust
  app keeps its tools in `examples/`.
- WebView2's folder mapping has no default document. `https://host/` names
  the folder and fails with ERR_ACCESS_DENIED, so the app loads
  `/index.html` by name.
