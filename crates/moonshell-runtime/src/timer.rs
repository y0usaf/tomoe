//! `shell.interval` / `shell.once` — timers queued for the binary to
//! arm as calloop sources.
//!
//! Lua can't insert event-loop sources itself (no stable loop handle
//! exists while Lua runs), so both calls push a [`PendingTimer`] onto
//! the [`crate::ShellCtx`] queue; the binary's drain inserts a calloop
//! `Timer` whose callback is [`PendingTimer::fire`]. Timers are armed
//! only while one exists — the zero-idle-wakeup gate.
//!
//! The callback is held as `WeakLua` + `LuaRegistryKey` (never a
//! `LuaFunction` — it outlives this stack frame), so a live timer can
//! never keep a dropped VM alive: after hot reload (M2 §5) drops the
//! `Vm`, the next fire finds no VM and asks to be removed.

use std::time::Duration;

use mlua::prelude::*;

/// A timer waiting for the binary to insert its calloop source.
pub struct PendingTimer {
    /// Time until the first fire.
    pub delay: Duration,
    /// `Some(period)` = repeating (`shell.interval`), `None` =
    /// one-shot (`shell.once`).
    pub period: Option<Duration>,
    weak: WeakLua,
    key: LuaRegistryKey,
}

impl PendingTimer {
    pub(crate) fn new(lua: &Lua, ms: u64, repeating: bool, f: LuaFunction) -> LuaResult<Self> {
        Ok(Self {
            delay: Duration::from_millis(ms),
            // Clamp the repeat period: interval(0) must not spin the loop.
            period: repeating.then(|| Duration::from_millis(ms.max(1))),
            weak: lua.weak(),
            key: lua.create_registry_value(f)?,
        })
    }

    /// Call the stored callback. Returns `false` when the timer should
    /// be dropped: the VM is gone (reload in flight) or the registry
    /// key no longer resolves. Callback *errors* are logged and keep
    /// the timer alive — a transient failure must not kill a clock.
    pub fn fire(&self) -> bool {
        let Some(lua) = self.weak.try_upgrade() else {
            return false;
        };
        match lua.registry_value::<LuaFunction>(&self.key) {
            Ok(f) => {
                if let Err(e) = f.call::<()>(()) {
                    tracing::error!("timer callback error: {e}");
                }
                true
            }
            Err(e) => {
                tracing::error!("timer registry lookup failed: {e}");
                false
            }
        }
    }
}
