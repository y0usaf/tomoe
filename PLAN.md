# tomoe — parity plan

Working tracker toward the DESIGN.md vision: **niri's performance,
Hyprland's features, ShojiWM's configurability**. DESIGN.md holds doctrine
and locked decisions; this file holds the concrete gap list and the order
we close it in. Check items off as they land; re-audit against `ref/`
when a milestone completes.

## Where we are (2026-07)

Done and working:

- winit + TTY (DRM/GBM/libinput/libseat) backends, VT switching,
  multi-output (side-by-side), per-display mode selection,
  explicit sync (`backend/tty.rs`)
- Multi-GPU (niri-shape: every seat GPU opened, rendering pinned to the
  primary render node via `GpuManager`, cross-device copies for scanout;
  `--drm-device` overrides the render GPU) and output/GPU hotplug
  (udev → `DrmScanner` diff → connect/disconnect → `outputs_changed`);
  zero connected outputs is a wait-state, not an error
- niri-style per-output redraw state machine + estimated-vblank timer,
  damage-tracked rendering, frame-callback throttling
- Physical-first coordinate doctrine (`coords.rs`), fractional-scale +
  viewporter, dmabuf, layer-shell, xdg-decoration (+ KDE legacy),
  data-device/clipboard, primary selection, xdg-output,
  pointer-constraints + relative-pointer, presentation-time
- Lua runtime: binds (Mod-agnostic), spawn, settings (gaps/scale/border/
  displays/focus-follows-mouse), window handles + queued ops, hooks
  (open/close/focus/outputs/pointer button+axis, hover enter/leave),
  pointer grabs, world camera (`set_view`), hot reload (fresh VM +
  `on_window_open` replay), watchdog
- Dogfood WMs: `wm.lua` (9 workspaces, dwindle, focus cycling),
  `zoomer.lua` (pan/zoom canvas, planes, drag move/resize)
- Compositor UI: hotkey overlay, exit confirm, config-error banner;
  xcursor themes; no-hooks full-screen fallback
- XWayland via xwayland-satellite (niri-shape: tomoe owns the X11
  sockets/lock file, exports `DISPLAY` at startup, spawns satellite
  on-demand via `-listenfd` on first connection, respawns on next
  connection after a crash; `xwayland.rs`)
- Capture: wlr-screencopy v3 (shm + dmabuf, `copy` immediate,
  `copy_with_damage` queued per manager and completed from the redraw
  loop) and ext-image-copy-capture-v1 + ext-image-capture-source-v1
  (smithay handlers; output sources, session constraint renegotiation on
  output changes); both render through one shared path in `capture.rs`
  on the primary GPU
- ScreenCast portal: `xdg-desktop-portal-tomoe` binary (zbus + pipewire,
  ShojiWM-shape: `PW_STREAM_FLAG_DRIVER | ALLOC_BUFFERS`, single thread,
  wlr-screencopy `ready` queues the PW buffer and kicks the next capture
  → vblank-paced); monitor sources, dmabuf with shm fallback, no-GUI
  output choice (`TOMOE_SCREENCAST_OUTPUT` / `TOMOE_PORTAL_CHOOSER` /
  single-output auto); compositor exports `XDG_CURRENT_DESKTOP=tomoe`,
  nix package ships `.portal` + `tomoe-portals.conf` + D-Bus service

## Gap inventory by reference

### vs niri (performance & session correctness)

- [x] presentation-time protocol (feedback rides the DRM frame as user data,
      fired from the vblank with the hardware timestamp; winit approximates
      with the compositor clock at submit)
- [ ] Direct scanout for fullscreen surfaces; hardware cursor plane
      (cursor is composited today — `backend/tty.rs:619`)
- [ ] VRR (adaptive sync) per output
- [ ] Tearing control (`wp_tearing_control_v1`) + async page flips
- [x] Output hotplug (udev → connector scan diff → connect/disconnect,
      reposition, `outputs_changed`)
- [x] Per-output position/mirror/disable (`settings.displays`: explicit
      physical `position`s anchor the layout and the rest packs after
      them; `mirror` maps at its target's position so it shows the same
      world region; `disabled` connectors stay stashed per device so a
      settings change re-enables them without a replug)
- [ ] Per-output scale (single global scale today; mixed-scale placement
      policy noted in DESIGN.md coordinate doctrine)
