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
- SSH: the app connects to NAO and Hardfought with libssh, checks their
  host keys against the `~/.ssh/known_hosts` entries the Rust app wrote, and
  shows the dgamelaunch menu. Keys, resize and disconnect work. A refused
  connection, an address that never answers (20 s) and an unknown host name
  each give the Rust app's error, and the page shows it on the server list.
- The per-chunk work of `consume_session` is `core/game_stream`: the banner
  check with its version warning, the auto-login, the demuxer and the
  tile-data notice. `NH_DEMO=1` swaps SSH for the old demo room, which goes
  through the same pipeline.
- Every pure module is ported: `demux.rs`, `glyph.rs`, `banner.rs`,
  `autologin.rs`, `tileset.rs`, `tilesrc.rs`, `statelog.rs` and
  `debuglog.rs`, with all their tests and a few more. With the SSH and
  session tests there are 160 cases.
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
| `banner.rs` | 265 | `core/banner`; `core/game_stream` sends `nh://server-version` | S | done |
| `autologin.rs` | 499 | `core/autologin`, run by `core/game_stream` | S | done |
| `statelog.rs` | 185 | `core/statelog`; `state_log_write` and the clear on connect | S | done |
| `debuglog.rs` | 304 | `core/debuglog`; every session feeds it (`NHTILES_LOG`, `NHTILES_RAW`) | S | done |
| `session.rs` | 85 | `core/session`: the `Session` interface and its events | S | done |
| `ssh.rs` | 446 | `net/ssh`: libssh 0.12.2 on mbedTLS 3.6.7, one pump thread per session | M | done (Windows) |
| `profiles.rs` | 1101 | TOML storage (toml++) and a keychain layer: Windows Credential Manager, macOS Keychain, Linux libsecret | M | defaults only; profiles and passwords in memory for one run |
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

1. Profile storage and the keychain.
2. The local Unix transport.
3. Packaging and CI.

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
- libssh is LGPL-2.1 and the app is MIT, so libssh stays a shared library
  (`ssh.dll` beside the executable). mbedTLS is linked into it statically.
- mbedTLS must have threads on for libssh. `cmake/mbedtls/` holds the user
  config: pthreads on Unix, and on Windows `MBEDTLS_THREADING_ALT` with the
  locks that libssh's winlocks.c gives.
- `cmake/patches/libssh-fixes.cmake` fixes three libssh 0.12.2 defects that
  this build hits on Windows: a missing return in the threading start-up, a
  refused connection that waited for the whole timeout, and socket errors
  that read "Unknown error". Each fix stops the configure if a new libssh
  no longer matches, so it is checked at every libssh update. They are worth
  sending upstream.
- libssh is not safe to call from two threads, so each SSH session does all
  its libssh calls on its pump thread. Keys wait for that thread's next
  10 ms turn.
- Checked live with a real game account (2026-10-18): the auto-login logs
  in, and a game over SSH draws its tiles.
- Not yet checked live: recording an unknown host key (trust on first use)
  and refusing a changed one. The decisions are unit-tested.
- mbedTLS and doctest come as pinned release archives with a SHA-256, not as
  git clones. Both carry test or example files with very long names, and
  git cannot check those out under a deep build directory on Windows
  (MAX_PATH). CMake's own extraction handles the long paths.
