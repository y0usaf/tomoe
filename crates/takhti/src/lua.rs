//! Embedded Lua config runtime — the compositor's extension surface.
//!
//! Contract: Lua never borrows compositor state. Reads go through a snapshot
//! the core refreshes before every Lua entry; writes are queued as ops
//! (`WindowOp`) or actions and applied when the callback returns. The render
//! loop never waits on config code.
//!
//! All WM policy (workspaces, tiling, focus order) lives in Lua: the default
//! implementation ships as `resources/wm.lua`, preloaded as module `"wm"`,
//! and uses only this public API.

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use anyhow::{Context, Result};
use mlua::{Function, Lua, MetaMethod, RegistryKey, Table, UserData, UserDataMethods, Value};
use tracing::{info, warn};

use crate::input::Action;

const DEFAULT_CONFIG: &str = include_str!("../../../resources/init.lua");
const WM_LUA: &str = include_str!("../../../resources/wm.lua");

/// Which of a connector's advertised modes to use, parsed from
/// `"<preferred|max|WxH>[@<Hz|max>]"` (e.g. "max@max", "2560x1440@144").
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Resolution {
    pub size: SizeSetting,
    pub refresh: RefreshSetting,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SizeSetting {
    /// The monitor's EDID-preferred mode. Beware: some monitors (notably
    /// super-ultrawides) advertise a conservative compatibility mode here,
    /// well below their native resolution and refresh rate.
    #[default]
    Preferred,
    /// Highest resolution by area.
    Max,
    /// Exact width and height in physical pixels.
    Exact(u16, u16),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RefreshSetting {
    /// No `@` part given: the preferred mode's own refresh for
    /// `SizeSetting::Preferred`, otherwise the highest at that size.
    #[default]
    Auto,
    /// Highest refresh rate at the chosen size.
    Max,
    /// Exact rate in millihertz (wl_output units; `@144` → 144000).
    Exact(i32),
}

impl std::str::FromStr for Resolution {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, ()> {
        let (size, refresh) = match s.split_once('@') {
            Some((size, refresh)) => (size, Some(refresh)),
            None => (s, None),
        };
        let size = match size {
            "preferred" => SizeSetting::Preferred,
            "max" => SizeSetting::Max,
            _ => {
                let (w, h) = size.split_once('x').ok_or(())?;
                SizeSetting::Exact(w.parse().map_err(drop)?, h.parse().map_err(drop)?)
            }
        };
        let refresh = match refresh {
            None => RefreshSetting::Auto,
            Some("max") => RefreshSetting::Max,
            Some(hz) => {
                let hz: f64 = hz.parse().map_err(drop)?;
                if !hz.is_finite() || hz <= 0.0 || hz > 10_000.0 {
                    return Err(());
                }
                RefreshSetting::Exact((hz * 1000.0).round() as i32)
            }
        };
        Ok(Resolution { size, refresh })
    }
}

/// Per-output settings: `settings.displays[output_name]` (tty backend).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DisplaySettings {
    pub resolution: Resolution,
}

#[derive(Debug, Clone)]
pub struct Settings {
    /// Gap between windows, in physical pixels (like all Lua geometry).
    pub gaps: i32,
    /// Per-output display settings, keyed by output name ("DP-1").
    /// Unlisted outputs use their EDID-preferred mode.
    pub displays: HashMap<String, DisplaySettings>,
    /// Output scale advertised to clients (snapped to N/120 by the core).
    /// Geometry stays physical at any scale; this only changes how big
    /// clients draw their content.
    pub scale: f64,
    /// Initial size of the nested dev window (winit backend only).
    pub winit_size: (i32, i32),
    /// Border thickness in physical pixels; 1 is one device pixel at any scale.
    pub border_width: i32,
    pub border_focused: [f32; 4],
    pub border_unfocused: [f32; 4],
}

impl Settings {
    /// Configured resolution for the named output, or the default (preferred).
    pub fn resolution_for(&self, output: &str) -> Resolution {
        self.displays
            .get(output)
            .map(|d| d.resolution)
            .unwrap_or_default()
    }
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            gaps: 8,
            displays: HashMap::new(),
            scale: 1.0,
            winit_size: (1280, 800),
            border_width: 2,
            border_focused: parse_color("#7aa2f7").unwrap(),
            border_unfocused: parse_color("#3b4261").unwrap(),
        }
    }
}