- [x] pointer-constraints + relative-pointer (games) — niri-shape: lock
      swallows motion, confine clamps at the surface/region edge, both keep
      sending relative motion; smithay deactivates on focus change
- [ ] idle-notify + idle-inhibit
- [ ] ext-session-lock-v1
- [ ] xdg-activation
- [x] Primary selection (focus follows keyboard focus, same as the clipboard)
- [x] libinput device config (tap, accel, natural scroll, DWT, scroll/click
      method…) via `settings.touchpad`/`settings.mouse`, per-device
      overrides in `settings.devices["<libinput name>"]`; unset fields
      revert to libinput defaults so reloads are idempotent
- [x] xkb config (rules/model/layout/variant/options) + key repeat via
      `settings.keyboard`, re-applied on change from any Lua entry
- [ ] Steady-state zoom re-advertises effective fractional scale
      (DESIGN.md camera follow-up)

### vs Hyprland (WM depth & eye-candy)

- [x] Popup grabs (anvil-shape: `grab_popup` + PopupKeyboardGrab/
      PopupPointerGrab, refused when another grab holds the device)
- [x] Fullscreen/maximize/minimize requests plumbed as Lua
      `on_window_request` events, policy decides; native default honors
      fullscreen on the window's output, acks maximize, ignores minimize
