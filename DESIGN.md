# tomoe — design & roadmap

> **tomoe** (巴, after Tomoe River — the fountain-pen paper you return to
> again and again) is a Wayland compositor where the window manager is
> yours to rewrite. The Rust core exposes mechanism — windows, outputs,
> input, a camera over an infinite canvas — and everything above it is
> Lua, hot-reloaded on save. Since FUSION.md, that "everything" includes
> the desktop shell: **moonshell**, the formerly standalone Lua shell,
> lives in-process as tomoe's shell subsystem (`moonshell-*` crates, the
> `ui.*`/`shell.*` Lua surface). One repo, one process, one Lua program
> driving both window management and shell UI.

## Vision

- **As fast as niri** — the per-output redraw state machine, damage
  tracking, and plane offloading are lifted from its shape.
- **As featureful as Hyprland** — the protocol/feature gap list in
  PLAN.md is audited against it.
- **As configurable as ShojiWM** — policy is a program, not a config
  file; the WM itself is a replaceable Lua module.
- **As integrated as GNOME Shell** — one process renders windows and
  shell UI (bars, OSDs, notifications, window decorations) atomically;
  ours is Lua and leaner (the FUSION.md capstone: compositor-drawn tab
  strips scripted from config).

Reference implementations are vendored under `ref/` (niri, ShojiWM, …)
and are the source of truth for patterns; they follow their own
conventions, not this repo's.

## The fusion decision (2026-07-24)

Recorded here per FUSION.md before any code moved; FUSION.md is the
work tracker, this is the record.

**What:** moonshell stopped being a standalone, compositor-agnostic
Wayland client and became tomoe's in-process UI engine. Its element
vocabulary (`ui.*`: hbox/vbox/stack/text/icon/image/progress/…), CPU
raster stack (tiny-skia + cosmic-text), and native services (rustbus
D-Bus: UPower/MPRIS/NetworkManager/notifications/SNI tray; sysfs
battery) move into the compositor. Bars, launchers, OSDs, the
notification daemon, and — the motivating capstone — **window
decorations such as tab strips** are compositor-rendered from Lua
element tables, composited atomically with window geometry.

**Why fusion beat the alternatives:**
- Tab layouts need compositor-side rendering. A client process drawing
  tab strips always trails window geometry by a frame during drags,
  resizes, animations, and zoomer pans, and dies with the shell
  process. No repo arrangement fixes that; in-process rendering does.
- One Lua VM means the bar reads `wm.workspaces` directly — the IPC
  round-trip, the `wm_state` broadcast vocabulary, and the stale-title
  gap all dissolve. WM policy and shell UI compose in one program with
  one reload contract.
- One process, one flake, one CI, one conventions doc, one LuaLS stub
  tree. GNOME Shell (mutter + shell, one process, JS) is the
  architectural precedent.

**What was knowingly given up (recorded, not implied):**
- moonshell's compositor-agnosticism (niri/Hyprland/Sway backends) is
  **discontinued**. External bars (waybar etc.) still work on tomoe via
  layer-shell and `tomoe-ipc` — fusion adds a native shell, it removes
  no protocol.
- moonshell's standalone memory target (25 MB RSS vs AGS/QuickShell) is
  moot in-process; the fused win is *zero additional processes and zero
  IPC wakeups* for the native shell.
- Doctrine 03 as previously applied (moonshell as the thin client
  proving the wire): the native shell is now policy inside the daemon.
  `tomoe-ipc` remains the versioned wire for *external* clients and
  keeps its own discipline; it just loses its captive first consumer.
- moonshell's pre-fusion DESIGN.md (now `docs/moonshell-DESIGN.md`)
  locked "no shared Rust beyond tomoe-ipc" and "integration ships as
  content, not code"; both rows are **reversed** by this decision.

## Doctrine conformance

Conforms to `~/Dev/doctrines` @ c7eb06a (2026-07). One row per
doctrine; "shell:" notes cover the fused moonshell subsystem.