/// Parse "#rrggbb" or "#rrggbbaa" into premultiplied RGBA floats.
pub fn parse_color(s: &str) -> Option<[f32; 4]> {
    let hex = s.strip_prefix('#').unwrap_or(s);
    let parse = |i: usize| u8::from_str_radix(hex.get(i..i + 2)?, 16).ok();
    match hex.len() {
        6 => Some([
            parse(0)? as f32 / 255.0,
            parse(2)? as f32 / 255.0,
            parse(4)? as f32 / 255.0,
            1.0,
        ]),
        8 => {
            let a = parse(6)? as f32 / 255.0;
            Some([
                parse(0)? as f32 / 255.0 * a,
                parse(2)? as f32 / 255.0 * a,
                parse(4)? as f32 / 255.0 * a,
                a,
            ])
        }
        _ => None,
    }
}

// ─── Snapshot (Lua-readable state) ────────────────────────────────────────────

#[derive(Debug, Clone, Default)]
pub struct WinProps {
    pub app_id: String,
    pub title: String,
    /// (x, y, w, h) in global *physical pixel* coordinates; None while
    /// unmapped. The whole Lua API speaks physical pixels: integers stay
    /// integers at any output scale, so configs never cause misalignment.
    pub geometry: Option<(i32, i32, i32, i32)>,
    pub mapped: bool,
    pub focused: bool,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct OutputProps {
    pub name: String,
    /// Physical pixels, like all Lua geometry.
    pub geometry: (i32, i32, i32, i32),
    /// Geometry minus layer-shell exclusive zones.
    pub usable: (i32, i32, i32, i32),
}

/// Queued window operations, applied by the core after each Lua entry.
#[derive(Debug, Clone)]
pub enum WindowOp {
    SetGeometry(u64, (i32, i32, i32, i32)),
    Show(u64),
    Hide(u64),
    Focus(u64),
    /// Clear keyboard focus entirely (e.g. switching to an empty workspace).
    ClearFocus,
    Close(u64),
}

pub struct PendingBind {
    pub combo: String,
    pub action: Action,
    /// Human-readable label for the hotkey overlay.
    pub desc: Option<String>,
}

#[derive(Default)]
struct Hooks {
    window_open: Vec<RegistryKey>,
    window_close: Vec<RegistryKey>,
    focus_change: Vec<RegistryKey>,
    outputs_changed: Vec<RegistryKey>,
}

#[derive(Default)]
struct Shared {
    actions: RefCell<Vec<Action>>,
    binds: RefCell<Vec<PendingBind>>,
    bind_fns: RefCell<Vec<RegistryKey>>,
    settings: RefCell<Settings>,
    ops: RefCell<Vec<WindowOp>>,
    windows: RefCell<HashMap<u64, WinProps>>,
    outputs: RefCell<Vec<OutputProps>>,
    hooks: RefCell<Hooks>,
}

// ─── Window userdata ──────────────────────────────────────────────────────────

#[derive(Clone)]
struct LuaWindow {
    id: u64,
    shared: Rc<Shared>,
}

impl LuaWindow {
    fn props(&self) -> WinProps {
        self.shared
            .windows
            .borrow()
            .get(&self.id)
            .cloned()
            .unwrap_or_default()
    }

