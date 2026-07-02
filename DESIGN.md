# takhti — design & roadmap

> **Takhti** (تختی, Urdu: the wooden school slate you write on, wash clean, and write on again) —
> a surface that's yours to rewrite, like the compositor's hot-reloadable Lua config.
> Used as the binary, crate, `~/.config/takhti/`, and Lua global name.

## Vision

A Wayland compositor that is:
- **As performant as niri** — render-on-damage with a per-output redraw state machine, direct scanout, DMA-BUF, VRR.
- **As feature-rich as Hyprland** — animations, blur/shadows/rounded corners, window rules, special workspaces, groups, tearing.
- **As configurable as ShojiWM** — config is a *program*, not a settings file: event hooks, custom layouts, scriptable decorations, process management, user-extensible IPC, hot reload with state persistence.

References live in `ref/` (niri 26.4.0, Hyprland 0.55.0, ShojiWM) and are the source of truth for patterns.

## Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Core | Rust + Smithay, git rev `ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef` | Same rev as niri 26.4 — the most battle-tested Smithay pin in existence |
| Config runtime | **Embedded Lua (mlua, LuaJIT, vendored)** | ShojiWM's capability surface without the out-of-process protocol machinery; single binary; µs-level hook calls; AwesomeWM/Neovim-proven. LuaLS `---@meta` annotations shipped for editor DX |
| Policy split | **Core-as-mechanism, dogfooded extension surface** (revised from "hybrid" after Phase 2): the Rust core exposes a large window/output/input/event API, and ALL WM policy — workspaces, tiling, focus order — ships as a default Lua library (`require("wm")`) built on that same public surface. Like Emacs/VS Code/pi: if our own WM can't be built on the API, the API isn't done | Guarantees the extension surface stays powerful; users can replace the entire WM, not just tune it. Native fallback: with no hooks registered, windows map full-screen so a broken config still shows something |
| Priorities | Daily-driveable core first, then eye-candy | User choice |
| XWayland | xwayland-satellite (niri/ShojiWM approach) | Avoids Hyprland's largest complexity sink |
| X11 clients, screencast | Later phases: wlr-screencopy + xdg-desktop-portal | |

## Architecture

```
crates/
  takhti/            # the compositor binary
    src/
      main.rs      # CLI, logging, backend selection
      state.rs     # Takhti: Space, seat, outputs, protocol states, Lua runtime handle
      handlers/    # Smithay delegate impls (compositor, xdg-shell, shm, seat, ...)
      backend/     # winit (dev) + tty (DRM/GBM/libinput, Phase 2)
      layout/      # built-in engines: scrolling, dwindle, master, monocle, floating
      input/       # keybind parsing/dispatch, libinput config
      lua/         # mlua runtime, `takhti` API table, watchdog, hot reload
      render/      # render elements; later: borders, rounded, shadow, blur
  takhti-ipc/        # (Phase 4) serde types for the JSON IPC socket + `takhti msg` CLI
```

### Coordinate doctrine (physical-first)

Niri's sharpness comes from a discipline: every element lands on an integer
physical pixel, so client buffers are sampled 1:1 (no resampling → crisp
text). Niri enforces it by convention — fractional logical coordinates plus
rounding helpers every code path must remember to call. Takhti enforces the
same invariant *structurally*:

