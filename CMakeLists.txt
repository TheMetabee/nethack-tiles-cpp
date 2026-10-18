cmake_minimum_required(VERSION 3.24)
project(nethack_tiles_cpp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# The app needs the system webview's development files: WebView2 comes with
# the build on Windows, but Linux needs WebKitGTK installed. Turn this off to
# build only the libraries, tests and tools, as CI does on Linux and macOS.
option(NH_BUILD_APP "Build the app itself, not only its libraries and tests" ON)

# --- Dependencies, each pinned to a release tag -----------------------------

# The window and the system webview that hosts the frontend: WebView2 on
# Windows, WKWebView on macOS, WebKitGTK on Linux.
if(NH_BUILD_APP)
  set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(webview)
endif()

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

# PNG decoding and encoding. stb has no release tags, so pin a commit.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)

# Profile storage reads and writes the Rust app's profiles.toml.
FetchContent_Declare(tomlplusplus
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tomlplusplus)

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
# 2.5 or later: 2.4.11 declares CMake 3.0 as its minimum, which CMake 4
# refuses, and CI runners have CMake 4.
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
  URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
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
  src/core/keychain.cpp
  src/core/profiles.cpp
  src/core/statelog.cpp
  src/core/static_files.cpp
  src/core/stb.cpp
  src/core/tileset.cpp
  src/core/tilesrc.cpp)
target_include_directories(nethack_core PUBLIC src)
target_include_directories(nethack_core SYSTEM PUBLIC ${stb_SOURCE_DIR})
target_link_libraries(nethack_core PUBLIC nlohmann_json::nlohmann_json
  PRIVATE tomlplusplus::tomlplusplus)
if(WIN32)
  # Credential Manager (advapi32) and the config folder (shell32, ole32).
  target_link_libraries(nethack_core PRIVATE advapi32 shell32 ole32)
endif()
if(WIN32)
  target_compile_definitions(nethack_core PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
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

if(NH_BUILD_APP)

add_executable(nethack_tiles
  src/app/main.cpp
  src/app/bridge.cpp)
target_link_libraries(nethack_tiles PRIVATE nethack_core nethack_net webview::core nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(nethack_tiles PRIVATE /W4 /permissive- /utf-8)
endif()
if(WIN32)
  target_compile_definitions(nethack_tiles PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

  # The icon and the version details (see packaging/windows/app.rc.in).
  set(NH_PRODUCT_NAME "NetHack Tiles Client")
  set(NH_ICON ${CMAKE_SOURCE_DIR}/packaging/windows/app.ico)
  configure_file(packaging/windows/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)
  target_sources(nethack_tiles PRIVATE ${CMAKE_BINARY_DIR}/app.rc)

  # A Release build opens no console window, as the Rust app's release build
  # does not. A Debug build keeps the console, which shows the log. Either
  # way the program starts at main().
  target_link_options(nethack_tiles PRIVATE /ENTRY:mainCRTStartup
    $<IF:$<CONFIG:Debug>,/SUBSYSTEM:CONSOLE,/SUBSYSTEM:WINDOWS>)
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

endif()  # NH_BUILD_APP

# --- Package: what a player gets ---------------------------------------------
#
#   cmake --build build --config Release
#   cpack --config build/CPackConfig.cmake -C Release -B build/package
#
# Every install rule here is in the `app` component, and the package takes
# only that component. The fetched libraries' own install rules (headers,
# static libraries, CMake files) stay out.

if(NH_BUILD_APP)
  include(cmake/notices.cmake)

  install(TARGETS nethack_tiles RUNTIME DESTINATION . COMPONENT app)
  # libssh goes beside the app on every system. Windows looks there for a
  # DLL on its own; Linux needs the app's rpath to say so ($ORIGIN is the
  # app's own folder). Only the versioned file and its soname link go in:
  # the libssh.so link is for building against it.
  if(WIN32)
    install(FILES $<TARGET_FILE:ssh> DESTINATION . COMPONENT app)
  else()
    install(TARGETS ssh LIBRARY DESTINATION . NAMELINK_SKIP COMPONENT app)
    set_target_properties(nethack_tiles PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  install(DIRECTORY assets/ DESTINATION assets COMPONENT app)
  if(NH_BUILD_FRONTEND)
    install(DIRECTORY ${FRONTEND_DIR}/dist/ DESTINATION frontend COMPONENT app)
  endif()
  install(FILES LICENSE DESTINATION . RENAME LICENSE.txt COMPONENT app)
  install(FILES ${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt DESTINATION . COMPONENT app)

  set(CPACK_PACKAGE_NAME "nethack-tiles")
  set(CPACK_PACKAGE_VENDOR "TheMetabee")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Play NetHack on public servers with graphical tiles")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TheMetabee/nethack-tiles-cpp")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "NetHack Tiles Client")
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
  # A ZIP on Windows, a tar.gz elsewhere: what each system unpacks without
  # extra tools, and a tar.gz keeps the libssh soname link and the app's
  # executable bit.
  if(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(NH_PACKAGE_SYSTEM windows)
  else()
    set(CPACK_GENERATOR TGZ)
    string(TOLOWER ${CMAKE_SYSTEM_NAME} NH_PACKAGE_SYSTEM)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(NH_PACKAGE_ARCH x64)
  else()
    string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} NH_PACKAGE_ARCH)
  endif()
  set(CPACK_COMPONENTS_ALL app)
  set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
  set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
  # One name for the file and the folder inside it.
  set(CPACK_PACKAGE_FILE_NAME
    "nethack-tiles-${PROJECT_VERSION}-${NH_PACKAGE_SYSTEM}-${NH_PACKAGE_ARCH}")
  set(CPACK_ARCHIVE_FILE_NAME ${CPACK_PACKAGE_FILE_NAME})
  # Unzips into one folder of that name, not loose files. A package made
  # from components reads the second variable.
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
  include(CPack)
endif()

# --- Developer tools --------------------------------------------------------

# Builds a tile sheet from NetHack's win/share/*.txt files. It is not part of
# the app and must stay out of any installer.
add_executable(tiles2png tools/tiles2png.cpp)
target_link_libraries(tiles2png PRIVATE nethack_core)
if(MSVC)
  target_compile_options(tiles2png PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(tiles2png PRIVATE -Wall -Wextra -Wpedantic)
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
  tests/keychain_test.cpp
  tests/profiles_rust_fixtures_test.cpp
  tests/profiles_test.cpp
  tests/ssh_test.cpp
  tests/statelog_test.cpp
  tests/static_files_test.cpp
  tests/tileset_test.cpp
  tests/tilesrc_test.cpp)
target_link_libraries(core_tests PRIVATE nethack_core nethack_net doctest::doctest_with_main)
if(MSVC)
  target_compile_options(core_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  add_custom_command(TARGET core_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:core_tests> $<TARGET_FILE_DIR:core_tests>
    COMMAND_EXPAND_LISTS)
endif()
add_test(NAME core_tests COMMAND core_tests)