    fn op(&self, op: WindowOp) {
        self.shared.ops.borrow_mut().push(op);
    }
}

impl UserData for LuaWindow {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("id", |_, this, ()| Ok(this.id));
        methods.add_method("app_id", |_, this, ()| Ok(this.props().app_id));
        methods.add_method("title", |_, this, ()| Ok(this.props().title));
        methods.add_method("is_mapped", |_, this, ()| Ok(this.props().mapped));
        methods.add_method("is_focused", |_, this, ()| Ok(this.props().focused));
        methods.add_method("geometry", |lua, this, ()| {
            match this.props().geometry {
                Some((x, y, w, h)) => {
                    let t = lua.create_table()?;
                    t.set("x", x)?;
                    t.set("y", y)?;
                    t.set("w", w)?;
                    t.set("h", h)?;
                    Ok(Value::Table(t))
                }
                None => Ok(Value::Nil),
            }
        });
        methods.add_method("set_geometry", |_, this, (x, y, w, h): (i32, i32, i32, i32)| {
            this.op(WindowOp::SetGeometry(this.id, (x, y, w.max(1), h.max(1))));
            Ok(())
        });
        methods.add_method("show", |_, this, ()| {
            this.op(WindowOp::Show(this.id));
            Ok(())
        });
        methods.add_method("hide", |_, this, ()| {
            this.op(WindowOp::Hide(this.id));
            Ok(())
        });
        methods.add_method("focus", |_, this, ()| {
            this.op(WindowOp::Focus(this.id));
            Ok(())
        });
        methods.add_method("close", |_, this, ()| {
            this.op(WindowOp::Close(this.id));
            Ok(())
        });
        methods.add_meta_method(MetaMethod::ToString, |_, this, ()| {
            Ok(format!("Window({}, {:?})", this.id, this.props().app_id))
        });
    }
}

// ─── Runtime ──────────────────────────────────────────────────────────────────

pub struct LuaRuntime {
    lua: Lua,
    shared: Rc<Shared>,
}