1. **The canonical coordinate space is integer physical pixels.** Layout, the
   Lua API, window/output bookkeeping (`space.rs`, our replacement for
   smithay's integer-*logical* `Space`), and render element positions are all
   `i32`/`Point<i32, Physical>`. Integers on the physical grid cannot
   misalign; there is no rounding to forget.
2. **All logical↔physical conversion lives in `coords.rs`** — nowhere else.
   Protocol objects that speak logical (xdg configure sizes, wl_output
   geometry, pointer events) convert exactly once at that boundary.
3. **Window body sizes are quantized at the boundary.** xdg configure sizes
   are integer logical, so at scale `s` clients can only produce buffers of
   `round(n·s)` physical pixels (`coords::configure_size`). No architecture
   escapes this — it's baked into the protocol. Compositor-drawn pixels
   (borders, gaps, UI) are free at any physical integer, which is why a
   1-device-pixel border works at any scale.
4. **Scale is snapped to N/120** (`coords::snap_scale`), the only granularity
   wp-fractional-scale-v1 can express; wp-viewporter + fractional-scale are
   advertised so clients render at native density from their first buffer.
5. **No offscreen intermediates.** Effects that render windows into a texture
   first (blur, rounded-corner passes) are a chance to resample; Phase 5
   effects must keep intermediate framebuffers pixel-aligned too.

Currently one scale applies to all outputs (`takhti.settings { scale }`);
per-output scale is a later, local change — it belongs in the existing
per-output `settings.displays` table (which already holds `resolution`).
Known follow-ups: compositor UI font sizes don't multiply by scale yet, and
output logical positions for mixed-scale multi-head need a placement policy.

### Performance keystones (from the references)

1. **Per-output redraw state machine** (`Idle / Queued / WaitingForVBlank / WaitingForEstimatedVBlank`)
   — the single biggest reason niri is smooth at high refresh where Hyprland/Anvil stutter. ShojiWM's own
   investigation confirms this: `ref/ShojiWM/knowledges/high-refresh-investigation.md`. Spec:
   `ref/niri/docs/wiki/Development:-Redraw-Loop.md`. Adopt from Phase 1 (simplified) onward.
2. Render only on damage; never redraw idle outputs; frame callbacks throttled per output sequence.
3. The render loop **never blocks on Lua**. Hooks run as calloop-scheduled callbacks with an
   instruction-count watchdog (Lua debug hooks) so a runaway config cannot hang the frame.
4. Direct scanout + overlay planes, VRR, tearing (immediate flips) in the TTY backend (Phase 2+).

### Extension-surface contract

- Lua never borrows compositor state: reads come from a snapshot the core refreshes
  before every Lua entry; writes are queued ops (`set_geometry/show/hide/focus/close`)
  applied when the callback returns. The frame loop never waits on Lua.
- Window objects are userdata handles (id-based, `__eq` by id) with properties
  (`app_id`, `title`, `geometry`) and methods; events (`on_window_open/close`,
  `on_focus_change`, `on_outputs_changed`) drive policy.
- The default WM (`resources/wm.lua`, preloaded as module `"wm"`) implements
  workspaces 1-9 + dwindle tiling + focus cycling purely on this API. It is loaded
  only when config `require`s it — replaceable wholesale.

### Lua API surface (target; grows per phase)

```lua
takhti.settings { gaps = 8, focus_follows_mouse = true }
takhti.bind("Super+Return", function() takhti.spawn("foot") end)
takhti.bind("Super+1", "workspace 1")            -- built-in actions as strings
takhti.on_window_open(function(win) ... end)      -- hooks: open/close/focus/output/...
takhti.rule { app_id = "mpv", floating = true }   -- rules as data or as functions
takhti.register_layout("fibonacci", fn)           -- custom layout, same trait as built-ins
takhti.workspace(2).layout = "scrolling"
takhti.process.service("waybar", { restart = "on-exit" })  -- ShojiWM-style manifest
takhti.ipc.serve("mybar", handler)                -- user-extensible IPC
takhti.on_reload(snapshot_fn, restore_fn)         -- hot reload with state persistence
```

## Roadmap

- **Phase 1 (now): skeleton that runs.** Flake devshell; winit backend; GlesRenderer +
  OutputDamageTracker; xdg-shell windows mapped and tiled (dwindle first); keyboard/pointer;
  Lua config with binds/spawn/hooks/settings. *Accept: spawn two terminals, they tile, keybinds work.*
- **Phase 2: TTY.** DRM/GBM/libinput/libseat session backend, multi-output, output config from Lua,
  full redraw state machine, presentation-time. *Accept: daily-drive login session.*
- **Phase 3: WM depth.** All five layouts incl. scrolling (niri column model), workspaces
  (per-output, niri-style dynamic), window rules engine, floating, fullscreen/maximize,
  layer-shell, focus-follows-mouse, drag/resize grabs.
- **Phase 4: IPC + tooling.** JSON socket (`takhti msg`), event stream, `takhti.ipc.serve`,
  LuaLS meta files, hot reload with snapshot/restore.
- **Phase 5: Eye-candy.** Borders/rounded/shadows (shader elements), dual-kawase blur,
  animation engine (springs + beziers, Hyprland-style AnimatedVariable on layout positions).
- **Phase 6: Ecosystem.** xwayland-satellite integration, screencopy + portal, gamma/night-light,
  idle/lock protocols, VRR/tearing polish, custom Lua layouts API stabilization.

## Reference reading list (implementation-time)

- Redraw loop: `ref/niri/docs/wiki/Development:-Redraw-Loop.md`, `ref/niri/src/niri.rs`
- Winit backend at our Smithay rev: `ref/niri/src/backend/winit.rs`
- Scrolling layout model: `ref/niri/src/layout/`
- Blur/effects: `ref/hyprland/src/render/`, ShojiWM `dualKawaseBlur`
- Process manifest API: `ref/ShojiWM/knowledges/process-api.md`
- Hot-reload state persistence: `ref/ShojiWM/packages/config/src/index.tsx` (`onDisable`/`onEnable`)
