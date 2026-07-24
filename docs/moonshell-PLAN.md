# moonshell — build plan

> **Superseded (2026-07-24, FUSION.md F0).** moonshell merged into
> tomoe; remaining work from this plan is re-homed in `../FUSION.md`
> (M3 services → F3, M4 interactivity → F4, M5/M6 remainders → F6).
> Kept as history of the standalone project — "DESIGN.md" below means
> `moonshell-DESIGN.md`.

Working tracker toward the DESIGN.md vision: **QuickShell's
extensibility, a terminal emulator's footprint, waybar's portability**.
DESIGN.md holds doctrine and locked decisions; this file holds the
concrete work list and the order it lands in. Milestones M0–M6 and
their acceptance criteria live in DESIGN.md's roadmap; this file breaks
them into steps and records what each one shakes loose (tomoe-PLAN
style).

## Where we are (2026-07)

**M0 done** (2026-07-08): flake + workspace (`moonshell`/`surface`/
`render` crates), bare binary maps a top-anchored layer surface with
exclusive zone and draws its version string via cosmic-text into
wl_shm. `nix flake check` = build+tests, fmt, clippy(-D warnings), and
the doctrine-06 `boot` check (headless sway + `moonshell --boot-check`
+ RSS gate). Measured: **6.9 MB idle RSS live / 6.3 MB in the sandbox
gate (budget 20 MB); 0 voluntary ctx switches over 5 s idle** (powertop
unavailable — ctx-switch delta is the standing wakeup proxy); output
disable/enable under headless sway survives (unmap → wait → remap).

**M1 done** (2026-07-08): §1 (element vocabulary + flex-lite layout +
draw pass), §2 (icon/image elements + asset cache), §3 (per-element
damage diffing via `Scene`), §4 (simple-bar acceptance fixture) — see
the M1 breakdown below. Measured on the fixture: **14.5 MB RSS
(release) with a full bar tree, 0 voluntary ctx switches over 5 s
idle** — under the 25 MB full-bar budget with room for the Lua VM.

