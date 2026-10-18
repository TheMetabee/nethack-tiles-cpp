//! Runs the Rust app's own profile code, to check the C++ port against it.
//!
//! The Profile types below are copied from nethack-tiles-client
//! src-tauri/src/profiles.rs. The crate versions match that app's lock file;
//! copy its Cargo.lock here before you build, so they stay the same:
//!
//!   cp ../../../nethack-tiles-client/src-tauri/Cargo.lock .
//!   cargo build --offline        # NASM must be on PATH on Windows
//!
//!   compat cases                  the documents in tests/profiles_rust_fixtures_test.cpp
//!   compat read <profiles.toml>   parse a file as the Rust app does
//!   compat key-set|key-get|key-del <service> <user> [password]
//!
//! It is a check, not part of either app, and CMake does not build it.

use serde::{Deserialize, Serialize};
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub enum NetHackVersion { #[default] V36, V50 }
fn default_port() -> u16 {
    22
}
fn default_font_family() -> String {
    "Menlo, DejaVu Sans Mono, Consolas, monospace".to_string()
}
fn default_font_size() -> u32 {
    16
}
fn default_scale() -> f32 {
    1.0
}
fn default_line_height() -> f32 {
    1.0
}
fn default_letter_spacing() -> f32 {
    0.0
}

/// How a profile reaches its game.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Transport {
    /// A public server over SSH, via dgamelaunch.
    #[default]
    Ssh,
    /// A NetHack installed on this machine, in a pseudo-terminal.
    Local,
}

/// One saved server connection plus its display preferences.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    /// Stable id; also the keychain account key.
    pub id: String,
    /// Display name in the picker.
    pub name: String,
    /// Where the game runs. Defaults to SSH so profiles written before local
    /// play existed still load.
    #[serde(default)]
    pub transport: Transport,
    /// The local NetHack to run, for [`Transport::Local`]. Empty means "look
    /// for one at connect time", so a profile does not go stale when NetHack
    /// is upgraded or moved.
    #[serde(default)]
    pub command: String,
    pub host: String,
    #[serde(default = "default_port")]
    pub port: u16,
    /// The shared SSH user the server publishes, e.g. `nethack`.
    pub ssh_user: String,
    /// The dgamelaunch account name typed at the in-terminal prompt.
    #[serde(default)]
    pub game_user: String,
    /// NetHack release the server runs; selects the glyph flag layout.
    #[serde(default)]
    pub version: NetHackVersion,
    /// Id of the tileset to render with.
    pub tileset_id: String,
    #[serde(default = "default_font_family")]
    pub font_family: String,
    #[serde(default = "default_font_size")]
    pub font_size: u32,
    #[serde(default = "default_scale")]
    pub scale: f32,
    /// Cell height as a multiple of the font size. A tile is drawn to fill its
    /// terminal cell, so this is how tall the tiles are.
    #[serde(default = "default_line_height")]
    pub line_height: f32,
    /// Extra pixels of cell width. Terminal cells are far narrower than they
    /// are tall, which squashes a square tile; this is how the player widens
    /// them back out.
    #[serde(default = "default_letter_spacing")]
    pub letter_spacing: f32,
    /// Draw tiles at a whole-number multiple of their native pixel size,
    /// centred in the cell, rather than stretched to fill it.
    #[serde(default)]
    pub pixel_perfect: bool,
    /// Write live game-state snapshots for an LLM to read.
    #[serde(default)]
    pub state_log_enabled: bool,
    /// Directory those snapshots go in. Empty means "not set".
    #[serde(default)]
    pub state_log_directory: String,
}

