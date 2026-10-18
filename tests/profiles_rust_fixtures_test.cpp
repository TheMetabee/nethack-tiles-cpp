// Fixtures captured from the Rust app's own code: its Profile struct and
// `toml::to_string_pretty` from toml 1.1.4, run on these documents. The C++
// writer must give the same bytes, and the C++ reader the same document.
//
// Regenerate them with tools/rust-compat (`compat cases`); do not
// edit the TOML by hand.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "core/profiles.hpp"

using namespace nh;

namespace {

struct RustCase {
  const char* name;
  const char* json;  // the document, as serde_json wrote it
  const char* toml;  // the file, as the Rust app writes it
};

const RustCase kCases[] = {
    {"empty",
     R"fixture({"profiles": []})fixture",
     R"fixture(profiles = []
)fixture"},
    {"empty-with-last",
     R"fixture({"lastUsed": "x", "profiles": []})fixture",
     R"fixture(profiles = []
lastUsed = "x"
)fixture"},
    {"no-last",
     R"fixture({"profiles": [{"command": "", "fontFamily": "Menlo, DejaVu Sans Mono, Consolas, monospace", "fontSize": 16, "gameUser": "", "host": "h", "id": "floats", "letterSpacing": 1234567.0, "lineHeight": 1.399999976158142, "name": "Floats", "pixelPerfect": false, "port": 22, "scale": 0.30000001192092896, "sshUser": "u", "stateLogDirectory": "", "stateLogEnabled": false, "tilesetId": "t", "transport": "ssh", "version": "v36"}]})fixture",
     R"fixture([[profiles]]
id = "floats"
name = "Floats"
transport = "ssh"
command = ""
host = "h"
port = 22
sshUser = "u"
gameUser = ""
version = "v36"
tilesetId = "t"
fontFamily = "Menlo, DejaVu Sans Mono, Consolas, monospace"
fontSize = 16
scale = 0.3
lineHeight = 1.4
letterSpacing = 1234567.0
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = ""
)fixture"},
    {"tricky",
     R"fixture({"lastUsed": "tricky", "profiles": [{"command": "tab\there", "fontFamily": "a'b", "fontSize": 16, "gameUser": "été 🐉", "host": "example.org", "id": "tricky", "letterSpacing": 0.0, "lineHeight": 1.0, "name": "Ivan's \"server\"", "pixelPerfect": false, "port": 22, "scale": 1.0, "sshUser": "nethack", "stateLogDirectory": "C:\\Users\\username\\NetHack state", "stateLogEnabled": false, "tilesetId": "t", "transport": "ssh", "version": "v36"}]})fixture",
     R"fixture(lastUsed = "tricky"

[[profiles]]
id = "tricky"
name = """Ivan's "server""""
transport = "ssh"
command = "tab\there"
host = "example.org"
port = 22
sshUser = "nethack"
gameUser = "été 🐉"
version = "v36"
tilesetId = "t"
fontFamily = "a'b"
fontSize = 16
scale = 1.0
lineHeight = 1.0
letterSpacing = 0.0
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = 'C:\Users\username\NetHack state'
)fixture"},
    {"floats",
     R"fixture({"profiles": [{"command": "", "fontFamily": "Menlo, DejaVu Sans Mono, Consolas, monospace", "fontSize": 16, "gameUser": "", "host": "h", "id": "floats", "letterSpacing": 1234567.0, "lineHeight": 1.399999976158142, "name": "Floats", "pixelPerfect": false, "port": 22, "scale": 0.30000001192092896, "sshUser": "u", "stateLogDirectory": "", "stateLogEnabled": false, "tilesetId": "t", "transport": "ssh", "version": "v36"}, {"command": "", "fontFamily": "Menlo, DejaVu Sans Mono, Consolas, monospace", "fontSize": 16, "gameUser": "", "host": "h", "id": "tiny", "letterSpacing": -2.5, "lineHeight": 30000001024.0, "name": "Tiny", "pixelPerfect": false, "port": 22, "scale": 1.0000000116860974e-07, "sshUser": "u", "stateLogDirectory": "", "stateLogEnabled": false, "tilesetId": "t", "transport": "ssh", "version": "v36"}]})fixture",
     R"fixture([[profiles]]
id = "floats"
name = "Floats"
transport = "ssh"
command = ""
host = "h"
port = 22
sshUser = "u"
gameUser = ""
version = "v36"
tilesetId = "t"
fontFamily = "Menlo, DejaVu Sans Mono, Consolas, monospace"
fontSize = 16
scale = 0.3
lineHeight = 1.4
letterSpacing = 1234567.0
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = ""

[[profiles]]
id = "tiny"
name = "Tiny"
transport = "ssh"
command = ""
host = "h"
port = 22
sshUser = "u"
gameUser = ""
version = "v36"
tilesetId = "t"
fontFamily = "Menlo, DejaVu Sans Mono, Consolas, monospace"
fontSize = 16
scale = 0.0000001
lineHeight = 30000000000.0
letterSpacing = -2.5
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = ""
)fixture"},
    {"quotes",
     R"fixture({"profiles": [{"command": "back\\slash and 'single'", "fontFamily": "bell\u0007", "fontSize": 16, "gameUser": "line one\nline two", "host": "h", "id": "quotes", "letterSpacing": 0.0, "lineHeight": 1.0, "name": "both ' and \" here", "pixelPerfect": false, "port": 22, "scale": 1.0, "sshUser": "u", "stateLogDirectory": "", "stateLogEnabled": false, "tilesetId": "t", "transport": "ssh", "version": "v36"}]})fixture",
     R"fixture([[profiles]]
id = "quotes"
name = """both ' and " here"""
transport = "ssh"
command = '''back\slash and 'single''''
host = "h"
port = 22
sshUser = "u"
gameUser = """
line one
line two"""
version = "v36"
tilesetId = "t"
fontFamily = "bell\u0007"
fontSize = 16
scale = 1.0
lineHeight = 1.0
letterSpacing = 0.0
pixelPerfect = false
stateLogEnabled = false
stateLogDirectory = ""
)fixture"},
};

ProfileDocument document(const char* text) {
  const auto j = nlohmann::json::parse(text);
  ProfileDocument doc;
  for (const auto& p : j.at("profiles")) doc.profiles.push_back(p.get<Profile>());
  if (j.contains("lastUsed") && !j.at("lastUsed").is_null()) {
    doc.last_used = j.at("lastUsed").get<std::string>();
  }
  return doc;
}

}  // namespace

TEST_CASE("each document is written as the rust app writes it") {
  for (const auto& c : kCases) {
    INFO(std::string(c.name));
    CHECK(format_profiles(document(c.json)) == std::string(c.toml));
  }
}

TEST_CASE("each file the rust app writes reads back as its document") {
  for (const auto& c : kCases) {
    INFO(std::string(c.name));
    CHECK(parse_profiles(c.toml, c.name) == document(c.json));
  }
}
