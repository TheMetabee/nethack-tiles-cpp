cmake_minimum_required(VERSION 3.24)
project(nethack_tiles_cpp VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# --- Dependencies, each pinned to a release tag -----------------------------

# The window and the system webview that hosts the frontend: WebView2 on
# Windows, WKWebView on macOS, WebKitGTK on Linux.
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(webview
  GIT_REPOSITORY https://github.com/webview/webview.git
  GIT_TAG 0.12.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(webview)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

# PNG decoding and encoding. stb has no release tags, so pin a commit.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

# --- Core: pure logic, no UI or network. Ported from the Rust backend. ------

add_library(nethack_core STATIC
  src/core/autologin.cpp
  src/core/banner.cpp
  src/core/debuglog.cpp
  src/core/demux.cpp
  src/core/glyph.cpp
  src/core/statelog.cpp
  src/core/stb.cpp
  src/core/tileset.cpp
  src/core/tilesrc.cpp)
target_include_directories(nethack_core PUBLIC src)
target_include_directories(nethack_core SYSTEM PUBLIC ${stb_SOURCE_DIR})
target_link_libraries(nethack_core PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
  target_compile_definitions(nethack_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(nethack_core PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(nethack_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- App: hosts the frontend and answers its commands -----------------------

add_executable(nethack_tiles
  src/app/main.cpp
  src/app/bridge.cpp)
target_link_libraries(nethack_tiles PRIVATE nethack_core webview::core nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(nethack_tiles PRIVATE /W4 /permissive- /utf-8)
endif()
if(WIN32)
  target_compile_definitions(nethack_tiles PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# Tile sheets ship next to the executable.
add_custom_command(TARGET nethack_tiles POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:nethack_tiles>/assets)

# --- Frontend: built with Vite, served from disk by Release builds ----------

# Turn this off to build only the C++ side, e.g. while Vite serves the page.
option(NH_BUILD_FRONTEND "Build the frontend and copy it next to the executable" ON)
if(NH_BUILD_FRONTEND)
  find_program(NPX NAMES npx.cmd npx REQUIRED)
  set(FRONTEND_DIR ${CMAKE_SOURCE_DIR}/frontend)
  file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS ${FRONTEND_DIR}/src/*)
  # Vite empties dist/ and writes index.html again on every build, so that
  # one file stands for the whole output.
  add_custom_command(
    OUTPUT ${FRONTEND_DIR}/dist/index.html
    COMMAND ${NPX} --yes pnpm@11.8.0 run build
    WORKING_DIRECTORY ${FRONTEND_DIR}
    DEPENDS ${FRONTEND_SOURCES}
      ${FRONTEND_DIR}/index.html ${FRONTEND_DIR}/package.json
      ${FRONTEND_DIR}/pnpm-lock.yaml ${FRONTEND_DIR}/vite.config.ts
      ${FRONTEND_DIR}/tsconfig.json ${FRONTEND_DIR}/tsconfig.node.json
    COMMENT "Building the frontend (run pnpm install in frontend/ first)"
    VERBATIM)
  add_custom_target(frontend ALL DEPENDS ${FRONTEND_DIR}/dist/index.html)
  # Asset names carry a content hash, so a plain copy would keep every old
  # bundle. Replace the folder.
  add_custom_command(TARGET frontend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:nethack_tiles>/frontend
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${FRONTEND_DIR}/dist $<TARGET_FILE_DIR:nethack_tiles>/frontend
    VERBATIM)
endif()

# --- Developer tools --------------------------------------------------------

# Builds a tile sheet from NetHack's win/share/*.txt files. It is not part of
# the app and must stay out of any installer.
add_executable(tiles2png tools/tiles2png.cpp)
target_link_libraries(tiles2png PRIVATE nethack_core)
if(MSVC)
  target_compile_options(tiles2png PRIVATE /W4 /permissive- /utf-8)
endif()

# --- Tests ------------------------------------------------------------------

enable_testing()
add_executable(core_tests
  tests/autologin_test.cpp
  tests/banner_test.cpp
  tests/debuglog_test.cpp
  tests/demux_test.cpp
  tests/glyph_test.cpp
  tests/statelog_test.cpp
  tests/tileset_test.cpp
  tests/tilesrc_test.cpp)
target_link_libraries(core_tests PRIVATE nethack_core doctest::doctest_with_main)
if(MSVC)
  target_compile_options(core_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME core_tests COMMAND core_tests)
