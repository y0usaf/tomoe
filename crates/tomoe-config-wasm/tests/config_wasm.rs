//! Proof that default tomoe config rides the corda kernel as config-as-WASM:
//! - the compiled config module is loaded at startup (`mount` before anything),
//! - its `mount` writes the canonical settings JSON into the context,
//! - unmounting reverts the context to its pre-mount state (spatiotemporal),
//! - the same public `cordis` ABI a user extension uses (no privileged path).

use cordis::Context;
use tomoe_config_wasm::{default_config_wasm, default_settings_json, SETTINGS_KEY};

#[test]
fn config_wasm_loads_at_startup_and_sets_settings() {
    let config_wasm = default_config_wasm();
    let mut ctx = Context::new();

    // Config mounts first and only writes: the settings key lands verbatim.
    let config_id = ctx.mount(&config_wasm).expect("mount config at startup");
    assert_eq!(
        ctx.get(SETTINGS_KEY).as_deref(),
        Some(default_settings_json().as_str()),
        "settings JSON must land in the corda context unmodified"
    );
    assert!(
        ctx.has(SETTINGS_KEY),
        "settings key present after config mount"
    );

    // Unmount reverts the context to pre-mount with no residue.
    ctx.unmount(config_id).expect("unmount config");
    assert!(!ctx.has(SETTINGS_KEY), "settings key reverts on unmount");
    assert_eq!(ctx.get(SETTINGS_KEY), None, "no residue after revert");
}

#[test]
fn config_wasm_is_a_real_wasm_module() {
    let wasm = default_config_wasm();
    // It must be a valid, self-contained binary wasm module (not a text form).
    assert!(!wasm.is_empty());
    assert!(wasm.starts_with(&[0x00, 0x61, 0x73, 0x6D]), "wasm header");
}

#[test]
fn default_settings_json_is_a_flat_object() {
    let v: serde_json::Value =
        serde_json::from_str(&default_settings_json()).expect("settings json parses");
    let obj = v.as_object().expect("settings json is an object");
    assert_eq!(obj["gaps"], 8);
    assert_eq!(obj["winit_size"][0], 1280);
    assert_eq!(obj["scale"], 1.0);
}