impl LuaRuntime {
    // mlua::Result because mlua's error is not Send+Sync (single-threaded runtime);
    // the caller converts to anyhow at the boundary.
    pub fn new() -> mlua::Result<Self> {
        let lua = Lua::new();
        let shared = Rc::new(Shared::default());

        let takhti = lua.create_table()?;

        // takhti.settings { gaps = 8, border = {...}, ... }
        let s = shared.clone();
        takhti.set(
            "settings",
            lua.create_function(move |_, table: Table| {
                let mut settings = s.settings.borrow_mut();
                if let Ok(gaps) = table.get::<i32>("gaps") {
                    settings.gaps = gaps;
                }
                if let Ok(displays) = table.get::<Table>("displays") {
                    settings.displays.clear();
                    for pair in displays.pairs::<String, Table>() {
                        let Ok((name, display)) = pair else {
                            warn!("settings.displays: expected {{ [\"DP-1\"] = {{...}} }} entries");
                            continue;
                        };
                        let mut ds = DisplaySettings::default();
                        if let Ok(res) = display.get::<String>("resolution") {
                            match res.parse() {
                                Ok(r) => ds.resolution = r,
                                Err(()) => warn!(
                                    "displays[{name:?}]: invalid resolution {res:?} \
                                     (expected \"preferred\", \"max\", or \"WxH\", \
                                     optionally followed by \"@<Hz|max>\")"
                                ),
                            }
                        }
                        settings.displays.insert(name, ds);
                    }
                }
                if let Ok(scale) = table.get::<f64>("scale") {
                    settings.scale = scale;
                }
                if let Ok(size) = table.get::<Table>("winit_size") {
                    settings.winit_size = (
                        size.get(1).unwrap_or(settings.winit_size.0),
                        size.get(2).unwrap_or(settings.winit_size.1),
                    );
                }
                if let Ok(border) = table.get::<Table>("border") {
                    if let Ok(width) = border.get::<i32>("width") {
                        settings.border_width = width;
                    }
                    if let Ok(color) = border.get::<String>("focused") {
                        match parse_color(&color) {
                            Some(c) => settings.border_focused = c,
                            None => warn!("invalid border.focused color {color:?}"),
                        }
                    }
                    if let Ok(color) = border.get::<String>("unfocused") {
                        match parse_color(&color) {
                            Some(c) => settings.border_unfocused = c,
                            None => warn!("invalid border.unfocused color {color:?}"),
                        }
                    }
                }
                Ok(())
            })?,
        )?;

        // takhti.bind("Alt+Return", fn | "action string" [, "overlay description"])
        let s = shared.clone();
        takhti.set(
            "bind",
            lua.create_function(move |lua, (combo, action, desc): (String, Value, Option<String>)| {
                let action = match action {
                    Value::String(name) => match Action::parse(&name.to_string_lossy()) {
                        Ok(action) => action,
                        Err(err) => {
                            warn!("takhti.bind({combo:?}): {err:#}");
                            return Ok(());
                        }
                    },
                    Value::Function(func) => {
                        let key = lua.create_registry_value(func)?;
                        let mut fns = s.bind_fns.borrow_mut();
                        fns.push(key);
                        Action::LuaFn(fns.len() - 1)
                    }
                    other => {
                        warn!(
                            "takhti.bind({combo:?}): expected string or function, got {}",
                            other.type_name()
                        );
                        return Ok(());
                    }
                };
                s.binds.borrow_mut().push(PendingBind { combo, action, desc });
                Ok(())
            })?,
        )?;

        // takhti.spawn("foot")
        takhti.set(
            "spawn",
            lua.create_function(|_, cmd: String| {
                spawn(&cmd);
                Ok(())
            })?,
        )?;

        // takhti.clear_focus() — drop keyboard focus (no window receives keys)
        let s = shared.clone();
        takhti.set(
            "clear_focus",
            lua.create_function(move |_, ()| {
                s.ops.borrow_mut().push(WindowOp::ClearFocus);
                Ok(())
            })?,
        )?;

        // takhti.quit()
        let s = shared.clone();
        takhti.set(
            "quit",
            lua.create_function(move |_, ()| {
                s.actions.borrow_mut().push(Action::Quit);
                Ok(())
            })?,
        )?;

        // takhti.windows() -> array of window objects
        let s = shared.clone();
        takhti.set(
            "windows",
            lua.create_function(move |_, ()| {
                let mut ids: Vec<u64> = s.windows.borrow().keys().copied().collect();
                ids.sort_unstable();
                Ok(ids
                    .into_iter()
                    .map(|id| LuaWindow {
                        id,
                        shared: s.clone(),
                    })
                    .collect::<Vec<_>>())
            })?,
        )?;

        // takhti.focused_window() -> window | nil
        let s = shared.clone();
        takhti.set(
            "focused_window",
            lua.create_function(move |_, ()| {
                let id = s
                    .windows
                    .borrow()
                    .iter()
                    .find(|(_, props)| props.focused)
                    .map(|(id, _)| *id);
                Ok(id.map(|id| LuaWindow {
                    id,
                    shared: s.clone(),
                }))
            })?,
        )?;

        // takhti.usable_area([output_index]) -> {x, y, w, h}
        let s = shared.clone();
        takhti.set(
            "usable_area",
            lua.create_function(move |lua, idx: Option<usize>| {
                let outputs = s.outputs.borrow();
                let output = outputs.get(idx.map(|i| i.saturating_sub(1)).unwrap_or(0));
                let (x, y, w, h) = output.map(|o| o.usable).unwrap_or((0, 0, 1280, 800));
                let t = lua.create_table()?;
                t.set("x", x)?;
                t.set("y", y)?;
                t.set("w", w)?;
                t.set("h", h)?;
                Ok(t)
            })?,
        )?;

        // takhti.outputs() -> array of {name, x, y, w, h, usable = {...}}
        let s = shared.clone();
        takhti.set(
            "outputs",
            lua.create_function(move |lua, ()| {
                let outputs = s.outputs.borrow();
                let list = lua.create_table()?;
                for (i, o) in outputs.iter().enumerate() {
                    let t = lua.create_table()?;
                    t.set("name", o.name.clone())?;
                    t.set("x", o.geometry.0)?;
                    t.set("y", o.geometry.1)?;
                    t.set("w", o.geometry.2)?;
                    t.set("h", o.geometry.3)?;
                    let u = lua.create_table()?;
                    u.set("x", o.usable.0)?;
                    u.set("y", o.usable.1)?;
                    u.set("w", o.usable.2)?;
                    u.set("h", o.usable.3)?;
                    t.set("usable", u)?;
                    list.set(i + 1, t)?;
                }
                Ok(list)
            })?,
        )?;

        // Event hooks.
        for (name, field) in [
            ("on_window_open", 0usize),
            ("on_window_close", 1),
            ("on_focus_change", 2),
            ("on_outputs_changed", 3),
        ] {
            let s = shared.clone();
            takhti.set(
                name,
                lua.create_function(move |lua, func: Function| {
                    let key = lua.create_registry_value(func)?;
                    let mut hooks = s.hooks.borrow_mut();
                    match field {
                        0 => hooks.window_open.push(key),
                        1 => hooks.window_close.push(key),
                        2 => hooks.focus_change.push(key),
                        _ => hooks.outputs_changed.push(key),
                    }
                    Ok(())
                })?,
            )?;
        }

        lua.globals().set("takhti", takhti)?;

        // Preload the default WM library: `require("wm")` runs it lazily, so
        // configs that ship their own WM never pay for (or fight with) ours.
        let preload: Table = lua
            .globals()
            .get::<Table>("package")?
            .get::<Table>("preload")?;
        preload.set(
            "wm",
            lua.create_function(|lua, _: Value| {
                lua.load(WM_LUA).set_name("wm.lua").eval::<Value>()
            })?,
        )?;

        Ok(Self { lua, shared })
    }