| Doctrine | Status | Notes |
|---|---|---|
| 01 extension-first core | follows | The default WM (`resources/wm.lua`), zoomer, and screencast policy are ordinary Lua on the public API; with no config, windows map full-screen. Shell: strengthened by fusion — every shipped widget/bar/OSD/decoration is Lua on the same `ui.*`/`shell.*` API user configs get (FUSION north star). Sanctioned native-UI exemption: the screenshot UI (PLAN.md). |
| 02 snapshot in, actions out | follows | Lua reads a snapshot refreshed before every entry; writes are queued ops applied after the callback. Every Lua entry is time-boxed by the dispatch watchdog (`settings.watchdog_ms`, default 1000 ms; forces the interpreter on — LuaJIT traces never check hooks). Shell: *gains* this contract by fusion — standalone moonshell had no watchdog (its recorded divergence); in-process render callbacks return element tables under the same snapshot/queue/watchdog rules, and F1 adds the raster deadline as its render-side analogue. |
| 03 daemon + thin client | follows (revised at F0) | tomoe is the state-owning daemon; `tomoe-ipc` is the versioned wire for external clients (`tomoe msg`, waybar-style consumers, the portal). The native shell is no longer a client — it is policy inside the daemon (see the fusion decision). The wire crate stays separate from the fast-moving event vocabulary (`wm_state` etc. live in Lua). |
| 04 declarative front, idempotent executor | n/a | The config is a live program by design; the home-manager module (F6) only places files. |
| 05 one declaration mechanism | follows | Binds, rules, processes, IPC endpoints, hooks: one registry shape each. Shell: strengthened by fusion — `ui.*` element tables become the single vocabulary for all compositor UI (builtin dialogs, bars, OSDs, launcher, decorations); `tomoe.ui`'s bespoke widget enum retires into it (F1/F4). |
| 06 bare core must boot | follows | No config: windows map full-screen, no shell surfaces; the transitional standalone moonshell binary keeps its own doctrine-06 gate (`boot` flake check, 20 MB RSS budget) until F6 deletes it. |
| 07 nix source of truth | follows | `nix build` / `nix flake check` (build+test, fmt, clippy, boot, arch-fresh); `cargo fmt`/`clippy` sanctioned exceptions for iteration. |
| 08 error containment | follows | Config errors surface as the on-screen banner, never a crash; watchdog aborts runaway Lua entries; a failed reload keeps the old VM's windows (open-event replay / `on_reload` restore). Shell surfaces join the same contract at F2. |
| 09 versioned contracts | follows | `tomoe-ipc` (versioned wire, external clients); the Lua API held to docs/stubs by parity tests (`docs/lua-api.md`, LuaLS stubs); portal D-Bus interfaces. Mismatch behavior: IPC version handshake, docgen/parity tests fail CI. |
| 10 interface early, machinery late | follows | Extraction waits for duplication: no shared lua-host crate was made while moonshell was standalone; fusion removed the need. Decoration slots (F5) get an API only after the tabs + titlebar instances exist. |
| 11 dogfood early | follows | tomoe is the daily driver (tty session via `tomoe-session`); the fused shell replaces the external bar as F2–F4 land. |
| conventions/lua.md | follows | Settings-table shape, `on_*` hook naming, `Mod` convention, reload contract; one conventions doc now serves WM and shell (`ui.*`/`shell.*` inherit moonshell's contract first-class). |

## Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Fusion | **moonshell merges into tomoe as its shell subsystem** | See "The fusion decision" above. The name survives on the `moonshell-*` crates, the shell builtins layer, and the `shell.*` global; the product is "tomoe with moonshell". |
| Lua namespaces | **`ui.*` + `shell.*` kept first-class alongside `tomoe.*`** | The inherited moonshell contract, not aliased: `ui.*` element vocabulary, `shell.*` shell API, `tomoe.*` compositor policy. nur-era configs keep porting with at most mechanical edits. |
| Scripting | **mlua + LuaJIT, vendored** | µs-level calls, single binary; LuaJIT FFI is the extensibility escape hatch. Same pick both sides of the fusion — one VM was a precondition for it. |
| Coordinates | **physical-first (`coords.rs`)** | Fractional scale without drift: element/window layout resolves to integer physical pixels before raster/placement. The shell element engine inherits this at F1 (pixel-stable at fractional scales). |
| Event loop | **calloop, single thread; no tokio** | Timers, sockets, D-Bus, inotify are calloop sources. rustbus (not zbus) for D-Bus because zbus structurally needs an async executor. Services ride the compositor loop at F3. |
| Shell rendering | **CPU raster (tiny-skia + cosmic-text) into GLES textures** | Shell UI redraws on state change, not per-frame; CPU raster with damage-rect texture upload composits like any element. Raster on the main thread with a deadline (worker handoff only if a real bar blows the frame budget — FUSION open decision). |
| Smithay pin | **`y0usaf/smithay#tomoe-tearing`** (niri's pin + async page flip) | Upstream lacks tearing page flips; the fork carries exactly that commit, rebased when the pin bumps. |
| Wire | **`tomoe-ipc`, in-workspace path dep, versioned for external clients** | Fusion made it a path dep; the versioned-wire discipline stays because external consumers remain (doctrine 09). |
| Licenses | **workspace AGPL-3.0-or-later; `moonshell-*` crates explicitly AGPL-3.0-only** | The crates keep their pre-fusion license rather than being silently relicensed by workspace inheritance. |

## Architecture

Generated map: ARCHITECTURE.md (crate graph + module trees, enforced
fresh by `checks.arch-fresh`). Roles:

```
crates/
  tomoe/                    # the compositor: backends (winit/tty), render,
                            # input, protocols, Lua runtime, IPC server (core)
  tomoe-ipc/                # versioned wire contract for external clients   (contract)
  xdg-desktop-portal-tomoe/ # screencast portal backend                      (satellite)
  moonshell/                # transitional standalone shell binary — F6 deletes (transitional)
  moonshell-surface/        # SCTK client stack — F6 deletes with the binary (transitional)
  moonshell-render/         # element tree → tiny-skia; cosmic-text; damage  (mechanism)
  moonshell-runtime/        # ui.*/shell.* Lua surface, reactive state       (mechanism)
  moonshell-services/       # rustbus D-Bus + sysfs services                 (mechanism)
resources/                  # the builtins layer: wm.lua, zoomer.lua,
                            # screencast.lua, moonshell/ (ui.* stdlib,
                            # widgets, theme)                                (policy)
```

## Extension surface contract

- **Read path:** immutable snapshot (windows, outputs, camera, pointer,
  and — post-F3 — `shell.services.*` state) refreshed before every Lua
  entry.
- **Write path:** queued operations and returned element tables,
  applied after the callback returns; the render loop never waits on
  config code.
- **Watchdog:** every Lua entry time-boxed (`settings.watchdog_ms`,
  default 1000; `0` opts out and restores full JIT). F1 adds the raster
  deadline for element trees (slow tree logs and skips the update).
- **Escape hatches:** LuaJIT FFI; process/socket/file primitives;
  post-F6: generic D-Bus proxy, `ui.canvas`.
- **Named exception:** the screenshot UI is native (PLAN.md).

## Deferred (and why)

The living list is PLAN.md ("deferred" markers) and FUSION.md; the
standing entries:

- **IME (text-input/input-method)** — after core parity; real work,
  tracked honestly rather than implied.
- **Touch/tablet, virtual keyboard** — no hardware pressure yet.
- **Shell raster worker thread** — only if a real bar blows the frame
  budget on real hardware (FUSION open decision).
- **GTK/Qt theme or CSS compat for shell UI** — never; themes are Lua
  tables.
- **taffy layout** — flex-lite covers the bar/launcher/OSD genre;
  adopt on a real grid/wrap need.

## Roadmap

Two trackers, both with per-item acceptance criteria:

- **PLAN.md** — compositor parity (milestones M1…; niri performance /
  Hyprland features / ShojiWM configurability).
- **FUSION.md** — the fusion milestones F0 (repo merge, this document)
  through F6 (retirement of the transitional standalone binary), with
  the F5 capstone: scriptable decorations and tab layouts.
