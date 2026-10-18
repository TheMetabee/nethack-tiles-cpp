# nethack-tiles-cpp

The NetHack Tiles Client with a C++ backend in place of Rust. The React +
xterm.js frontend is unchanged apart from one file: `frontend/src/lib/backend.ts`
replaces Tauri's `invoke` and `listen` with a two-function bridge.

See [docs/PORTING.md](docs/PORTING.md) for what is ported and what is next.

## Run (Windows, MSVC 2022)

```sh
cd frontend
npx pnpm@11.8.0 install
npx pnpm@11.8.0 run dev        # Vite on http://localhost:1420
```

In a second terminal, from the repo root:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug
build/Debug/nethack_tiles.exe
```

A Debug build loads `http://localhost:1420`, so frontend edits hot-reload.

A Release build needs no Vite. The build runs `pnpm run build` and copies
`frontend/dist` to `build/Release/frontend`, and the app serves that folder at
`https://nethack-tiles.example/` inside WebView2:

```sh
cmake --build build --config Release
build/Release/nethack_tiles.exe
```

Set `NH_DEMO=1` to play a demo room in place of SSH, for work on the page.

Run `pnpm install` in `frontend/` once before the first build. To build only
the C++ side, configure with `-DNH_BUILD_FRONTEND=OFF`. Set
`NH_FRONTEND_URL` to load another address in any build.

To inspect the page, set `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` to
`--remote-debugging-port=9222` before starting the app, then open
`edge://inspect` or `chrome://inspect`.

## Package (Windows)

```sh
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -B build/package
```

This makes `build/package/nethack-tiles-<version>-windows-x64.zip`: one
folder with `nethack_tiles.exe`, `ssh.dll`, the tile sheets, the built
frontend, `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`. Unzip it anywhere and
run the exe. `cmake/notices.cmake` writes the notices from the licence files
of everything bundled.

A Release exe has the app icon and version details and opens no console
window, as the Rust app's release build does. To see its log, run a Debug
build, which keeps the console. If the WebView2 runtime is missing, the app
says so and offers Microsoft's download page.

## CI

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request:

| Job | What it checks |
|---|---|
| Frontend | Types, the bundle and the Vitest tests |
| Windows | The whole app in Release, with the frontend built in, every test, and the ZIP package. The app's files are kept as the `nethack-tiles-windows` artifact for 14 days: unzip it and run `nethack_tiles.exe`. |
| Core (Linux, macOS) | The libraries, tools and tests, with `-DNH_BUILD_APP=OFF` |

`NH_BUILD_APP=OFF` builds everything except the app itself. Use it where
the system webview's development files are missing, such as Linux without
WebKitGTK.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Pure logic ported from the Rust backend. No UI, no network. |
| `src/net/` | The SSH transport, on libssh. |
| `cmake/` | The mbedTLS config for libssh and the libssh fixes. |
| `src/app/bridge.*` | The command and event channel to the page. |
| `src/app/main.cpp` | The window, the commands, the demo session. |
| `tests/` | doctest tests, ported case for case from the Rust tests. |
| `tools/rust-compat/` | Runs the Rust app's profile and keychain code, to check the C++ port against it. Not built by CMake. |
| `tools/tiles2png.cpp` | Builds a tile sheet from NetHack's `win/share/*.txt`. Not part of the app. |
| `frontend/` | The React + xterm.js frontend. |
| `assets/tiles/` | Tile sheets and their manifests, copied next to the executable. |

## The bridge

| Direction | Page side | C++ side |
|---|---|---|
| Command | `window.__nh_invoke(command, args)` returns a promise | `bridge.command(name, handler)`; the handler returns JSON or throws |
| Event | `window.__nh_emit(event, payload)` calls the listeners | `bridge.emit(event, payload)`, safe from any thread |

Command and event names are the same as in the Tauri app.
