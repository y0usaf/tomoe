//! `shell.state` — the reactive primitive (nur's `LuaState`, GPUI-free).
//!
//! A state is an `Rc`-shared value cell. `state:set(v)`:
//! 1. stores the value,
//! 2. fires per-state subscribers (registered via `state:subscribe(fn)`),
//! 3. marks the shell dirty ([`ShellCtx::mark_dirty`]) — the notify-all
//!    model: every window repaints on the next loop pass, and windows
//!    whose element tree came out identical early-out in the scene diff.
//!
//! Subscribers are stored as `LuaRegistryKey` (nur's standing lesson:
//! never store a `LuaFunction` for callbacks that outlive the stack
//! frame) and reached through a [`WeakLua`] so a subscriber closure
//! held inside the VM can never keep the VM alive across a hot reload.

use std::cell::RefCell;
use std::rc::Rc;

use mlua::prelude::*;

use crate::api::ShellCtx;

type Notifier = Rc<dyn Fn()>;

struct StateInner {
    value: LuaValue,
    notifiers: Vec<Notifier>,
}

/// The userdata behind `shell.state(initial)`.
pub struct LuaState {
    inner: Rc<RefCell<StateInner>>,
    ctx: Rc<ShellCtx>,
}

impl LuaState {
    pub fn new(value: LuaValue, ctx: Rc<ShellCtx>) -> Self {
        Self {
            inner: Rc::new(RefCell::new(StateInner {
                value,
                notifiers: Vec::new(),
            })),
            ctx,
        }
    }

    fn get(&self) -> LuaValue {
        self.inner.borrow().value.clone()
    }

    fn set(&self, value: LuaValue) {
        self.inner.borrow_mut().value = value;
        // Rc snapshot so the RefCell is released while subscribers run
        // (a subscriber may read or set this very state).
        let notifiers: Vec<Notifier> = self.inner.borrow().notifiers.clone();
        for n in notifiers {
            n();
        }
        self.ctx.mark_dirty();
    }
}

impl LuaUserData for LuaState {
    fn add_methods<M: LuaUserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("get", |_lua, this, ()| Ok(this.get()));

        methods.add_method("set", |_lua, this, value: LuaValue| {
            this.set(value);
            Ok(())
        });

        // state:map(fn) — transform the current value.
        methods.add_method("map", |_lua, this, transform: LuaFunction| {
            transform.call::<LuaValue>(this.get())
        });

        // state:subscribe(fn) — fn() is called (no arguments, nur's
        // contract) after every set.
        methods.add_method("subscribe", |lua, this, callback: LuaFunction| {
            let key = lua.create_registry_value(callback)?;
            let weak = lua.weak();
            this.inner.borrow_mut().notifiers.push(Rc::new(move || {
                let Some(lua) = weak.try_upgrade() else {
                    return; // VM already dropped (reload in flight)
                };
                match lua.registry_value::<LuaFunction>(&key) {
                    Ok(f) => {
                        if let Err(e) = f.call::<()>(()) {
                            tracing::error!("state subscriber error: {e}");
                        }
                    }
                    Err(e) => tracing::error!("state subscriber registry lookup failed: {e}"),
                }
            }));
            Ok(())
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Vm;

    fn vm() -> Vm {
        let vm = Vm::new().unwrap();
        vm.install_shell(&ShellCtx::new()).unwrap();
        vm
    }

    #[test]
    fn get_set_roundtrip_through_lua() {
        let vm = vm();
        vm.exec(
            r#"
            local s = shell.state("initial")
            before = s:get()
            s:set("changed")
            after = s:get()
            "#,
            "t.lua",
        )
        .unwrap();
        let g = vm.lua().globals();
        assert_eq!(g.get::<String>("before").unwrap(), "initial");
        assert_eq!(g.get::<String>("after").unwrap(), "changed");
    }

    #[test]
    fn subscribe_fires_on_every_set() {
        let vm = vm();
        vm.exec(
            r#"
            hits = 0
            local s = shell.state(0)
            s:subscribe(function() hits = hits + 1 end)
            s:set(1)
            s:set(2)
            "#,
            "t.lua",
        )
        .unwrap();
        assert_eq!(vm.lua().globals().get::<i64>("hits").unwrap(), 2);
    }

    #[test]
    fn subscriber_reads_the_new_value() {
        let vm = vm();
        vm.exec(
            r#"
            local s = shell.state("a")
            s:subscribe(function() seen = s:get() end)
            s:set("b")
            "#,
            "t.lua",
        )
        .unwrap();
        assert_eq!(vm.lua().globals().get::<String>("seen").unwrap(), "b");
    }

    #[test]
    fn map_transforms_without_mutating() {
        let vm = vm();
        vm.exec(
            r#"
            local s = shell.state(21)
            doubled = s:map(function(v) return v * 2 end)
            kept = s:get()
            "#,
            "t.lua",
        )
        .unwrap();
        let g = vm.lua().globals();
        assert_eq!(g.get::<i64>("doubled").unwrap(), 42);
        assert_eq!(g.get::<i64>("kept").unwrap(), 21);
    }

    #[test]
    fn ui_text_unwraps_state_userdata() {
        // The stdlib's ui.text auto-reads reactive state.
        let vm = vm();
        vm.exec(
            r#"
            local s = shell.state("12:34")
            el = ui.text(s)
            "#,
            "t.lua",
        )
        .unwrap();
        let el: LuaTable = vm.lua().globals().get("el").unwrap();
        assert_eq!(el.get::<String>("content").unwrap(), "12:34");
    }
}