impl Profile {
    /// A new profile with sensible defaults for `name`.
    pub fn new(id: impl Into<String>, name: impl Into<String>) -> Self {
        Profile {
            id: id.into(),
            name: name.into(),
            transport: Transport::default(),
            command: String::new(),
            host: String::new(),
            port: default_port(),
            ssh_user: String::new(),
            game_user: String::new(),
            version: NetHackVersion::default(),
            tileset_id: String::new(),
            font_family: default_font_family(),
            font_size: default_font_size(),
            scale: default_scale(),
            line_height: default_line_height(),
            letter_spacing: default_letter_spacing(),
            pixel_perfect: false,
            state_log_enabled: false,
            state_log_directory: String::new(),
        }
    }
}

/// The on-disk document.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileDocument {
    #[serde(default)]
    pub profiles: Vec<Profile>,
    /// Id of the profile to preselect on launch.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_used: Option<String>,
}


use std::io::Read;
fn doc(last: Option<&str>, profiles: Vec<Profile>) -> ProfileDocument { ProfileDocument { profiles, last_used: last.map(String::from) } }
fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args[1].as_str() {
        // Prints each case as: name, the document as JSON, and the TOML the Rust app writes.
        "cases" => {
            let mut tricky = Profile::new("tricky", "Ivan's \"server\"");
            tricky.host = "example.org".into();
            tricky.ssh_user = "nethack".into();
            tricky.tileset_id = "t".into();
            tricky.state_log_directory = r"C:\Users\username\NetHack state".into();
            tricky.command = "tab\there".into();
            tricky.game_user = "\u{e9}t\u{e9} \u{1F409}".into();
            tricky.font_family = "a'b".into();
            let mut floats = Profile::new("floats", "Floats");
            floats.host = "h".into(); floats.ssh_user = "u".into(); floats.tileset_id = "t".into();
            floats.scale = 0.1 + 0.2; floats.line_height = 1.4; floats.letter_spacing = 1234567.0;
            let mut tiny = Profile::new("tiny", "Tiny");
            tiny.host = "h".into(); tiny.ssh_user = "u".into(); tiny.tileset_id = "t".into();
            tiny.scale = 0.0000001; tiny.line_height = 3.0e10; tiny.letter_spacing = -2.5;
            let mut quotes = Profile::new("quotes", "both ' and \" here");
            quotes.host = "h".into(); quotes.ssh_user = "u".into(); quotes.tileset_id = "t".into();
            quotes.command = r"back\slash and 'single'".into();
            quotes.game_user = "line one\nline two".into();
            quotes.font_family = "bell\u{7}".into();
            let cases = vec![
                ("empty", doc(None, vec![])),
                ("empty-with-last", doc(Some("x"), vec![])),
                ("no-last", doc(None, vec![floats.clone()])),
                ("tricky", doc(Some("tricky"), vec![tricky])),
                ("floats", doc(None, vec![floats, tiny])),
                ("quotes", doc(None, vec![quotes])),
            ];
            let out: Vec<_> = cases.into_iter().map(|(name, d)| serde_json::json!({
                "name": name,
                "toml": toml::to_string_pretty(&d).unwrap(),
                "doc": d,
            })).collect();
            println!("{}", serde_json::to_string_pretty(&out).unwrap());
        }
        // Reads a TOML file and prints it back as JSON, or the error.
        "read" => {
            let mut text = String::new();
            std::fs::File::open(&args[2]).unwrap().read_to_string(&mut text).unwrap();
            match toml::from_str::<ProfileDocument>(&text) {
                Ok(d) => println!("{}", serde_json::to_string(&d).unwrap()),
                Err(e) => { println!("ERROR {e}"); std::process::exit(1); }
            }
        }
        "key-set" => { keyring::Entry::new(&args[2], &args[3]).unwrap().set_password(&args[4]).unwrap(); println!("set"); }
        "key-get" => match keyring::Entry::new(&args[2], &args[3]).unwrap().get_password() {
            Ok(p) => println!("password {:?}", p),
            Err(e) => println!("ERROR {e}"),
        },
        "key-del" => { let _ = keyring::Entry::new(&args[2], &args[3]).unwrap().delete_credential(); println!("deleted"); }
        _ => panic!("unknown mode"),
    }
}
