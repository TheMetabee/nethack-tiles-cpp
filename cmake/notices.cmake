# Writes THIRD_PARTY_NOTICES.txt into the build folder: the name, version,
# home and full licence text of everything the app ships. The package
# carries it beside the app's own LICENSE.txt.
#
# Included from CMakeLists.txt after the dependencies are fetched, so their
# licence files are on disk. Only what reaches a player is listed: doctest,
# for one, is in no package.

set(NH_NOTICES "Third-party software in NetHack Tiles Client\n")
string(APPEND NH_NOTICES "============================================\n\n")
string(APPEND NH_NOTICES "The app's own licence is in LICENSE.txt. It includes the parts below,\n")
string(APPEND NH_NOTICES "each under its own licence, which follows its name.\n\n")
string(APPEND NH_NOTICES "libssh is a separate library (ssh.dll) under the LGPL-2.1: you may\n")
string(APPEND NH_NOTICES "replace it with your own build of libssh 0.12.2. Its source is at\n")
string(APPEND NH_NOTICES "https://www.libssh.org/, and the changes this app makes to it are in\n")
string(APPEND NH_NOTICES "cmake/patches/libssh-fixes.cmake in this app's source.\n\n")

# Adds one part: its name, version, home page, licence name and licence file.
function(nh_notice name version home licence file)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "The licence file of ${name} is missing: ${file}")
  endif()
  file(READ "${file}" text)
  string(APPEND NH_NOTICES
    "------------------------------------------------------------------------\n"
    "${name} ${version}\n${home}\nLicence: ${licence}\n"
    "------------------------------------------------------------------------\n\n"
    "${text}\n\n")
  set(NH_NOTICES "${NH_NOTICES}" PARENT_SCOPE)
endfunction()

# Adds a package from the frontend bundle, with the version installed.
function(nh_npm_notice dir name home)
  set(root "${dir}/node_modules/${name}")
  if(NOT EXISTS "${root}")
    # pnpm keeps a dependency of a dependency here, not at the top.
    set(root "${dir}/node_modules/.pnpm/node_modules/${name}")
  endif()
  file(READ "${root}/package.json" manifest)
  string(JSON version GET "${manifest}" version)
  string(JSON licence GET "${manifest}" license)
  nh_notice("${name}" "${version}" "${home}" "${licence}" "${root}/LICENSE")
  set(NH_NOTICES "${NH_NOTICES}" PARENT_SCOPE)
endfunction()

nh_notice("libssh" "0.12.2" "https://www.libssh.org/" "LGPL-2.1-or-later"
  "${libssh_SOURCE_DIR}/COPYING")
nh_notice("Mbed TLS" "3.6.7" "https://www.trustedfirmware.org/projects/mbed-tls/"
  "Apache-2.0 (dual-licensed with GPL-2.0-or-later; this app takes Apache-2.0)"
  "${mbedtls_SOURCE_DIR}/LICENSE")
nh_notice("webview" "0.12.0" "https://github.com/webview/webview" "MIT"
  "${webview_SOURCE_DIR}/LICENSE")
if(WIN32)
  # webview fetches the SDK inside its own scope, where the source folder's
  # variable stays, so take the place FetchContent puts every source.
  nh_notice("Microsoft Edge WebView2 SDK" "${WEBVIEW_MSWEBVIEW2_VERSION}"
    "https://www.nuget.org/packages/Microsoft.Web.WebView2" "BSD-3-Clause"
    "${FETCHCONTENT_BASE_DIR}/microsoft_web_webview2-src/LICENSE.txt")
endif()
nh_notice("JSON for Modern C++" "3.11.3" "https://github.com/nlohmann/json" "MIT"
  "${json_SOURCE_DIR}/LICENSE.MIT")
nh_notice("toml++" "3.4.0" "https://github.com/marzer/tomlplusplus" "MIT"
  "${tomlplusplus_SOURCE_DIR}/LICENSE")
nh_notice("stb_image and stb_image_write" "2c980bb" "https://github.com/nothings/stb"
  "MIT or public domain" "${stb_SOURCE_DIR}/LICENSE")

if(NH_BUILD_FRONTEND)
  set(frontend "${CMAKE_SOURCE_DIR}/frontend")
  nh_npm_notice("${frontend}" react "https://react.dev/")
  nh_npm_notice("${frontend}" react-dom "https://react.dev/")
  nh_npm_notice("${frontend}" scheduler "https://react.dev/")
  nh_npm_notice("${frontend}" @xterm/xterm "https://xtermjs.org/")
  nh_npm_notice("${frontend}" @xterm/addon-fit "https://xtermjs.org/")
  nh_npm_notice("${frontend}" @xterm/addon-webgl "https://xtermjs.org/")
  nh_notice("PT Mono" "" "https://www.paratype.com/fonts/pt/pt-mono" "OFL-1.1"
    "${frontend}/src/fonts/PTMono-OFL.txt")
endif()

file(WRITE "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt" "${NH_NOTICES}")