**M2 done**: §1 done (2026-07-08) — `runtime` crate boots a
vendored-LuaJIT VM, loads the ported `ui.*` stdlib from
`lua/moonshell/stdlib.lua`, and parses nur's element-table contract
into `render::Element`. §2 done (2026-07-08) — `surface` is
multi-window (`Shell` + `WindowId` handles). §3 done (2026-07-08) —
config resolution, `shell.window`/`shell.state`, `LuaPainter` render
callbacks; a Lua bar runs live on tomoe at **2.3 MB RSS (release),
0 voluntary ctx switches / 5 s**. §4 done (2026-07-08) — `shell.
interval`/`once` (queued `PendingTimer` → calloop timers, armed only
while one exists), `shell.exec`/`exec_async` (worker thread + calloop
channel), `shell.quit`, `shell.get_window` (named-window registry),
`shell.displays` (snapshot fed from `surface::Shell::displays()`).
Live on tomoe: all APIs verified by a self-checking config; **2.2 MB
RSS idle, 0 voluntary ctx switches / 5 s (no timers); 6.6 MB with a
1 s clock ticking**. §5 done (2026-07-08) — hot reload: one inotify
instance (calloop `Generic` on a dup'd fd) watches the config tree;
`.lua` changes debounce (100 ms) into a full VM swap; `shell.reload()`
takes the same path by flag; `shell.watch_file` rides the same watcher.
Live on tomoe: **7.3 MB RSS idle (release), 0 voluntary ctx
switches / 5 s; 10 successive hot reloads → zero RSS growth**. §6 done
(2026-07-08) — **M2 complete**: theme/utils/widgets ported to
`lua/moonshell/`, `package.preload` registration with `nur.*` aliases,
`ui.bar_layout` + theme-aware `shell.window` wrapper, placeholder
`shell.services.*` facades, Lua conventions doc landed in
`~/Dev/doctrines/conventions/lua.md`. **nur's `examples/simple-bar/
init.lua` runs byte-for-byte unmodified** (vendored as the acceptance
fixture; CI test `nur_simple_bar_runs_unmodified`). Live on tomoe:
**17.4 MB RSS (release) with the full bar + 1 Hz clock (budget 25 MB);
wakeups only from the live timer (≈3 ctx switches per tick, zero
otherwise); live edit → reload verified, RSS stable across reloads**.

**M3 in progress**: §1 done (2026-07-08) — `services` crate (native,
event-driven, Lua-free), compositor service with the **tomoe backend**:
`tomoe-ipc` consumed as a git dep (the first external consumer —
doctrine 03 landed), nonblocking socket as a calloop `Generic`, tomoe's
`wm.lua` grew the `wm_state` workspace vocabulary (served + broadcast;
zero wire change), snapshots bridged into `shell.services.compositor`.
Verified live + on a nested tomoe: workspace switches, window counts,
focus titles, disconnect/reconnect, hot-reload re-seed. **7.7 MB RSS
(release, full test bar, connected), 0 voluntary ctx switches / 5 s
idle.** §2 done (2026-07-08) — niri/Hyprland/Sway backends, hand-rolled
protocols (no niri-ipc/hyprland/swayipc crates), shared `wire.rs`
plumbing, tomoe backend refactored onto it. Verified live on niri and
sway (nested/headless); **7.1 MB RSS (release) connected to niri,
0 voluntary ctx switches / 5 s idle**. Hyprland is unit-tested only —
nested Hyprland crashes in this environment (known gap, §2 notes).
§3 done (2026-07-08) — battery service: **rustbus, not zbus** (locked
decision revised in DESIGN.md — zbus needs an async executor thread,
rustbus rides calloop as a plain fd source); UPower `DisplayDevice`
over the system bus, sysfs 30 s-poll fallback (inotify doesn't fire on
sysfs). Live on the UPower path (desktop, no battery: `available =
false`, defaults kept, widget hides); **17.8 MB RSS (release) full
bar + 1 Hz clock, wakeups from the clock only**. §4 done (2026-07-08)
— network (NetworkManager over rustbus, sysfs operstate fallback) +
mpris (session bus, playerctld-style most-recently-active tracking);
shared plumbing extracted to `dbus.rs`; nonblocking runtime
request/reply via `try_get_response`. Verified live (ethernet,
Firefox player, playerctld add/remove); **9.6 MB RSS (release, all
three D-Bus services connected), 0 voluntary ctx switches / 5 s
idle**. Also fixed here: `resolve_config` canonicalization made a
home-manager (store-symlinked) config watch **/nix/store** — ~523k
inotify watches (the whole per-user budget), 116 MB RSS on the live
bar, and reloads pinned to the immutable old store file; the path is
now absolutized without resolving symlinks, and the watcher is
hard-capped at 4096 dirs. Next open item: **M3 §5 — audio**
(PipeWire native protocol vs wireplumber decision).

Two working inputs exist:

- **nur** (`~/Dev/nur`) — the reference implementation and current
  daily driver. Its Lua API (`shell.*`, `ui.*`), `lua/` stdlib, widget
  modules, and services logic are inherited; its GPUI backend, cx/
  APP_PTR bridge, and tokio runtime are not. See nur's ARCHITECTURE.md
  for the contract, nur's CLAUDE.md for its pitfalls (most evaporate
  without GPUI).
- **tomoe** (`~/Dev/tomoe`) — the sibling compositor. Its `tomoe-ipc`
  crate (wire contract: ndjson frames, `WIRE_VERSION`, socket
  discovery, blocking client) is the integration point; its IPC server
  and event stream are already live (tomoe PLAN.md M4 §2 done).

## Gap inventory by reference

### vs nur (the port)

- [x] `shell.window / get_window / state / interval / once / exec /
      quit / displays / reload` — API surface (M2; `clipboard` needs a
      data-control protocol → M4)
- [x] `ui.*` stdlib + `theme.lua` + `utils.lua` ported verbatim (M2;
      bundled modules preload under `moonshell.*` *and* `nur.*` — the
      alias delegates through `require`, one shared instance)
- [x] Widgets ported as Lua modules: clock, battery, workspaces,
      network, mpris, volume_panel, media_panel (M2 — render against
      placeholder services until M3; panel open/close needs M4 clicks +
      `handle:close`). Not ported: system_tray, bar_overlay, wallust
      (need the tray service / more surface — M3/M4)
- [ ] Services: applications (.desktop scan + inotify), audio,
      bluetooth, notifications daemon, power-profiles, sysinfo,
      system tray (SNI) (M3) — **re-implemented event-driven**
      (rustbus D-Bus/sysfs), not ported: nur's CLI-polling backends
      (`wpctl`, `nmcli`, `playerctl`, `bluetoothctl`,
      `powerprofilesctl`) are the memory/wakeup cost we're
      eliminating. Compositor auto-detect done (M3 §1; tomoe >
      Hyprland > niri > Sway); battery done (M3 §3; UPower via
      rustbus, sysfs fallback); network done (M3 §4; NetworkManager
      via rustbus, sysfs operstate fallback); mpris done (M3 §4;
      session bus, playerctld-style tracking)
- [x] Compositor backends: Hyprland, niri, Sway (M3 §2) + tomoe
      (M3 §1) — event-driven, thread-free, no compositor crates
- [ ] nix: home-manager module + `mkBar`-style lib helpers (post-M3,
      composed with tomoe's module)

### vs QuickShell/AGS (the rivalry)

- [ ] Clickable/hoverable elements, sliders, scroll (M4)
- [ ] Popups/tooltips (xdg_popup on layer surfaces) (M4)
- [ ] Text input — cosmic-text editor; IME deferred, recorded in
      DESIGN.md (M5)
- [ ] Virtualized lists (launcher-scale) (M5)
- [ ] Session lock surfaces (ext-session-lock) (M5)
- [ ] Process/Socket/FileView-equivalent io primitives (M6)
- [ ] Generic D-Bus from Lua — exceeds QuickShell's surface (M6)
- [ ] LuaJIT FFI + `ui.canvas` — the over-the-ceiling hatches QML
      doesn't have (M6)
- Conceded, permanently: widget shaders/blur (compositor's job),
  WebEngine-style embedding, Qt component breadth (see DESIGN.md
  Deferred)

### vs the memory goal (the discipline)

- [ ] RSS measured in CI from M0 on (smem in the flake check; fail
      over budget: 20 MB bare, 25 MB full bar, 40 MB hard)
- [ ] Zero idle wakeups: frame callbacks requested only while dirty;
      calloop timers only while a `shell.interval` exists
- [ ] Zero steady-state subprocesses (M3 accept)
- [ ] Glyph/scale cache budget: cosmic-text caches are the dominant
      allocation — measure before optimizing

## Interconnection tracker (mirrored in tomoe PLAN.md)

- [x] M3 §1: tomoe compositor backend landed 2026-07-08 —
      `$TOMOE_SOCKET`/derived-path discovery via the `tomoe-ipc` git
      dep (pinned rev in Cargo.toml; fetch hash in flake
      `cargoLock.outputHashes`), `subscribe` stream (`window_open/
      close`, `focus_change`, `wm_state`). The workspace vocabulary is
      **`wm_state`** — `{ active, workspaces = { { id, windows },
      … } }` — served *and* broadcast by tomoe's `wm.lua` (its commit
      45aea74): policy rides the user event vocabulary, the wire crate
      stayed frozen — doctrine 03's split held on first contact. Known
      gap (tracked in tomoe PLAN.md): no core event for title changes
      after window_open, so a focused title can go stale.
- [x] M2: shared Lua conventions doc landed 2026-07-08 as
      `~/Dev/doctrines/conventions/lua.md` (API global, settings tables,
      module shape, `on_*` naming, `Mod`, the reload contract) — tomoe
      and moonshell both cite it instead of restating.
- [ ] ~~M3 §7: check whether tomoe's IPC listener accepts **multiple
      concurrent clients**~~ — **superseded by FUSION.md**: the native
      shell no longer connects over IPC; the multi-client question
      remains a tomoe-ipc concern for external clients (re-home to
      tomoe PLAN.md if it itches).
- [ ] ~~post-M3: tomoe ships a default moonshell bar config as content;
      combined home-manager module composes both flakes~~ — **superseded
      by FUSION.md**: the bar is a builtin Lua module in-process (F2);
      one home-manager module, one flake (F6).
- [ ] ~~M3+: taskbar widget rides ext-foreign-toplevel-list-ish data per
      compositor~~ — **superseded by FUSION.md**: on tomoe the taskbar
      reads wm state in-VM (F2); external taskbars keep
      wlr-foreign-toplevel-management.

## Milestone order & first steps

M0 → M6 as in DESIGN.md. M0 breakdown (the doctrine-06 spike):

1. [x] Flake: `nix flake check` runs build+tests, fmt, clippy
   (-D warnings), and `boot` — headless sway (unwrapped; the wrapper
   needs a dbus session) + `moonshell --boot-check` + a 20 MB RSS gate.
   The sandbox has no fonts; the check writes a `FONTCONFIG_FILE`
   pointing at dejavu so the text path is exercised (fontdb honors it).
2. [x] `surface`: registry/seat/output binds; top-anchored layer
   surface with exclusive zone; `SlotPool` double buffer; `Painter`
   trait is the mechanism/policy boundary (returns `Damage`).
3. [x] `render`: tiny-skia clear/rect + cosmic-text line into ARGB8888
   (R<->B swizzle once in `Rgba::to_skia`); integer-physical sizing;
   fontless systems skip text instead of panicking.
4. [x] Damage: painter-reported rects → `damage_buffer`; frame callback
   requested only when a commit is in flight; fully idle = zero
   scheduled wakeups.
5. [x] Measured 2026-07-08: idle RSS 6.9 MB live (6284 kB in the CI
   gate), 0 voluntary ctx switches over 5 s idle. powertop/smem not
   installed — /proc VmRSS + ctx-switch delta are the standing proxies.

M1 breakdown (element vocabulary; all in `render`, no Lua):

1. [x] Element tree + flex-lite layout + draw pass (2026-07-08):
   `element.rs` (HBox/VBox/Stack/Text/Spacer/Separator/Progress/
   CircularProgress — every variant carries a uniform `Style`; doctrine
   05 shape documented in the module header), `layout.rs` (measure +
   place: flex-grow semantics, gap/padding/justify/align, logical→
   physical scale multiplied exactly once), `draw.rs` (lockstep walk,
   edge-rounding to integer px, rounded rects + bezier arcs in the
   Renderer). Bare binary renders its version bar through the tree —
   the doctrine-06 artifact now exercises the M1 path.
2. [x] `icon` (SVG via resvg) + `image` elements (2026-07-08):
   `assets.rs` `AssetCache` inside `Renderer` — pixmaps cached per
   (source, physical size, tint), misses negative-cached, stored
   premultiplied in buffer byte order so `blit` is a plain src-over.
   Icon contract from nur: `path` > XDG theme lookup (`{name}.svg` in
   hicolor/Adwaita/breeze scalable) > name-as-text fallback; tint
   keeps alpha, replaces color. Image intrinsic size = native file px
   1:1 (crisp), style overrides rescale (bilinear). Deps: resvg 0.47
   (default-features off — matches tiny-skia 0.12), image 0.25
   (png+jpeg only).
3. [x] Per-element damage diffing (2026-07-08): `scene.rs` `Scene`
   caches the previous element+layout trees; diff walks both pairs in
   lockstep (equal subtree → skip; equal container shell + rect →
   recurse; else damage old+new subtree *bounds* — children can
   overflow containers). Rects use the draw pass's edge rounding,
   +1 px inflation (AA/glyph-overhang insurance), canvas clamp,
   overlap coalescing → `SceneDamage::{None,Full,Rects}`. Canvas is
   still repainted in full when damage ≠ None (buffers alternate;
   partial repaint needs per-slot buffer-age tracking — deferred until
   profiling demands it); identical tree + geometry early-outs before
   layout, so steady state does zero shaping work. `surface` grew
   `Canvas.fresh` (no committed content at this buffer size: first
   draw/remap/resize) — painter invalidates its scene, damage upgraded
   to Full.
4. [x] Accept (2026-07-08): `crates/moonshell/examples/simple_bar.rs` —
   nur's simple-bar tree as a static Rust table (mapping notes in the
   file header: `items_center` → `align: Center`, `fill` → `grow: 1.0`,
   `button` → styled hbox shell, theme tokens from nur's Mocha).
   Renders correctly on the live tomoe session across all three
   regions (grim-verified at 5120×1440). **Pixel A/B against
   nur-on-GPUI is blocked on this machine**: nur has no tomoe
   compositor backend, so its render callback dies and the bar maps
   blank — the very gap moonshell M3 exists to fill; parity was
   checked against nur's source-level layout semantics instead
   (`crates/runtime/src/bridge/element.rs`). Revisit pixel A/B under a
   shared compositor if it ever matters. Shook loose: icon
   name-fallback text overflowed its box and overpainted siblings
   (also outside the reported damage bounds) — `text_line` grew a
   `max_w` clip, fallback now fits the box (regression test
   `icon_fallback_text_clips_to_box`). Measured: 14.5 MB RSS release,
   0 voluntary ctx switches / 5 s.

M3 breakdown (services, natively):

1. [x] compositor service + tomoe backend (2026-07-08).
   `crates/services`: Lua-free, doctrine-05 shape — one plain state
   struct + one `start(handle, notify)` per service; backends are
   calloop sources, never threads. `compositor::detect()`: tomoe
   (`$TOMOE_SOCKET`, else `tomoe_ipc::find_socket()` path exists) >
   Hyprland > niri > Sway (nur's precedence); non-tomoe backends warn
   until §2. tomoe backend (`compositor/tomoe.rs`): `subscribe` sent
   *first*, then `windows` + `wm_state` snapshot requests — racing
   events apply on top, never lost; nonblocking `UnixStream` as a
   calloop `Generic`; pure `Model` (frames in → state out) unit-tested
   apart from the wiring; disconnect resets to `connected = false`,
   notifies, arms a 2 s retry timer (the only periodic wakeup, only
   while disconnected — and only for IPC-socket loss; losing the
   *Wayland* compositor kills the bar with it, correctly). Bridge:
   `runtime::services_bridge::push_compositor` — the one place service
   state crosses into Lua — calls `shell.services.compositor:set(snap)`
   so widgets ride the ordinary `shell.state` path; the binary's
   notify closure stores the snapshot on `Engine` and re-seeds each
   fresh VM *before* config exec (same contract as displays).
   Snapshot adds `connected` + per-workspace `windows` count to nur's
   shape; workspaces widget now highlights active (theme accent) and
   defaults to occupied-only (`show_empty` opts it out). Read path
   only — service *actions* (focus_workspace) stay placeholder until
   the write path lands with M4 clicks. tomoe side: `wm.lua` serves +
   broadcasts `wm_state` (vocabulary above). Verified: live session
   (degraded path: no `wm_state` → warn once, core events still track
   focus titles) and nested tomoe (workspace switch/move, counts,
   disconnect → reconnect, hot-reload re-seed). Measured: 7.7 MB RSS
   release, 0 voluntary ctx switches / 5 s idle while connected.
2. [x] niri, Hyprland, Sway backends (2026-07-08) — nur's *logic*
   ported onto event sockets; nur's *implementations* (threads + the
   `niri-ipc`/`hyprland`/`swayipc` crates — the `hyprland` crate drags
   tokio, a locked-decision violation) were not. All three are
   hand-rolled protocol clients in the tomoe backend's shape:
   nonblocking socket as a calloop `Generic`, `serde_json::Value`
   parsing (tolerant of version skew — unknown events skip instead of
   failing a typed deserialize), retry timer only while disconnected,
   notify only when the mapped snapshot changed. Shared plumbing
   extracted to `compositor/wire.rs` (`read_available`, `take_line`,
   `arm_retry`, `RETRY`); tomoe backend refactored onto it — one
   mechanism, four consumers (doctrine 05). Per backend: **niri** =
   `"EventStream"` request on `$NIRI_SOCKET`, then ndjson events into
   a pure `Model` (workspaces BTreeMap + windows map + focused id;
   initial burst is just events — no snapshot requests); **Hyprland**
   = `.socket2.sock` event lines classified by `relevant()`, any hit
   coalesces (per wakeup) into one re-fetch of `j/workspaces` +
   `j/activeworkspace` + `j/activewindow` over short-lived blocking
   socket1 connections (1 s timeout — Hyprland closes after each
   reply; no line carries enough state to track incrementally);
   **sway** = i3 binary framing hand-rolled (`frame`/`take_frame`/
   `read_frame`), one subscribe connection (`workspace`+`window`),
   re-fetch `GET_WORKSPACES` + `GET_TREE` per event batch; the tree
   walk counts leaf views per workspace and takes the focused view's
   title — fixing a nur bug where a focused *empty workspace* node
   became the "window title". Initial seeding rides a calloop
   `Timer::immediate` because `notify` needs the loop's `&mut D`
   (fetch-based backends have no connect-time frames to piggyback on).
   Verified live: **niri** (nested under headless sway — workspace
   switch, spawn/close window counts, title changes including
   post-open retitles; 7.1 MB RSS release, 0 voluntary ctx switches /
   5 s idle) and **sway** (headless — same drill; compositor death
   correctly takes the bar down with it); **tomoe** regression-checked
   on the live session post-refactor. **Hyprland: unit tests only**
   (`parse_state`, `relevant`, socket-dir discovery paths) — nested
   Hyprland crashes at boot in this environment (AsyncResourceGatherer
   abort, both under headless sway and on tomoe); flag for a first
   real Hyprland session before calling M3 §7 acceptance.
3. [x] battery (2026-07-08): UPower over the system D-Bus, sysfs
   fallback; replaces the placeholder facade through the same `:set()`
   path. **D-Bus = rustbus 0.19, not zbus** — zbus structurally needs
   an async executor (async-io reactor thread or tokio), forbidden by
   the single-thread locked decision; rustbus is pure-Rust/sync,
   exposes the socket fd, and `refill_all()` drains nonblocking, so
   the bus is a calloop `Generic` on a dup'd fd (the watcher's
   technique). DESIGN.md locked-decisions row revised. Shape mirrors
   the compositor backends: pure `Model` (properties in, snapshot
   out) unit-tested apart from the wiring, incl. one round-trip test
   marshalling a real `PropertiesChanged` (`s a{sv} as`) through
   rustbus's dynamic params API. Setup is the only blocking IO
   (connect + AddMatch + GetAll, 2 s timeout), match registered
   *before* the snapshot request (events racing the snapshot apply on
   top). Everything rides UPower's aggregate `DisplayDevice` — one
   number, multi-battery pre-combined, hotplug arrives as `IsPresent`
   flips (no DeviceAdded tracking). Snapshot = nur's shape +
   `available`; when `IsPresent = false` (desktops report
   `Percentage = 0`) the published snapshot keeps nur's render-safe
   defaults (100%/false) and the bundled widget renders nothing
   (returns an empty hbox — nil would truncate a children list). Bus
   lost mid-flight → reset + degrade to sysfs. Fallback: first
   `/sys/class/power_supply/*` with `type = Battery`, 30 s calloop
   timer — sysfs emits no inotify events, so “sysfs + inotify” was a
   fiction; the timer only exists on UPower-less machines that have a
   battery (no battery + no UPower = no source, zero wakeups). Engine
   caches the snapshot and re-seeds fresh VMs before config exec
   (compositor contract). Verified live on the UPower path (upowerd,
   no battery — `available=false`, GetAll parse, reload re-seed);
   **17.8 MB RSS release, full bar + 1 Hz clock, no added wakeups**
   (14 vol. ctx switches / 5 s ≡ the clock's cadence). Laptop
   verification (real Percentage/State changes, sysfs fallback)
   flagged for a battery machine — same posture as Hyprland in §2,
   fold into §7 acceptance.
4. [x] network + mpris (2026-07-08), both on shared rustbus plumbing
   lifted out of battery into `dbus.rs` (`Dbl`, `reply_ok`, `send`,
   `get_all`/`get_one`, `SETUP`, the wire-level test-message builder —
   the §3 “lift it out when a second service needs it” note, done).
   New trick over §3: *runtime* request/reply without blocking — send
   the call, remember `(serial, what-was-asked)`, resolve via
   `try_get_response` on the next fd wakeup, and guard every reply
   against the state having moved on. **network**: NM object chain
   root → PrimaryConnection → AccessPoint; one `path_namespace` match
   rule covers all three; chain re-roots (connection switch, AP roam)
   clear downstream state immediately and re-query async;
   `NameOwnerChanged` re-seeds across NM restarts; connected = `State
   ≥ 50` (carrier semantics ≡ nur's operstate). Fallback: sysfs
   operstate 30 s poll (link only — SSID needed nmcli in nur too);
   none only if `/sys/class/net` is unreadable. **mpris**:
   playerctld-style tracking, event-driven — every player signal
   bumps an activity counter, snapshot = freshest Playing player,
   else freshest; discovery = `ListNames` at setup +
   `NameOwnerChanged` (`arg0namespace`) at runtime; one
   `path='/org/mpris/MediaPlayer2'` rule catches every player's
   `PropertiesChanged`/`Seeked`, keyed by unique-name sender.
   `Position` never signals: re-queried at status/track transitions,
   set by `Seeked`, frozen between — live progress bars interpolate
   in Lua. Actions stay placeholder until the write path (M4).
   Verified live: ethernet (`connected=true, ssid=nil`), Firefox
   player metadata/status flips via busctl PlayPause (position exact
   at each transition), playerctld activation/kill = player
   add/remove + fallback. **9.6 MB RSS (release, test bar, all three
   D-Bus services connected), 0 voluntary ctx switches / 5 s idle**
   — the buses only wake the loop when something changes. WiFi
   ssid/strength verification flagged for a wireless machine (§7,
   same posture as §2 Hyprland / §3 laptop). Shook loose (fixed in
   this step): the store-symlinked-config watcher incident — see the
   standing lessons — and a flag: a second moonshell got ECONNREFUSED
   from the live tomoe IPC socket while the session bar existed;
   check whether tomoe's IPC accepts multiple clients (§7 /
   interconnection tracker).
5. [ ] audio — decide the native path (PipeWire native protocol vs
   wireplumber): zero steady-state subprocesses is the constraint.
6. [ ] notifications daemon + SNI tray + power-profiles (rustbus —
   serving methods = replying to METHOD_CALL messages on the same
   fd source; org.freedesktop.Notifications / StatusNotifier);
   system_tray + bar_overlay widgets become portable.
7. [ ] acceptance + wind-down decision: widget parity vs nur, both-
   compositor live test, execsnoop-clean 5 min, RSS ≤ 25 MB full bar.

M2 breakdown (the Lua runtime):

1. [x] `runtime` crate: VM + element bridge + `ui.*` stdlib
   (2026-07-08). mlua 0.10, `luajit`+`vendored` (the FFI escape hatch
   is the locked decision; vendored → single binary, no system Lua).
   `Vm` owns the `Lua`; one `Vm` per config lifetime — hot reload (§5)
   drops it wholesale, nothing leaks across reloads. `element.rs`
   `from_table` is the doctrine-05 fourth arm: nur's element-table
   contract → `render::Element`, with per-element default merging
   (`apply_style` over the variant's `Default`), `fill`→`grow` mapping
   (numeric `grow` accepted, wins), hbox/button defaulting
   `align=center` (nur's unconditional `items_center`), `button`
   parsed as its M4 visual shell (handlers ignored), scroll/slider/
   input rejected with milestone-naming errors, unsupported style
   props accepted-and-ignored so nur configs load. `TextDefaults`
   threads window `fg`/`font_size` inheritance through the parse — §3
   fills it from `shell.window` opts, no API break. Stdlib ported
   minus `ui.bar_layout` + the theme-aware `shell.window` wrapper
   (both need `moonshell.theme` and the `shell` global — §6).
2. [x] `surface` multi-window (2026-07-08): `Shell` (the calloop
   dispatch data) owns the connection, globals, one shared recycled
   `SlotPool`, and a `BTreeMap<WindowId, Window>`; per-window
   `LayerOptions` in surface's own vocabulary (`Layer`/`Anchors`/
   `Keyboard`/`Margins` — no SCTK types on the public API), per-window
   painter + configure/scale/damage/frame state. `Shell::connect()`
   returns the shell + its event loop so callers insert their own
   sources (§4 timers, §5 inotify) with `&mut Shell` dispatch data;
   `create_window`/`destroy_window`/`mark_dirty`/`mark_all_dirty`/
   `quit` work at config time and from inside source callbacks alike.
   M0 lessons preserved per window: close-remap gated on
   was-configured + output-exists, `new_output` remaps all unmapped,
   frame callbacks only while a commit is in flight. Boot check =
   every window drew once. `examples/two_bars.rs` is the acceptance
   fixture (two bars, timer destroys one from a callback, then quits —
   the exact §3/§4 access pattern); verified exit-0 under headless
   sway and the live tomoe session; flake `boot` gate still green.
3. [x] `shell.window` + `shell.state` + render callbacks in the
   binary (2026-07-08). Lua never touches `surface::Shell` (no stable
   `&mut` exists while Lua runs): `runtime::api::ShellCtx` is the
   action queue — `shell.window` pushes a `PendingWindow` (parsed
   `LayerOptions` + `Rc<RefCell<WindowShared>>` carrying the render
   key and paint defaults), `state:set`/`handle:render` raise a dirty
   flag; the binary drains both after config exec and once per loop
   pass via the new `Shell::run_with(event_loop, tick)` hook.
   `LuaPainter` (in `runtime`) calls the render fn at paint time,
   parses the table with the window's `TextDefaults` (`fg`/
   `font_size`), wraps it in a full-canvas Stack painting the window
   `bg`, and feeds the shared `Scene`; a failing callback logs and
   paints bare bg (a remapped surface must still get a buffer). One
   `Renderer` is shared `Rc<RefCell<…>>` across all windows — the
   font system is the dominant allocation, one copy not N. Window
   opts speak nur's vocabulary (bar mode `position`/popup mode
   `anchor`+`popup_width`+margins, `layer`, `keyboard`, `exclusive`,
   `bg`/`fg`/`font_size`, `transparent`); divergences: bars stretch
   via anchor (size 0) instead of reading display bounds — multi-
   monitor correct — and unknown `anchor` strings error instead of
   silently becoming top-right. Config resolution: `--config` >
   `$MOONSHELL_CONFIG` (both must exist) > `$XDG_CONFIG_HOME`/
   `~/.config/moonshell/init.lua` (optional) > bare version bar.
   Verified live on tomoe (grim: text, colors, spacer layout correct;
   `--boot-check` exit 0 with a config): **2.3 MB RSS release, 0
   voluntary ctx switches / 5 s.**
4. [x] timers & process (2026-07-08): `shell.interval`/`once` queue
   `PendingTimer`s (WeakLua + registry key — a live timer can't keep a
   dropped VM alive; fire-after-drop asks calloop to remove the
   source, so §5's reload is self-cleaning); the binary's drain arms
   them as calloop `Timer` sources — armed only while one exists (the
   zero-idle-wakeup gate), interval period clamped ≥ 1 ms, callback
   errors logged but keep the timer. `shell.exec` = blocking `sh -c`
   trimmed stdout (nur's contract); `shell.exec_async` = named worker
   thread → calloop channel; the reply is plain Send data (id +
   output), the Lua half lives in `ShellCtx.exec_callbacks` keyed by
   id. `shell.quit` = ctx flag; `run_with` reordered to
   tick→redraw→exit-check→dispatch so a config-time quit can't block
   on an event that never comes. `shell.get_window(name)` = registry
   of `WindowShared` by explicit `name` (unnamed windows aren't
   registered; handle methods beyond `:render` — nur's `close`/`hide`/
   `show`/`toggle` — deferred until the visibility story exists in
   `surface`, tracked for §6/M4). `shell.displays()` reads a snapshot
   the binary refreshes each drain *and* before config exec —
   `Shell::connect` now does two roundtrips so output geometry exists
   by then (`surface` grew `DisplayInfo` + `Shell::displays()`,
   xdg-output logical size preferred, mode/scale fallback). nur's
   `shell.clipboard_*` needs a data-control protocol on layer
   surfaces — deferred to M4, recorded here. Measured live on tomoe:
   2.2 MB RSS idle + 0 voluntary ctx switches / 5 s (no timers),
   6.6 MB with a 1 s clock (glyph-cache growth from digits).
5. [x] hot reload (2026-07-08): `watcher.rs` (binary) owns one inotify
   instance (`inotify` 0.11, default-features off — no tokio/stream),
   inserted as a calloop `Generic` on a *dup'd* fd so source teardown
   order can't invalidate it. Config tree = every non-hidden dir under
   the config file's parent (canonicalized in `resolve_config`), new
   subdirs picked up from CREATE events, dir watches never removed
   (bounded by distinct dirs ever seen — documented tradeoff). A
   `.lua` change debounces (100 ms one-shot timer, throttle-style)
   into `ctx.request_reload()`; the tick's reload destroys Lua-created
   windows (tracked in `Engine.windows` — the old drain closure became
   `Engine`, which owns the `Vm` so it can swap it), clears file
   watches, `ctx.reset_for_reload()` (pending/timers/watches/named/
   exec-callbacks/dirty; the exec channel, displays, quit survive —
   they belong to the loop), then fresh VM + re-exec. *Boot*-time
   config errors still fail hard; *reload*-time errors log and leave
   the shell windowless with the watcher alive — the next save
   retries (verified live: break → alive + error logged → fix →
   recovers). `shell.reload()` raises the same flag; `shell.
   watch_file(path, fn)` queues a `PendingWatch` (WeakLua callback,
   timer discipline) that the drain registers — the watcher watches
   the *parent dir* and matches by canonicalized full path, so the
   editor rename-replace dance can't kill it. inotify init failure
   degrades to manual `shell.reload()` with a warning. Measured live
   on tomoe (release): 7.3 MB RSS idle, 0 voluntary ctx switches /
   5 s, 10 successive reloads with zero RSS growth.
6. [x] stdlib completion + acceptance (2026-07-08): `theme.lua`/
   `utils.lua`/7 widget modules ported to `lua/moonshell/` (utils:
   `math.tointeger` guarded — LuaJIT is 5.1-based; integral floats
   already print bare). `Vm::new` registers every bundled module in
   `package.preload` under `moonshell.*` *and* a `nur.*` alias whose
   loader is `require("moonshell.X")` — one shared instance, so a nur
   config's `theme:set(...)` drives the same table the `shell.window`
   wrapper reads (regression test `theme_set_flows_into_window_
   defaults`). `shell`-dependent policy (theme-aware `shell.window`
   wrapper, `shell.services`) lives in `shell_ext.lua`, loaded by
   `install_shell` after the Rust API registers — stdlib.lua stays
   VM-boot-time (`ui.bar_layout` requires the theme lazily).
   `moonshell/services.lua` is the doctrine-05 facade declarator:
   `services.define(name, initial, actions)` → nur's handle shape
   (`:get/:set/:map/:subscribe` over `shell.state` + named actions);
   M2 ships sysinfo/compositor/battery/network/audio/mpris as static
   placeholders (actions warn once to stderr), M3 replaces the backing
   through the same `:set()` path. Acceptance: nur's simple-bar
   init.lua vendored byte-for-byte at `examples/simple-bar/` (flake
   fileset grew `./examples`), exercised in CI down to the parsed
   element tree. Live: 17.4 MB RSS release, wakeups only from the 1 Hz
   clock, live-edit reload verified, RSS stable.

Nothing Lua-shaped was built in M0/M1 — the render core was provably
tiny before the runtime landed on top.

## Standing lessons (imported)

- From tomoe: never regenerate buffers per frame (false-damage redraw
  storms — `ref/ShojiWM/knowledges/tty-backend-notes.md` via tomoe);
  integer-physical pixel discipline (tomoe DESIGN.md coordinate
  doctrine) — CPU rendering makes misalignment *blurry text*, the one
  unforgivable sin in a bar
- From nur: keep Lua-facing functions `LuaResult`, convert to anyhow at
  one boundary; store `LuaRegistryKey` (’static), never `LuaFunction`,
  for callbacks that outlive the stack frame — these survive the GPUI
  removal
- From nur's TODO list: `watch_file`/hot-reload was the most-wanted
  missing feature — it's in M2, not later
- From M0: cosmic-text 0.19 `set_text` only marks dirty — call
  `shape_until_scroll` before `layout_runs`/`draw`, or advance is 0 and
  nothing renders (the unit test exists because the live bar shipped
  blank)
- From M0: never remap unconditionally in `LayerShellHandler::closed` —
  a compositor with zero outputs closes the new surface immediately and
  the create/close loop storms (~150k remaps/s observed). Remap only if
  the old surface was ever configured and an output exists; otherwise
  wait for `new_output`
- From M1 §1: round layout rect *edges* (x0/x1), not (x, w) — rounding
  width opens one-pixel seams between adjacent children. Text is
  measured with the same `shape()` the draw pass uses, so layout and
  paint can never disagree about advance width. Layout stays f32;
  integer conversion happens once, in `draw.rs`
- From M1 §2: cached asset pixmaps live pre-swizzled ([B,G,R,A]) and
  premultiplied, so src-over blitting is channel-agnostic — swizzle
  exactly once at decode/rasterize time, mirroring `Rgba::to_skia`.
  resvg minor versions pin tiny-skia minors (0.47 ↔ 0.12); bump them
  together
- From M1 §4: text drawn by elements must stay inside their layout
  rect — the damage diff reports subtree *bounds*, so any overpaint is
  also un-damaged pixels the compositor will trust; clip at the draw
  call (`text_line(max_w)`), don't rely on measure == paint width for
  fallback/degenerate content
- From M1 §4: `pkill -f` patterns in dev scripts must not match the
  invoking shell's own command line (quote a bracketed char:
  `[s]imple_bar`)
- From M2 §6: the same applies to *measurement* — `pgrep -f` can match
  a wrapping `bash -c` line whose /proc numbers look plausible (a
  2.3 MB "RSS" was the wrapper shell); match on the binary path
  (`target/release/[m]oonshell`) and sanity-check ctx switches against
  expected timer cadence before recording
- From M2 §1: mlua's vendored LuaJIT (`luajit-src`) shells out to
  `make`/`cc` at build time — builds must run inside the devshell/
  sandbox (stdenv provides both); a bare `cargo build` on the host
  fails in `luajit-src` with a bare `NotFound`
- From M2 §1: the bridge speaks *nur's* contract even where render
  defaults differ (separator defaults horizontal in the bridge,
  vertical in `render`; progress `bg` is the *track*, pulled back out
  of the parsed style) — compat lives in one layer, `render` keeps its
  own sensible defaults
- From M2 §1: Lua color strings (`"#rrggbb"`) inside Rust raw strings
  need `r##"..."##` — `"#` terminates a plain `r#"..."#` literal
  mid-string
- From M2 §3: mlua 0.10's `Lua` is `Clone` + `'static` — painters (and
  anything living *outside* the VM) may hold `Lua` clones, but
  callbacks stored *inside* the VM (state subscribers) must go through
  `WeakLua` + `LuaRegistryKey`, or the strong cycle keeps the VM alive
  across hot reload
- From M2 §4: `WeakLua` (and the Lua half of any callback) is `!Send`
  — whatever crosses a thread boundary must be plain data; ship an id
  + payload over the channel and keep the WeakLua/registry-key half in
  a loop-thread map keyed by that id
- From M2 §4: anything Lua can request at config time must be
  satisfiable before the event loop runs — `shell.quit` needed the
  loop reordered (tick before dispatch), `shell.displays` needed
  roundtrips inside `Shell::connect`; audit new `shell.*` calls for
  the "top-level call, no loop yet" case
- From M3 §1: `pkill -f` self-matching, part two — it's not enough to
  bracket the pattern (`[t]omoe`): if the *same* bash -c command also
  launches the target, the raw launch string sits in the shell's own
  cmdline and pkill kills the invoking shell mid-script (observed as
  silent no-output tool calls). Keep launch and kill in separate
  commands, or launch through a helper script
- From M3 §1: calloop's `NoIoDrop<T>` has no `DerefMut` — read Unix
  sockets in `Generic` callbacks through `Read for &UnixStream`
  (`(&**stream).read(…)`)
- From M3 §1: a git dep needs its fetch hash in **both** flake spots
  (`cargoLock.outputHashes` for buildRustPackage *and* the clippy
  check's `importCargoLock`) — factor one `cargoLock` attrset
- From M2 §5: never inotify-watch a file directly — editors save by
  rename-replace and the watch dies with the old inode; watch the
  parent dir and match events by canonicalized full path
- From M2 §5: every calloop source sharing loop state via
  `Rc<RefCell<Engine>>` is safe only because Lua callbacks touch
  `ShellCtx` (a separate Rc), never `Engine` — a `shell.*` function
  that reached back into `Engine` would re-borrow and panic; keep the
  action-queue indirection when adding API
- From M3 §2: stale socket *files* defeat path-exists detection — a
  dead nested tomoe left `tomoe.wayland-2.sock` behind and `detect()`
  picked tomoe while running under sway (connect then refused,
  retrying forever). Unix sockets don't unlink themselves; rm stale
  test sockets, and treat exists() as a hint, not proof of life
- From M3 §2: when a backend must *fetch* its initial state (no
  connect-time frames to ride), deliver it through a calloop
  `Timer::immediate` — `notify` needs the loop's `&mut D`, which never
  exists outside a source callback
- From M3 §2: nested-compositor test recipe — headless sway (pixman)
  hosts niri fine (winit + software EGL); a niri window opened on the
  *live* session dies in seconds (something closes it); Hyprland
  refuses to nest at all here. `niri msg` has no `--socket` flag —
  export `NIRI_SOCKET`
- From M3 §3: zbus cannot run single-threaded — its connection needs
  an executor ticked by an async-io reactor thread or tokio; when a
  "D-Bus as a calloop source" is required, rustbus is the shape that
  fits (sync, fd-exposed, `refill_all()` nonblocking drain). Its typed
  layer has no `f64` — doubles need a local newtype unmarshalling via
  `u64` bits (`Dbl` in battery.rs; lift it out when a second service
  needs doubles)
- From M3 §3: sysfs attribute changes do not fire inotify — "watch
  sysfs with inotify" is a design fiction; the honest fallbacks are a
  coarse timer or a netlink uevent socket (AF_NETLINK
  KOBJECT_UEVENT — upgrade path if the 30 s poll ever matters)
- From M3 §4: **never canonicalize a config path used as a watch
  root or reload source** — a home-manager config is a symlink into
  /nix/store, so canonicalizing made the watcher recurse the entire
  store (~523k watches ≡ the whole per-user inotify budget — every
  *other* process's inotify then fails ENOSPC, which is how it
  surfaced: unrelated watcher tests broke) and pinned reloads to the
  immutable old store file so config switches never applied. Symlink
  identity is load-bearing: `std::path::absolute`, not
  `canonicalize`; plus a 4096-dir hard cap in the watcher as
  insurance
- From M3 §4: runtime D-Bus request/reply on a calloop source =
  `send` + remember `(serial, what)` + `try_get_response` after
  `refill_all()` — and every reply must be guarded against the state
  having moved on (chain re-rooted, player quit) before applying
- From M3 §4: MPRIS `Position` never emits `PropertiesChanged` —
  poll-free position = re-query at status/track transitions + apply
  `Seeked`; anything smoother is Lua-side interpolation, not a timer
  in the service
- From M3 §4: `grep voluntary_ctxt /proc/pid/status` also matches
  `nonvoluntary_ctxt_switches` — anchor it (`^voluntary_ctxt`) or the
  wakeup numbers are garbage
- From M3 §3: a service snapshot should stay *render-safe* when the
  underlying thing is absent (no battery ⇒ keep 100%/false + an
  `available` flag), and "hide the widget" is Lua policy on that flag,
  not a core decision — widgets return an empty box, never nil (a nil
  in a children list truncates it)
- From M1 §3: shm buffers alternate, so a partial-damage frame must
  either fully repaint (current: correctness by determinism —
  over-reported damage is always safe) or track per-slot buffer age
  before clipping the repaint. And a remapped/resized surface has no
  content to diff against — any frame-diff cache needs an invalidation
  signal from the surface layer (`Canvas.fresh`), or the first paint
  after remap reports `None` and the surface never gets a buffer