    /// Execute the config at `path` (pre-resolved via `resolve_config_path`),
    /// or the embedded default if None.
    pub fn load(&mut self, path: Option<&Path>) -> Result<()> {
        let (code, name) = match path {
            Some(p) => (
                std::fs::read_to_string(p).with_context(|| format!("error reading {p:?}"))?,
                p.display().to_string(),
            ),
            None => (DEFAULT_CONFIG.to_string(), "<built-in default>".to_string()),
        };
        info!("loading config from {name}");
        self.lua
            .load(&code)
            .set_name(&name)
            .exec()
            .map_err(|err| anyhow::anyhow!("Lua error: {err}"))?;
        Ok(())
    }

    // ── Snapshot sync (called by the core before Lua entries) ──

    /// Replace the snapshot. Returns true if the outputs part changed.
    pub fn sync(&self, windows: HashMap<u64, WinProps>, outputs: Vec<OutputProps>) -> bool {
        *self.shared.windows.borrow_mut() = windows;
        let changed = *self.shared.outputs.borrow() != outputs;
        *self.shared.outputs.borrow_mut() = outputs;
        changed
    }

    pub fn settings(&self) -> Settings {
        self.shared.settings.borrow().clone()
    }

    pub fn take_binds(&mut self) -> Vec<PendingBind> {
        self.shared.binds.take()
    }

    pub fn take_actions(&mut self) -> Vec<Action> {
        self.shared.actions.take()
    }

    pub fn take_ops(&mut self) -> Vec<WindowOp> {
        self.shared.ops.take()
    }

    pub fn has_window_open_hooks(&self) -> bool {
        !self.shared.hooks.borrow().window_open.is_empty()
    }

    pub fn has_window_close_hooks(&self) -> bool {
        !self.shared.hooks.borrow().window_close.is_empty()
    }

    // ── Event emission ──

    pub fn call_bind(&mut self, idx: usize) {
        let key_valid = self.shared.bind_fns.borrow().len() > idx;
        if !key_valid {
            return;
        }
        let func = {
            let fns = self.shared.bind_fns.borrow();
            self.lua.registry_value::<Function>(&fns[idx])
        };
        match func {
            Ok(func) => {
                if let Err(err) = func.call::<()>(()) {
                    warn!("Lua bind error: {err}");
                }
            }
            Err(err) => warn!("Lua registry error: {err}"),
        }
    }

    pub fn emit_window_open(&mut self, id: u64) {
        self.emit_window_event(id, |hooks| &hooks.window_open, "on_window_open");
    }

