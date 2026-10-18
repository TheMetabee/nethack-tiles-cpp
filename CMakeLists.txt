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

# SSH to the public servers: libssh on mbedTLS. mbedTLS builds with plain
# CMake on every platform, where OpenSSL needs Perl and NASM on Windows.
#
# mbedTLS is linked into libssh statically. libssh is LGPL-2.1, so it stays a
# shared library: the app is MIT, and a player can replace the libssh library.
# Every file that includes an mbedTLS header must see the same
# configuration, so this applies to all later targets. See the header for why.
include_directories(${CMAKE_SOURCE_DIR}/cmake/mbedtls)
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="nh_mbedtls_config.h")
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
# The release tarball, not a git clone: 3.6.x needs its `framework`
# submodule, and git cannot check that out under a deep build directory on
# Windows, because some of its test file names pass MAX_PATH. The tarball
# holds the framework already. The hash is the one in the release notes.
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# A static library that goes into a shared one needs position-independent code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_MakeAvailable(mbedtls)

# libssh finds mbedTLS with find_library. Set its result to the targets above,
# so it links what this build makes. Its configure checks read headers only.
set(MBEDTLS_INCLUDE_DIR ${mbedtls_SOURCE_DIR}/include CACHE PATH "" FORCE)
set(MBEDTLS_SSL_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDTLS_CRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MBEDTLS_X509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(WITH_MBEDTLS ON CACHE BOOL "" FORCE)
# Only the client is needed: no compression, SFTP, server, Kerberos or pcap.
foreach(option WITH_ZLIB WITH_SFTP WITH_SERVER WITH_GSSAPI WITH_EXAMPLES WITH_NACL
               WITH_PCAP WITH_SYMBOL_VERSIONING UNIT_TESTING CLIENT_TESTING
               WITH_DEBUG_CALLTRACE)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libssh
  GIT_REPOSITORY https://git.libssh.org/projects/libssh.git
  GIT_TAG libssh-0.12.2
  GIT_SHALLOW TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -P
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/libssh-fixes.cmake)
set(BUILD_SHARED_LIBS ON)
FetchContent_MakeAvailable(libssh)
unset(BUILD_SHARED_LIBS)
# Its warnings are libssh's to fix, and they bury this project's own.
if(MSVC)
  target_compile_options(ssh PRIVATE /w)
else()
  target_compile_options(ssh PRIVATE -w)
endif()

# An archive, not a git clone, for the same reason as mbedTLS: some of its
# example file names pass MAX_PATH under a deep build directory.
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
  URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(doctest)

# --- Core: pure logic, no UI or network. Ported from the Rust backend. ------

add_library(nethack_core STATIC
  src/core/autologin.cpp
  src/core/banner.cpp
  src/core/debuglog.cpp
  src/core/demux.cpp
  src/core/game_stream.cpp
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

# --- Net: the SSH transport, on libssh -----------------------------------------

add_library(nethack_net STATIC src/net/ssh.cpp)
target_link_libraries(nethack_net PUBLIC nethack_core PRIVATE ssh::ssh)
if(MSVC)
  target_compile_options(nethack_net PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(nethack_net PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- App: hosts the frontend and answers its commands -----------------------

add_executable(nethack_tiles
  src/app/main.cpp
  src/app/bridge.cpp)
target_link_libraries(nethack_tiles PRIVATE nethack_core nethack_net webview::core nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(nethack_tiles PRIVATE /W4 /permissive- /utf-8)
endif()
if(WIN32)
  target_compile_definitions(nethack_tiles PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# libssh is a shared library (see above); Windows finds it beside the .exe.
if(WIN32)
  add_custom_command(TARGET nethack_tiles POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:nethack_tiles> $<TARGET_FILE_DIR:nethack_tiles>
    COMMAND_EXPAND_LISTS)
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
  tests/game_stream_test.cpp
  tests/glyph_test.cpp
  tests/ssh_test.cpp
  tests/statelog_test.cpp
  tests/tileset_test.cpp
  tests/tilesrc_test.cpp)
target_link_libraries(core_tests PRIVATE nethack_core nethack_net doctest::doctest_with_main)
if(MSVC)
  target_compile_options(core_tests PRIVATE /W4 /permissive- /utf-8)
endif()
if(WIN32)
  add_custom_command(TARGET core_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:core_tests> $<TARGET_FILE_DIR:core_tests>
    COMMAND_EXPAND_LISTS)
endif()
add_test(NAME core_tests COMMAND core_tests)
