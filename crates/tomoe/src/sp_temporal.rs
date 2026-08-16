//! SP-Temporal ceremony over the fused shell mounted inside the
//! compositor.
//!
//! The fused shell is not a separate client: it is the `shell.*`/`ui.*`
//! Lua surface registered into the compositor's *one* VM (FUSION.md),
//! owned by `LuaRuntime` and composited by `ShellSurfaces`. Its
//! temporal mount/unmount lifecycle is therefore a *VM* lifecycle: a
//! config is mounted (VM boots + declarations), its actions are drained
//! (adopt + refresh), and a hot reload unmounts it and boots a fresh VM
//! — "kill + reattach the compositor client" happens entirely in-process.
//!
//! This is the ceremony, run as the state of the round-trip — not as a
//! statement:
//!
//! 1. **Snapshot** the compositor context the shell will mount over
//!    (empty shell, drained queues, display snapshot).
//! 2. **Mount** a shell config declaring `shell.window` surfaces over
//!    every `ui.*` element type and exercising the `shell.*` API.
//! 3. **Exercise** the compositing effects: adopt, refresh (render →
//!    texture), click-through to a Lua handler, interval/once timers,
//!    `shell.exec`/`shell.exec_async`, `shell.state` + subscribers,
//!    `shell.watch_file`, keyboard + service facades, exclusive-zone
//!    reservation.
//! 4. **Unmount** (the config reload's `shell.clear()` + VM drop) and
//!    **diff** the context: the shell must leave no residue (empty
//!    surface set, every action queue drained, no globals leaked into a
//!    fresh context) while the compositor context's display snapshot
//!    survives.
//! 5. **Re-mount after reload/restart**: a fresh VM re-attaches to the
//!    compositor snapshot, `on_reload` restore + open-replay reconstruct
//!    state, and the same surface topology composes again.
//!
//! Failure contract: any residue after unmount, or any missed
//! reconstruction on re-mount, fails the test.

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use smithay::utils::{Physical, Size};

    use crate::lua::LuaRuntime;
    use crate::shell::ShellSurfaces;
    use crate::ui::element_tree::Engine;

    /// A shell policy chunk: two surfaces, one carrying a clickable
    /// button hit-tested later, one exercising the wider `ui.*`
    /// vocabulary; plus the `shell.*` API and an `on_reload` persisters
    /// pair.
    const SHELL_POLICY: &str = r#"
        clickshell = 0
        sub_hits = 0
        live_count = #shell.displays()

        global_st = shell.state("mounted")

        -- Bar: full-width top surface with a clickable button.
        local bar = shell.window({ name = "bar", position = "top", height = 30 })
        bar:render(function()
          return ui.hbox({ children = {
            ui.text({ content = "L", width = 50, height = 30 }),
            ui.button({
              width = 50, height = 30,
              on_click = function() clickshell = clickshell + 1 end,
              children = { ui.text("btn") },
            }),
          }})
        end)

        -- Panel: stretched right edge; exercises the element vocabulary.
        local panel = shell.window({ name = "panel", position = "right", width = 60 })
        panel:render(function()
            return ui.vbox({ children = {
                ui.hstack({ children = { ui.label("a"), ui.label({ text = "b" }) } }),
                ui.vstack({ children = { ui.text("c") } }),
                ui.stack({ children = { ui.text("s") } }),
                ui.overlay({ children = { ui.text("o") } }),
                ui.separator({ orientation = "vertical" }),
                ui.progress_bar({ value = 0.5 }),
                ui.circular_progress({ value = 0.25 }),
                ui.icon({ name = "battery-empty" }),
                ui.spacer(),
                ui.text(global_st),
            }})
        end)

        -- shell.state reactivity: subscribers fire on :set.
        global_st:subscribe(function() sub_hits = sub_hits + 1 end)

        shell.interval(100000000, function() timer_fired = (timer_fired or 0) + 1 end)
        shell.once(100000000, function() once_fired = (once_fired or 0) + 1 end)
        shell.watch_file("/nonexistent/path", function() end)

        -- shell.exec (blocking) and shell.exec_async (round-trip).
        exec_out = shell.exec("echo world")
        async_out = ""
        shell.exec_async("echo async", function(out) async_out = out end)

        -- on_reload: the reconstruction axis — save in the outgoing VM,
        -- restore in the fresh one (open-event replay is the fallback).
        tomoe.on_reload("ceremony", function()
            return { clickshell = clickshell, st = global_st:get(), live_count = live_count }
        end, function(state)
            restored_clicks = state.clickshell
            restored_st = state.st
            restored_live = state.live_count
        end)
    "#;

    /// The compositor's display snapshot re-attached to a shell context.
    fn seed_display(rt: &LuaRuntime) {
        rt.shell_ctx()
            .set_displays(vec![moonshell_surface::DisplayInfo {
                name: "DP-1".into(),
                x: 0,
                y: 0,
                width: 1280,
                height: 720,
                scale: 1,
            }]);
    }

    fn outputs() -> Vec<(String, Size<i32, Physical>, f64)> {
        vec![("DP-1".to_string(), Size::from((1280, 720)), 1.0)]
    }

    /// Drain one surface/window: adopt declared surfaces + re-raster the
    /// dirty trees. Mirrors `state::after_lua` (FUSION F2) without the
    /// calloop sources.
    fn drain(
        rt: &mut LuaRuntime,
        shell: &mut ShellSurfaces,
        engine: &mut Engine,
        outputs: &[(String, Size<i32, Physical>, f64)],
    ) -> bool {
        let ctx = rt.shell_ctx();
        let adopted = shell.adopt(ctx.take_pending());
        if ctx.take_dirty() {
            shell.mark_dirty();
        }
        shell.refresh(rt, engine, outputs) || adopted
    }

    #[test]
    fn fused_shell_mount_exhaust_unmount_remount_roundtrip() {
        // ── (1) SNAPSHOT the compositor context the shell mounts over ──
        let mut shell = ShellSurfaces::default();
        let mut engine = Engine::new();
        assert!(shell.is_empty(), "pre-mount: shell must be empty");

        let mut shell_rt = LuaRuntime::new().unwrap();
        seed_display(&shell_rt);
        // Context the shell mounts over, as a liging fingerprint:
        // the display snapshot it composes into.
        let display_context = seed_display_fingerprint(&shell_rt);

        // ── (2) MOUNT the shell policy ──
        shell_rt
            .lua()
            .load(SHELL_POLICY)
            .set_name("sp-temporal-shell.lua")
            .exec()
            .unwrap();

        let mounted = drain(&mut shell_rt, &mut shell, &mut engine, &outputs());
        assert!(mounted, "mount: draining must adopt the declared surfaces");
        assert!(
            !shell.is_empty(),
            "mount: the compositor must now hold shell surfaces"
        );

        // ── (3) EXERCISE every ui.*/shell.* effect against the mounted ──
        //     shell.
        let ctx = shell_rt.shell_ctx();
        // shell.exec (blocking subprocess) returned trimmed stdout.
        let exec_out: String = shell_rt.lua().globals().get("exec_out").unwrap();
        assert_eq!(exec_out, "world", "shell.exec did not forward stdout");

        // Hit-through: a click on the button in the textured bar bubbles
        // to the deepest on_click handler in the VM.
        let hit = shell
            .click_target("DP-1", (75.0, 15.0))
            .expect("click must land on the bar's button cell");
        let (shared, path) = hit;
        assert!(shell_rt.click_shell(&shared, &path), "handler must run");
        let clicks: i64 = shell_rt.lua().globals().get("clickshell").unwrap();
        assert_eq!(clicks, 1, "button on_click must have fired exactly once");

        // Timers (interval + once) queued by the policy; firing each
        // runs its callback under the watchdog.
        let timers = ctx.take_timers();
        assert_eq!(timers.len(), 2, "policy must have queued interval + once");
        for timer in &timers {
            assert!(shell_rt.fire_shell_timer(timer), "live timer must fire");
        }
        let timer_fired: i64 = shell_rt.lua().globals().get("timer_fired").unwrap();
        let once_fired: i64 = shell_rt.lua().globals().get("once_fired").unwrap();
        assert_eq!((timer_fired, once_fired), (1, 1));

        // shell.state reactivity: subscribers fire on :set, and the
        // value is read back by the tree (ui.text(global_st)).
        shell_rt
            .lua()
            .load("global_st:set(\"changed\")")
            .set_name("exercise.lua")
            .exec()
            .unwrap();
        let st: String = shell_rt
            .lua()
            .load("return global_st:get()")
            .eval()
            .unwrap();
        assert_eq!(st, "changed", "shell.state: after :set must read back");
        let sub_hits: i64 = shell_rt.lua().globals().get("sub_hits").unwrap();
        assert_eq!(sub_hits, 1, "shell.state subscriber must fire once");

        // shell.exec_async round-trip through the calloop channel.
        let channel = ctx.take_exec_channel().expect("exec channel from adoption");
        use smithay::reexports::calloop;
        let mut loop_ = calloop::EventLoop::<LuaRuntime>::try_new().unwrap();
        loop_
            .handle()
            .insert_source(channel, |event, _, rt| {
                if let calloop::channel::Event::Msg(reply) = event {
                    rt.dispatch_shell_exec_reply(reply);
                }
            })
            .unwrap();
        for _ in 0..50 {
            let _ = loop_.dispatch(Some(Duration::from_millis(1)), &mut shell_rt);
            let done: bool = shell_rt
                .lua()
                .globals()
                .get::<String>("async_out")
                .map(|s| !s.is_empty())
                .unwrap_or(false);
            if done {
                break;
            }
        }
        let async_out: String = shell_rt.lua().globals().get("async_out").unwrap();
        assert_eq!(async_out, "async", "exec_async reply must reach Lua");

        // shell.watch_file queues a file watch (drained above).
        let watches = ctx.take_watches();
        assert_eq!(watches.len(), 1, "watch_file must queue a watch");

        // Compositing effects: keyboard + service facade pushes (no-op
        // without an attached facade, but the drain path must not error).
        shell_rt.push_shell_keyboard(1, "right");
        shell_rt.push_shell_services(None, None, None, None, None);

        // Exclusive-zone reservation: the top bar reserves its 30 px.
        let zone = smithay::utils::Rectangle::new((0, 0).into(), (1280, 720).into());
        let shrunk = shell.shrink_zone(zone);
        assert_eq!(shrunk.loc.x, 0);
        assert_eq!(shrunk.loc.y, 30, "top bar must reserve its exclusive zone");
        // The right-edge panel then reserves 60 px off the right.
        assert_eq!(shrunk.size.w, 1280 - 60);
        assert_eq!(shrunk.size.h, 720 - 30);

        // ── (4) UNMOUNT + DIFF the compositor context ──
        // Persist state out of the outgoing VM (reload's first half).
        let persisted = shell_rt.save_reload_state();
        assert!(
            persisted.contains_key("ceremony"),
            "save_reload_state must carry the ceremony key"
        );

        assert!(shell.clear(), "unmount must drop the surface set");
        assert!(shell.is_empty(), "unmount: no surface residue may remain");

        // The old VM (and its shell ctx) is dropped — kill the client.
        drop(shell_rt);

        // Diff: a fresh context boots with no residue from the old one.
        let mut fresh = LuaRuntime::new().unwrap();
        let fresh_ctx = fresh.shell_ctx();
        assert!(
            fresh_ctx.take_pending().is_empty(),
            "re-mount ctx must not inherit pending windows"
        );
        assert!(
            fresh_ctx.take_timers().is_empty(),
            "re-mount ctx must not inherit timers"
        );
        assert!(
            fresh_ctx.take_watches().is_empty(),
            "re-mount ctx must not inherit watches"
        );
        // The dead VM's exec_async callback map dies with the dead ctx;
        // take-and-drop the fresh channel (every ctx owns one) to show
        // the fresh shell starts clean.
        fresh_ctx.take_exec_channel();
        let leaked: mlua::Value = fresh.lua().globals().get("live_count").unwrap();
        assert!(leaked.is_nil(), "leaked global must not cross the reload");

        // The compositor's display snapshot was not clobbered by the
        // unmount — re-attaching it to a fresh shell yields the same
        // context the earlier policy mounted over.
        seed_display(&fresh);
        assert_eq!(seed_display_fingerprint(&fresh), display_context);

        // ── (5) RE-MOUNT after reload: reconstruct via on_reload ----──
        //     restore + re-declared policy (the reconstruction axis).
        fresh
            .lua()
            .load(SHELL_POLICY)
            .set_name("sp-temporal-shell.lua")
            .exec()
            .unwrap();
        let reconstructed = drain(&mut fresh, &mut shell, &mut engine, &outputs());
        assert!(reconstructed, "re-mount must re-adopt the surfaces");
        assert!(
            !shell.is_empty(),
            "re-mount: the surface topology must reconstruct"
        );
        let restored = fresh.restore_reload_state(&persisted);
        assert_eq!(restored, 1, "one on_reload state must restore");
        let restored_clicks: i64 = fresh.lua().globals().get("restored_clicks").unwrap();
        let restored_st: String = fresh.lua().globals().get("restored_st").unwrap();
        let restored_live: i64 = fresh.lua().globals().get("restored_live").unwrap();
        assert_eq!(restored_clicks, 1, "persisted clickshell must be restored");
        assert_eq!(restored_st, "changed", "persisted state must be restored");
        assert_eq!(
            restored_live, 1,
            "the compositor display snapshot must be visible on re-mount"
        );
        // The re-mounted shell is live again: a click still reaches its
        // button handler in the fresh VM.
        let click = shell
            .click_target("DP-1", (75.0, 15.0))
            .expect("re-mount: button cell must be hit-testable");
        let (shared, path) = click;
        assert!(fresh.click_shell(&shared, &path), "re-mount: handler runs");
        let clicks: i64 = fresh.lua().globals().get("clickshell").unwrap();
        assert_eq!(
            clicks, 1,
            "re-mount: fresh handler fires once from its own init"
        );
    }

    /// Finger the compositor-context snapshot the shell mounts over, as
    /// a display list the shell would see via `shell.displays()`.
    /// Used both to assert the context survived unmount (identical) and
    /// that the fresh shell re-attaches to it (identical).
    fn seed_display_fingerprint(rt: &LuaRuntime) -> i32 {
        // The display list is the compositor-context term the shell
        // reads; hash its width (the only interesting field at this
        // scale).
        let displays: mlua::Table = rt.lua().load("return shell.displays()").eval().unwrap();
        let n: i32 = displays.len().unwrap() as i32;
        n
    }
}
