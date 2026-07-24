//! Push native service snapshots into the Lua facades.
//!
//! The one place `services` state crosses into Lua: `services` itself
//! stays Lua-free (one-way arrow), and the Lua-facing shape —
//! `shell.services.<name>` with `:get/:set/:map/:subscribe` — is
//! declared once in `resources/moonshell/services.lua` (doctrine 05).
//! Backends replace the *backing* of those facades by calling `:set`
//! with a fresh snapshot table; every widget subscription and dirty
//! mark then rides the ordinary `shell.state` path.
//!
//! Absent facades are an error to the caller: the binary logs at
//! debug (a reload mid-flight or a config that nuked `shell` is not
//! worth a crash — the next snapshot retries).

use mlua::prelude::*;
use moonshell_services::battery::BatteryState;
use moonshell_services::compositor::CompositorState;
use moonshell_services::mpris::MprisState;
use moonshell_services::network::NetworkState;

/// Push compositor state plus keyboard activity into Lua facades.
pub fn push_compositor(lua: &Lua, state: &CompositorState) -> LuaResult<()> {
    let t = lua.create_table()?;
    t.set("connected", state.connected)?;
    t.set("active_workspace", state.active_workspace)?;
    let ws = lua.create_table()?;
    for (i, w) in state.workspaces.iter().enumerate() {
        let wt = lua.create_table()?;
        wt.set("id", w.id)?;
        wt.set("name", w.name.as_str())?;
        wt.set("active", w.active)?;
        wt.set("windows", w.windows)?;
        ws.set(i + 1, wt)?;
    }
    t.set("workspaces", ws)?;
    if let Some(title) = &state.active_window {
        t.set("active_window", title.as_str())?;
    }
    set_service(lua, "compositor", t)?;

    let keyboard = lua.create_table()?;
    if let Some(activity) = state.keyboard_activity {
        keyboard.set("sequence", activity.sequence)?;
        keyboard.set("hand", activity.hand.as_str())?;
    } else {
        keyboard.set("sequence", 0u64)?;
        keyboard.set("hand", "right")?;
    }
    set_service(lua, "keyboard", keyboard)
}

/// `shell.services.battery:set(snapshot)`.
pub fn push_battery(lua: &Lua, state: &BatteryState) -> LuaResult<()> {
    let t = lua.create_table()?;
    t.set("available", state.available)?;
    t.set("percent", state.percent)?;
    t.set("charging", state.charging)?;
    set_service(lua, "battery", t)
}

/// `shell.services.network:set(snapshot)`.
pub fn push_network(lua: &Lua, state: &NetworkState) -> LuaResult<()> {
    let t = lua.create_table()?;
    t.set("connected", state.connected)?;
    if let Some(ssid) = &state.ssid {
        t.set("ssid", ssid.as_str())?;
    }
    t.set("strength", state.strength)?;
    set_service(lua, "network", t)
}

/// `shell.services.mpris:set(snapshot)`.
pub fn push_mpris(lua: &Lua, state: &MprisState) -> LuaResult<()> {
    let t = lua.create_table()?;
    t.set("player_name", state.player_name.as_str())?;
    t.set("status", state.status.as_str())?;
    t.set("title", state.title.as_str())?;
    t.set("artist", state.artist.as_str())?;
    t.set("album", state.album.as_str())?;
    t.set("art_url", state.art_url.as_str())?;
    t.set("length", state.length)?;
    t.set("position", state.position)?;
    t.set("volume", state.volume)?;
    set_service(lua, "mpris", t)
}

fn set_service(lua: &Lua, name: &str, snapshot: LuaTable) -> LuaResult<()> {
    let shell: LuaTable = lua.globals().get("shell")?;
    let services: LuaTable = shell.get("services")?;
    let facade: LuaTable = services.get(name)?;
    let set: LuaFunction = facade.get("set")?;
    set.call((facade, snapshot))
}
