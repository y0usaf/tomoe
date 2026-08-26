//! Default compositor configuration as a compiled WASM module on the
//! cordis-rs kernel (docs/abi.md + the LOCKED Architecture resolution).
//!
//! tomoe's config migrates from `resources/init.lua`/`tomoe.lua.settings()`
//! (mlua) to **config-as-WASM**: the default configuration is a precompiled
//! `.wasm` module read once at compositor startup and mounted on a
//! `cordis::Context` (the shared spatiotemporal kernel). Config is an
//! extension that only WRITES: its `mount` calls the core set-1 `ctx_set` to
//! populate config keys, and `ctx_read` to declare its reads — the exact
//! proven pattern in cordis-rs `tests/config_wasm.*` (`[[principle:no-
//! privileged-path]]`).
//!
//! The compositor reads the settings it needs back out of the kernel's
//! String/JSON context (`Context::get("tomoe.settings")` deserialized into its
//! own typed `Settings`), routed through cordis string/JSON keys. Unmounting
//! reverts every effect this guest wrote (kernel-owned inverse replay;
//! [[principle:spatiotemporal]]).
//!
//! The default settings JSON is the source of truth for the module bytes
//! (`default_config_wasm`), so config *is* data — a WASM module — with no text
//! config format and no config parser on the host side.

use serde::Serialize;

/// The single config key the compositor reads back from the kernel. Typed
/// data serializes as JSON (docs/abi.md value model).
pub const SETTINGS_KEY: &str = "tomoe.settings";

/// Default compositor settings, mirroring the compositor's `tomoe::lua::Settings`
/// serde shape. Kept here so the *config* (the compiled wasm) owns its own
/// defaults — the host only consumes the JSON over the corda boundary.
#[derive(Debug, Clone, Serialize)]
pub struct DefaultSettings {
    pub gaps: i32,
    pub scale: f64,
    pub winit_size: (i32, i32),
    pub border_width: i32,
    pub corner_radius: i32,
    pub shadow_range: i32,
    pub shadow_power: f64,
    pub blur_enabled: bool,
    pub tearing: bool,
    pub wait_for_frame_completion: bool,
    pub screenshot_freeze: bool,
}

impl Default for DefaultSettings {
    fn default() -> Self {
        Self {
            gaps: 8,
            scale: 1.0,
            winit_size: (1280, 800),
            border_width: 2,
            corner_radius: 0,
            shadow_range: 12,
            shadow_power: 3.0,
            blur_enabled: false,
            tearing: false,
            wait_for_frame_completion: false,
            screenshot_freeze: true,
        }
    }
}

/// The default settings as a flat JSON object (`serde_json::to_value`).
pub fn default_settings_value() -> serde_json::Value {
    serde_json::to_value(DefaultSettings::default())
        .expect("default settings always serialize to a JSON object")
}

/// The default settings JSON — the value the compiled config writes into the
/// cordis context under [`SETTINGS_KEY`].
pub fn default_settings_json() -> String {
    default_settings_value().to_string()
}

/// Build the default config as a compiled WASM module whose `mount` writes the
/// settings JSON into the kernel under [`SETTINGS_KEY`] via the core set-1
/// `ctx_set` host function, and declares the same key with `ctx_read`.
pub fn default_config_wasm() -> Vec<u8> {
    let json = default_settings_json();
    let key = SETTINGS_KEY;
    // Layout one data segment: key then value.
    let key_off = 0;
    let val_off = key.len();
    // Pointer/length args for `ctx_set(keyed,keylen,valptr,vallen)`.
    // The JSON lands in a WAT string literal: escape the quotes so the
    // `data` directive carries the exact bytes (WAT supports `\"`).
    let wat_escape = |s: &str| s.replace('\\', "\\\\").replace('"', "\\\"");
    let wat = format!(
        r#"(module
  (import "host" "ctx_set" (func $ctx_set (param i32 i32 i32 i32)))
  (import "host" "ctx_read" (func $ctx_read (param i32 i32)))
  (memory (export "memory") 2)
  (data (i32.const {key_off}) "{key}")
  (data (i32.const {val_off}) "{json}")

  (func (export "scratch") (result i32 i32)
    i32.const 4096  i32.const 4096)

  (func (export "mount")
    i32.const {key_off} i32.const {key_len}
    i32.const {val_off} i32.const {val_len}
    call $ctx_set
    i32.const {key_off} i32.const {key_len} call $ctx_read)

  (func (export "on_change") (param i32 i32))
)
"#,
        key_off = key_off,
        key_len = key.len(),
        val_off = val_off,
        val_len = json.len(),
        key = wat_escape(key),
        json = wat_escape(&json),
    );
    // Parse the WAT text to a binary module. This is the "compiled config".
    wat::parse_str(&wat).expect("default config wat must be valid")
}