    pub fn emit_window_close(&mut self, id: u64) {
        self.emit_window_event(id, |hooks| &hooks.window_close, "on_window_close");
    }

    pub fn emit_focus_change(&mut self, id: Option<u64>) {
        let keys: Vec<Function> = {
            let hooks = self.shared.hooks.borrow();
            hooks
                .focus_change
                .iter()
                .filter_map(|k| self.lua.registry_value::<Function>(k).ok())
                .collect()
        };
        let window = id.map(|id| LuaWindow {
            id,
            shared: self.shared.clone(),
        });
        for func in keys {
            if let Err(err) = func.call::<()>(window.clone()) {
                warn!("Lua on_focus_change error: {err}");
            }
        }
    }

    pub fn emit_outputs_changed(&mut self) {
        let keys: Vec<Function> = {
            let hooks = self.shared.hooks.borrow();
            hooks
                .outputs_changed
                .iter()
                .filter_map(|k| self.lua.registry_value::<Function>(k).ok())
                .collect()
        };
        for func in keys {
            if let Err(err) = func.call::<()>(()) {
                warn!("Lua on_outputs_changed error: {err}");
            }
        }
    }

    fn emit_window_event(
        &mut self,
        id: u64,
        select: impl Fn(&Hooks) -> &Vec<RegistryKey>,
        name: &str,
    ) {
        let keys: Vec<Function> = {
            let hooks = self.shared.hooks.borrow();
            select(&hooks)
                .iter()
                .filter_map(|k| self.lua.registry_value::<Function>(k).ok())
                .collect()
        };
        let window = LuaWindow {
            id,
            shared: self.shared.clone(),
        };
        for func in keys {
            if let Err(err) = func.call::<()>(window.clone()) {
                warn!("Lua {name} error: {err}");
            }
        }
    }
}

/// Resolve the config file location: an explicit CLI path wins, else the XDG
/// path if it exists, else None (the embedded default config). Re-resolved on
/// every reload check so creating/removing the user config is itself a change.
pub fn resolve_config_path(cli: Option<&Path>) -> Option<PathBuf> {
    cli.map(PathBuf::from).or_else(|| {
        let base = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
        let candidate = base.join("takhti/init.lua");
        candidate.exists().then_some(candidate)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_resolution() {
        let parse = |s: &str| s.parse::<Resolution>();
        let res = |size, refresh| Resolution { size, refresh };

        assert_eq!(
            parse("preferred"),
            Ok(res(SizeSetting::Preferred, RefreshSetting::Auto))
        );
        assert_eq!(parse("max"), Ok(res(SizeSetting::Max, RefreshSetting::Auto)));
        assert_eq!(
            parse("max@max"),
            Ok(res(SizeSetting::Max, RefreshSetting::Max))
        );
        assert_eq!(
            parse("2560x1440"),
            Ok(res(SizeSetting::Exact(2560, 1440), RefreshSetting::Auto))
        );
        assert_eq!(
            parse("2560x1440@144"),
            Ok(res(SizeSetting::Exact(2560, 1440), RefreshSetting::Exact(144_000)))
        );
        assert_eq!(
            parse("1920x1080@59.94"),
            Ok(res(SizeSetting::Exact(1920, 1080), RefreshSetting::Exact(59_940)))
        );
        assert_eq!(
            parse("preferred@max"),
            Ok(res(SizeSetting::Preferred, RefreshSetting::Max))
        );

        for bad in ["", "1920", "1920x", "x1080", "1920x1080@", "max@", "60@max", "1920x1080@-60"] {
            assert_eq!(parse(bad), Err(()), "{bad:?} should not parse");
        }
    }
}

/// Spawn a command via `sh -c`, detached from the compositor.
pub fn spawn(cmd: &str) {
    let res = std::process::Command::new("/bin/sh")
        .arg("-c")
        .arg(cmd)
        .spawn();
    match res {
        Ok(child) => info!("spawned {cmd:?} (pid {})", child.id()),
        Err(err) => warn!("error spawning {cmd:?}: {err}"),
    }
}
