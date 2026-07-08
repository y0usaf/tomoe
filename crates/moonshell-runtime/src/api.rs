//! The `shell.*` Lua API and the action queue between Lua and the
//! binary's event loop.
//!
//! Lua never touches `surface::Shell` directly (it is the calloop
//! dispatch data — no stable `&mut` exists while Lua runs). Instead,
//! `shell.window(...)` queues a [`PendingWindow`] and `state:set(...)`
//! raises the dirty flag; the binary drains both after every Lua entry
//! point (config exec now; timers and hot reload in M2 §4–5) while it
//! *does* hold `&mut Shell`. Snapshot in, actions out — the doctrine-02
//! shape, even though Lua is the application here.
//!
//! Registered by [`crate::Vm::install_shell`]:
//! - `shell.window(opts) -> handle` — queue a layer-surface window;
//!   `handle:render(fn)` attaches the render callback.
//! - `shell.state(initial) -> state` — reactive value; `:get()`,
//!   `:set(v)`, `:map(fn)`, `:subscribe(fn)`.
//!
//! `shell.interval`/`once`/`exec`/`quit`/`get_window`/`displays` are
//! M2 §4; `shell.reload`/`watch_file` are M2 §5 (tracked in PLAN.md).

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use mlua::prelude::*;
use moonshell_surface::LayerOptions;

use crate::state::LuaState;
use crate::window::{self, WindowHandle, WindowShared};

/// A `shell.window` call waiting for the binary to create the actual
/// layer surface (with a `LuaPainter` wired to `shared`).
pub struct PendingWindow {
    pub options: LayerOptions,
    /// Render callback + window paint defaults, shared with the
    /// [`WindowHandle`] Lua already holds.
    pub shared: Rc<RefCell<WindowShared>>,
}

/// The Lua↔loop bridge state: queued actions and the notify-all dirty
/// flag. One per [`crate::Vm`]; both sides hold it by `Rc`.
#[derive(Default)]
pub struct ShellCtx {
    pending: RefCell<Vec<PendingWindow>>,
    dirty: Cell<bool>,
}

impl ShellCtx {
    pub fn new() -> Rc<Self> {
        Rc::new(Self::default())
    }

    /// Something visible changed (state set, render fn attached) —
    /// every window repaints on the next drain. Unchanged element
    /// trees early-out in the scene diff, so notify-all stays cheap.
    pub fn mark_dirty(&self) {
        self.dirty.set(true);
    }

    /// Take-and-clear the dirty flag.
    pub fn take_dirty(&self) -> bool {
        self.dirty.replace(false)
    }

    /// Drain queued window creations.
    pub fn take_pending(&self) -> Vec<PendingWindow> {
        self.pending.take()
    }
}

/// Register the `shell` global. See the module docs for the surface.
pub fn register_shell(lua: &Lua, ctx: &Rc<ShellCtx>) -> LuaResult<()> {
    let shell = lua.create_table()?;

    let c = ctx.clone();
    shell.set(
        "window",
        lua.create_function(move |_lua, config: LuaTable| {
            let opts = window::parse_options(&config)?;
            let shared = Rc::new(RefCell::new(WindowShared {
                render_key: None,
                bg: opts.bg,
                text: opts.text,
            }));
            c.pending.borrow_mut().push(PendingWindow {
                options: opts.layer,
                shared: shared.clone(),
            });
            c.mark_dirty();
            Ok(WindowHandle::new(shared, c.clone()))
        })?,
    )?;

    let c = ctx.clone();
    shell.set(
        "state",
        lua.create_function(move |_lua, initial: LuaValue| Ok(LuaState::new(initial, c.clone())))?,
    )?;

    lua.globals().set("shell", shell)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Vm;

    fn vm_with_shell() -> (Vm, Rc<ShellCtx>) {
        let vm = Vm::new().unwrap();
        let ctx = ShellCtx::new();
        vm.install_shell(&ctx).unwrap();
        (vm, ctx)
    }

    #[test]
    fn window_queues_pending_and_marks_dirty() {
        let (vm, ctx) = vm_with_shell();
        vm.exec("w = shell.window({ height = 24 })", "t.lua")
            .unwrap();
        assert!(ctx.take_dirty());
        let pending = ctx.take_pending();
        assert_eq!(pending.len(), 1);
        assert_eq!(pending[0].options.height, 24);
        assert!(pending[0].shared.borrow().render_key.is_none());
    }

    #[test]
    fn render_attaches_callback_and_marks_dirty() {
        let (vm, ctx) = vm_with_shell();
        vm.exec("w = shell.window({})", "t.lua").unwrap();
        let pending = ctx.take_pending();
        ctx.take_dirty();
        vm.exec("w:render(function() return ui.hbox({}) end)", "t.lua")
            .unwrap();
        assert!(ctx.take_dirty());
        assert!(pending[0].shared.borrow().render_key.is_some());
    }

    #[test]
    fn take_pending_drains() {
        let (vm, ctx) = vm_with_shell();
        vm.exec("shell.window({}); shell.window({})", "t.lua")
            .unwrap();
        assert_eq!(ctx.take_pending().len(), 2);
        assert!(ctx.take_pending().is_empty());
    }

    #[test]
    fn state_set_marks_dirty() {
        let (vm, ctx) = vm_with_shell();
        vm.exec("s = shell.state(1)", "t.lua").unwrap();
        ctx.take_dirty(); // creation does not dirty; only set does
        assert!(!ctx.take_dirty());
        vm.exec("s:set(2)", "t.lua").unwrap();
        assert!(ctx.take_dirty());
        vm.exec("v = s:get()", "t.lua").unwrap();
        let v: i64 = vm.lua().globals().get("v").unwrap();
        assert_eq!(v, 2);
    }
}