- [x] xdg interactive move/resize forwarded to Lua as `on_window_request`
      ("move"/"resize" + edges); a consuming hook takes over via
      `tomoe.grab_pointer` (the core releases the client's click grab),
      unconsumed drags are dropped
- [x] Focus-follows-mouse (`settings.focus_follows_mouse`, sloppy focus,
      no restack) + `on_pointer_enter/leave` hover events, suppressed
      while any pointer grab is active
- [ ] Window rules (`tomoe.rule { app_id = ..., ... }`)
- [ ] Animation engine (springs + beziers on layout positions,
      AnimatedVariable-style; open/close/move/workspace-switch)
- [ ] Rounded corners (shader element, pixel-aligned per doctrine §5)
- [ ] Drop shadows
- [ ] Dual-kawase blur (windows + blur-behind for layer surfaces)
- [ ] Special workspaces / groups — Lua-level per policy split; needs
      mechanism audit (hide/show + per-window state suffice?)

### vs ShojiWM (extension surface)

- [ ] Process API: `tomoe.process.once/service/spawn` declarative
      manifest, diffed by id, restart policies
      (`ref/ShojiWM/knowledges/process-api.md`)
- [ ] IPC: JSON socket + `tomoe msg` CLI + event stream +
      `tomoe.ipc.serve` for user-defined endpoints (bars/launchers)
- [ ] Hot reload with state persistence: `tomoe.on_reload` /
      persist-restore so workspaces survive a reload without replay hacks
      (`ref/ShojiWM/packages/config/src/index.tsx` onEnable/onDisable)
- [x] Request events surfaced to Lua: maximize/minimize/fullscreen via
      `tomoe.on_window_request` (ShojiWM's `onWindow*Request` family);
      activate requests wait on xdg-activation (M5)
- [ ] Input-device change events (per-device *config* landed with M1 §5;
      the add/remove events + device query surface remain)
- [ ] Output reconfigure API (re-run config on hotplug, query available
      modes)
- [ ] LuaLS `---@meta` annotation files shipped for editor DX
- [ ] Layer-surface events (`on_layer_create/update/destroy`) and
      reserved-insets query (usable_area exists; insets breakdown doesn't)
- [ ] Scriptable decorations (long-term; Hyprland-style built-in borders/
      titlebar first, ShojiWM-style Lua-driven SSD tree later — study
      `ref/ShojiWM/knowledges/shared-edge-tree-plan.md` before designing)

### Ecosystem (all three have it)

- [x] **XWayland via xwayland-satellite (expedited → M2)** — landed; the
      rough edge it shook loose was the legacy `wl_drm` global (smithay's
      wl_drm kills Xwayland with a fatal "invalid format" protocol error),
      so `bind_wl_display` is gone from both backends — clients use
      linux-dmabuf. No Lua toggle yet; auto-enabled when the binary
      supports `--test-listenfd-support`
- [x] **Screencopy (expedited → M2)** — wlr-screencopy v3 (niri port) and
      ext-image-copy-capture-v1 (smithay handlers) both landed; verified
      with grim (shm copy) and wf-recorder (damage-queued frames at
      content rate). wf-recorder also forced linux-dmabuf v4 (default
      feedback) on the winit backend — it hard-binds v4
- [x] **Own xdg-desktop-portal backend (expedited → M2)** — landed:
      `crates/xdg-desktop-portal-tomoe`, ScreenCast over PipeWire built
      like ShojiWM's (DRIVER flag, vblank-paced), sidestepping the xdpw
      30fps bug (`ref/ShojiWM/knowledges/screencast-30fps-xdpw-bug.md`).
      Existing wlr portals remain usable as fallbacks alongside it
      (`tomoe-portals.conf` routes only ScreenCast to us)
- [ ] Foreign-toplevel (wlr + ext-foreign-toplevel-list) for bars/docks
- [ ] Gamma control / night light
- [ ] text-input + input-method (IME) — after core parity
- [ ] Touch + tablet-v2 — deferred, no hardware pressure yet
- [ ] Virtual keyboard — deferred

## Milestones (ordered)

### M1 — Finish Phase 3: a WM you can actually use daily

The stubs that break real apps, plus the input plumbing Lua policy needs.

1. ~~Popup grabs~~ done (highest impact: context menus)
2. ~~Fullscreen/maximize/minimize requests~~ done — `on_window_request`
   Lua events with a sane native default (fullscreen honored on the
   window's output); activate requests deferred to xdg-activation (M5)
3. ~~xdg move/resize grab forwarding~~ done — `on_window_request`
   "move"/"resize" events hand the drag to Lua's pointer-grab machinery
   (dogfooded by zoomer's CSD titlebar/edge drags)
4. ~~Focus-follows-mouse setting + `on_pointer_enter/leave` window
   events~~ done — hover diffing in `pointer_moved`; FFM is sloppy focus
   without restacking, hover state holds still during grabs/drags
5. ~~xkb config + libinput config~~ done — `settings.keyboard` (xkb +
   repeat, seat-side so it works on both backends) and
   `settings.touchpad`/`mouse`/`devices` per-device tables (tty backend
   tracks live libinput devices, re-applies on settings change and on
   hotplug/VT re-add)
6. ~~Primary selection; pointer-constraints + relative-pointer~~ done —
   primary selection piggybacks on keyboard-focus changes; constraints are
   enforced in the relative-motion path (lock = relative-only, confine =
   clamp at the edge), activation on motion/creation, cursor position hints
   honored
7. ~~presentation-time~~ done — feedback is the DRM frame's user data,
   presented from the vblank (HwClock when the kernel stamps it); verified
   against weston-presentation-shm on winit

*Accept: daily-drive with real apps — menus, dialogs, fullscreen video,
CSD titlebar drags — all behave; keyboard layout and touchpad configured
from Lua.*

### M2 — Daily-drive blockers: XWayland + screensharing (expedited)

Pulled forward from the old M4: with M1 done, these two are the only
things left between tomoe and full-time use. Session hardening and the
extension surface can wait; a compositor you can't share a screen from
or run X11 apps on cannot be daily driven.

1. ~~XWayland via xwayland-satellite~~ done — sockets/lock owned by
   tomoe, `DISPLAY` exported at startup, on-demand spawn via
   `-listenfd`, spawner thread waits and re-arms the watch on exit.
   What it shook loose: the legacy `wl_drm` global (removed — fatal
   "invalid format" error against Xwayland), verified with xeyes
   rendering through satellite
2. ~~wlr-screencopy~~ done — v3 with shm + dmabuf, immediate `copy` and
   redraw-loop-completed `copy_with_damage` (per-manager queues with own
   damage trackers, sync-fence-deferred `ready`). Verified: grim
   screenshots, wf-recorder records damage-paced frames. Side fix: winit
   backend now offers linux-dmabuf v4 with default feedback (wf-recorder
   binds v4 unconditionally)
3. ~~ext-image-copy-capture-v1~~ done — smithay's image_capture_source +
   image_copy_capture handlers; output sources via WeakOutput user-data,
   shm + dmabuf constraints (render formats of the primary GPU),
   constraint renegotiation and session stop on output changes. Globals
   verified advertised; frame path shares the screencopy render helpers.
   No cursor sessions yet (clients get embedded cursors via
   `paint_cursors`; separate cursor streams can come with the portal)
4. ~~Our own xdg-desktop-portal backend~~ done —
   `xdg-desktop-portal-tomoe` (zbus + pipewire, monitor sources): the
   pipeline is the ShojiWM port (DRIVER + ALLOC_BUFFERS, wayland fd on
   the PW loop, screencopy `ready` → `queue_buffer` → next capture, so
   pacing follows vblank, not the audio quantum); GBM dmabuf buffers
   with memfd/shm fallback; discovery ships in the nix package
   (`tomoe.portal`, `tomoe-portals.conf`, D-Bus activation) and the
   compositor now exports `XDG_CURRENT_DESKTOP=tomoe`. Verified: builds
   through the flake, claims the bus, exports impl.portal.ScreenCast

*Accept: X11 app runs under satellite ✓; grim screenshots ✓; OBS/browser
screenshare captures at monitor refresh via our portal (built + bus
verified; end-to-end OBS fps check pending a real tomoe session), and
at least one third-party portal backend also works end-to-end (pending a
real-session xdg-desktop-portal-wlr run — the protocols it rides are in).*

### M3 — Session hardening (Phase 2 leftovers)

1. ~~Output hotplug end-to-end~~ done — udev → connector →
   `outputs_changed` → Lua reconfigure (ShojiWM's docked-monitor pattern
   is writable now)
2. ~~Per-output position/mirror/disable~~ done — `settings.displays`
   gains `position = {x, y}` (physical), `mirror = "<output>"`, and
   `disabled = true`, all live-reloadable; per-output *scale* still
   open (blocked on the DESIGN.md mixed-scale placement policy)
3. ext-session-lock + idle-notify/idle-inhibit (swaylock/swayidle work)
4. Direct scanout for fullscreen + cursor plane; VRR; tearing control
   (mind `ref/ShojiWM/knowledges/fullscreen-direct-scanout-tearing.md`
   for the NVIDIA/async-flip pitfalls)

*Accept: laptop lid/dock cycles, lock screen, fullscreen game with direct
scanout confirmed via drm_info; no idle redraw storms.*

### M4 — Phase 4: extension-surface parity with ShojiWM

1. Process API (once/service/spawn manifest, restart/reload policies) —
   note: M2's satellite/portal supervision is a natural first consumer
2. IPC socket + `tomoe msg` + event stream + `tomoe.ipc.serve`
3. Hot reload with `on_reload` persist/restore (replace the
   window-replay hack)
4. Window rules
5. LuaLS meta files; example configs exercising all of the above

*Accept: waybar-equivalent driven purely over user IPC; config reload
preserves workspace assignments; services survive and diff correctly.*

### M5 — Ecosystem remainder

1. Foreign-toplevel (wlr + ext-foreign-toplevel-list) for bars/docks
2. xdg-activation
3. Gamma control / night light

*Accept: taskbar sees windows, activation focuses them, night light
works.*

### M6 — Phase 5: eye-candy

1. Animation engine (springs/beziers) driving layout positions + opacity
2. Borders polish, rounded corners, shadows as shader elements —
   pixel-aligned intermediates per coordinate doctrine §5
3. Dual-kawase blur incl. blur-behind for layer surfaces

*Accept: side-by-side with Hyprland defaults, no visible fidelity gap;
UFO test still flat at high refresh with animations running.*

## Standing lessons from `ref/` (re-read before touching these areas)

- TTY loop wake/maintenance ordering:
  `ref/ShojiWM/knowledges/firefox-wayland-display-maintenance.md`
- Never regenerate cursor buffers per frame (false-damage redraw storm):
  `ref/ShojiWM/knowledges/tty-backend-notes.md`
- Redraw-loop spec: `ref/niri/docs/wiki/Development:-Redraw-Loop.md`
- Effect damage/invalidation policy design:
  `ref/ShojiWM/knowledges/effect-invalidation.md`
- Never bind the legacy wl_drm global (`bind_wl_display`): smithay's
  implementation posts a fatal "invalid format" protocol error at
  Xwayland, killing xwayland-satellite; linux-dmabuf covers every
  current client. Also: offer dmabuf **v4** (default feedback) on every
  backend — wf-recorder and friends hard-bind v4 and die on v3
