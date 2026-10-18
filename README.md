# nethack-tiles-cpp

A C++ port of [NetHack Tiles Client](https://github.com/statico/nethack-tiles-client)
by Ian Langworth: play NetHack on the public servers, with graphical tiles.

The original is a Tauri app with a Rust backend. This port starts from its
v0.1.10 and replaces the Rust backend and Tauri with C++. The React +
xterm.js frontend is the original's, unchanged apart from one file:
`frontend/src/lib/backend.ts` replaces Tauri's `invoke` and `listen` with a
two-function bridge. It also has one fix from after v0.1.10: an open menu
hides only the tiles it covers, not the whole map. The tile sheets and the
app icon also come from the original. Both projects are under the MIT licence; see [LICENSE](LICENSE).

The port reads and writes the original app's saved profiles and, on Windows,
its saved passwords, so either app sees what the other saved.

See [docs/PORTING.md](docs/PORTING.md) for what is ported and what is next.

## Not ported yet

Everything else from the original works on Windows and Linux: SSH play on
the public servers, the tiles, saved profiles and passwords, custom tile
sheets and the state log. What remains:

| Piece | Where it is missing | What the port needs |
|---|---|---|
| Local play (`local.rs`): run a NetHack installed on this machine | All platforms; the app says it is not ported | A pseudo-terminal (`forkpty`) on Linux and macOS. The original does not run local games on Windows either; ConPTY could add it later. |
| The folder picker for the state log | Linux and macOS; Windows has one | A GTK dialog on Linux, `NSOpenPanel` on macOS. Until then, set `stateLogDirectory` in `profiles.toml` by hand. |
| The app on macOS | macOS builds and tests the core only | Serving the built frontend from disk (a `WKURLSchemeHandler`), finding the app's folder (`_NSGetExecutablePath`), the Keychain for passwords, then an app bundle, signing and notarisation. |

Not yet checked live, though unit-tested: recording a new server's host key
on first connect, and refusing a host key that has changed.

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

## Run on Linux

Install the development files for WebKitGTK and libsecret (Ubuntu 24.04),
then build as above:

```sh
sudo apt-get install -y libwebkit2gtk-4.1-dev libgtk-3-dev libsecret-1-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/nethack_tiles
```

A Release build serves `build/frontend/` itself, under `nethack-tiles://app/`.
`cpack --config build/CPackConfig.cmake -B build/package` makes
`nethack-tiles-<version>-linux-x64.tar.gz`: the app, `libssh.so.4` beside it
(found through the app's `$ORIGIN` rpath), the tile sheets, the frontend and
the licences. It needs WebKitGTK 4.1 and glibc 2.39 (Ubuntu 24.04) or newer.
The frontend build needs Node.js; without it, configure with
`-DNH_BUILD_FRONTEND=OFF` and copy a `frontend/dist` built elsewhere to
`build/frontend`. Game passwords go to the desktop keyring through libsecret
(the Secret Service). Local play is not ported to Linux yet.

In WSL, start the app with `WEBKIT_DISABLE_DMABUF_RENDERER=1`, or its window
stays blank, and detached (`setsid nohup ... &`), or WebKit's page process
ends with the WSL command and the window stops reacting.

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

## Releases

A release is two portable packages on a GitHub release, a Windows ZIP and a
Linux tar.gz. There is no installer.

1. Set `VERSION` in `project()` in `CMakeLists.txt`, for example `0.1.0`, and
   commit it. The ZIP's name and the exe's version details come from there.
2. Tag that commit with the same version and push the tag:

   ```sh
   git tag v0.1.0
   git push origin v0.1.0
   ```

3. `.github/workflows/release.yml` builds and tests the app on Windows and
   Linux, makes both packages and attaches them to a **draft** release with
   notes from the commits since the last release. Check the draft on GitHub,
   then publish it.

If the tag and `VERSION` differ, the workflow stops and says what to change.
To build an existing tag again, run the workflow by hand from the Actions tab
with that tag; it replaces the packages on the draft.

## CI

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request:

| Job | What it checks |
|---|---|
| Frontend | Types, the bundle and the Vitest tests |
| Windows | The whole app in Release, with the frontend built in, every test, and the ZIP package. The app's files are kept as the `nethack-tiles-windows` artifact for 14 days: unzip it and run `nethack_tiles.exe`. |
| Linux (app and core) | The whole app with WebKitGTK, the tools and the tests |
| Core (macOS) | The libraries, tools and tests, with `-DNH_BUILD_APP=OFF` |

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
