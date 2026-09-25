# Rust parity inventory

Baseline: the Rust/Smithay + Lua tree at `6de3ba6^` (old paths are relative to
that tree). New paths are relative to this repository. Status is one of Done,
Partial, Missing, Improved (with why), or Dropped (with why).

| # | Feature | Old source | New source | Status |
|---|---|---|---|---|
| 1 | Scale snapping to N/120 | crates/tomoe/src/coords.rs | native/space.c `snapped_scale` | Done |
| 2 | Configure-size quantization (logical round trip) | crates/tomoe/src/coords.rs | native/space.c, src/runtime.lisp `%quantize-layout` | Done |
| 3 | Integer physical world coordinates | crates/tomoe/src/space.rs | native/space.c | Done |
| 4 | Global `scale` setting for outputs without their own | crates/tomoe/src/lua.rs (settings.scale) | src/api.lisp `configure-output` (per output only) | Partial |
| 5 | Fractional-scale and preferred buffer scale | crates/tomoe/src/state.rs `send_scale` | native/space.c `set_surface_scale` | Done |
| 6 | Infinite canvas camera, screen = (world − offset)·zoom | crates/tomoe/src/space.rs | native/space.c `tomoe_set_view`, src/api.lisp `set-view` | Done |
| 7 | Zoom clamp 1/16..16 | crates/tomoe/src/space.rs | native/space.c, src/api.lisp | Done |
| 8 | Zoom scales windows only; layers, UI, cursor stay screen-fixed | crates/tomoe/src/render/mod.rs | native/space.c `make_leaf` | Done |
| 9 | World-space hit testing | crates/tomoe/src/space.rs, input.rs | native/space.c `physical_hit_test` | Done |
| 10 | Output enter/leave by overlap | crates/tomoe/src/space.rs `refresh` | native/space.c `refresh_leaf` | Done |
| 11 | Off-output window culling | crates/tomoe/src/render/mod.rs | native/space.c `render_leaf` | Done |
| 12 | Clear color 0.05 grey | crates/tomoe/src/backend/winit.rs, tty.rs | native/space.c `render_scene_buffer` | Done |
| 13 | `tomoe.pointer()` world/screen pointer position | crates/tomoe/src/lua.rs | none | Missing |
| 14 | Spring/ease animation engine, `window_move` spring (1.0, 800) | crates/tomoe/src/animation.rs | none | Missing |
| 15 | `window_open` fade, 150 ms ease_out_expo, also on show | crates/tomoe/src/animation.rs, state.rs | none | Missing |
| 16 | `animations` setting (false/true/per-property spec, bezier curves) | crates/tomoe/src/lua.rs | none | Missing |
| 17 | Window border ring, width 2, #7aa2f7 / #3b4261, none when fullscreen | crates/tomoe/src/render/border.rs, layout.rs | none | Missing |
| 18 | Rounded window corners (`border.radius`), none when fullscreen | crates/tomoe/src/render/clipped_surface.rs | none | Missing |
| 19 | Drop shadow, range 12, #00000099, power 3 | crates/tomoe/src/render/shadow.rs | none | Missing |
| 20 | Dual-kawase blur behind listed layer namespaces | crates/tomoe/src/render/blur.rs, render/mod.rs | none | Missing |
| 21 | ext-background-effect-v1 blur regions | crates/tomoe/src/protocols/background_effect.rs | none | Missing |
| 22 | Per-window blur behind (zoom 1, not fullscreen) | crates/tomoe/src/render/mod.rs `render_masked` | none | Missing |
| 23 | `Window:set_properties` radius/tearing/blur/border | crates/tomoe/src/lua.rs | none | Missing |
| 24 | Damage tracking (redraw only damaged regions) | crates/tomoe/src/backend/winit.rs, tty.rs | native/space.c (full-frame damage) | Partial |
| 25 | Direct scanout for fullscreen windows | crates/tomoe/src/backend/tty.rs | none | Missing |
| 26 | wp-presentation-time feedback | crates/tomoe/src/state.rs, tty.rs | native/protocols.c, native/space.c `surfaces_textured` | Done |
| 27 | Drag-and-drop with drag icon | crates/tomoe/src/handlers.rs, render/mod.rs | native/protocols.c `seat_start_drag`, native/input.c | Done |
| 28 | `XCURSOR_THEME` / `XCURSOR_SIZE` | crates/tomoe/src/cursor.rs | native/backend.c | Done |
| 29 | Block cursor when no theme loads | crates/tomoe/src/state.rs, render/mod.rs | wlroots built-in cursor via native/backend.c | Improved: wlroots draws its built-in arrow when no theme loads, instead of an 8×16 white block |
| 30 | Client cursor surfaces, hidden cursor | crates/tomoe/src/render/mod.rs | native/input.c `request_cursor` | Done |
| 31 | XKB keymap and repeat settings | crates/tomoe/src/lua.rs, state.rs | src/api.lisp `configure-keyboard`, native/input.c | Done |
| 32 | Key bindings with configurable `mod` | crates/tomoe/src/input.rs, lua.rs | src/api.lisp `bind-key` (no `mod`) | Partial |
| 33 | Hold bindings (press/release latched by keycode) | crates/tomoe/src/input.rs | src/api.lisp `bind-key :release`, native/input.c | Done |
| 34 | VT switching (XF86Switch_VT_1..12) | crates/tomoe/src/input.rs, backend/tty.rs | none | Missing |
| 35 | libinput settings: touchpad/mouse classes and per-device overrides | crates/tomoe/src/backend/tty.rs, lua.rs | none | Missing |
| 36 | `on_pointer_button` with consume and named buttons | crates/tomoe/src/lua.rs, input.rs | native/input.c `:button` (observe only) | Partial |
| 37 | `on_pointer_axis` with consume | crates/tomoe/src/lua.rs | none | Missing |
| 38 | `on_pointer_enter` / `on_pointer_leave` | crates/tomoe/src/lua.rs, input.rs | none | Missing |
| 39 | `focus_follows_mouse` (sloppy) | crates/tomoe/src/input.rs | none | Missing |
| 40 | `grab_pointer` / `ungrab_pointer` for arbitrary motion (pan) | crates/tomoe/src/lua.rs | src/api.lisp `grab` (window move/resize only) | Partial |
| 41 | Pointer constraints (lock/confine, cursor hint) | crates/tomoe/src/handlers.rs, input.rs | native/protocols.c, native/input.c | Done |
| 42 | Relative pointer | crates/tomoe/src/input.rs | native/protocols.c, native/input.c `motion` | Done |
| 43 | `keyboard_activity` IPC event | crates/tomoe/src/ipc.rs | src/runtime.lisp `:activity` | Done |
| 44 | xdg move/resize/minimize requests from clients | crates/tomoe/src/handlers.rs | native/window.c (fullscreen/maximize only) | Partial |
| 45 | xdg popup unconstraining and popup grabs | crates/tomoe/src/handlers.rs | native/window.c (placement only) | Partial |
| 46 | xdg-decoration, `force_server_side_decorations` | crates/tomoe/src/handlers.rs, lua.rs | native/protocols.c `decoration_apply`, src/api.lisp `settings` | Done |
| 47 | KDE server-decoration | crates/tomoe/src/handlers.rs | native/protocols.c | Done |
| 48 | Layer shell | crates/tomoe/src/handlers.rs | native/layer.c | Done |
| 49 | Xwayland | crates/tomoe/src/xwayland.rs (xwayland-satellite) | native/window.c (wlroots XWM, lazy) | Improved: in-process XWM, no satellite process to supervise |
| 50 | xdg-activation, 10 s tokens, urgent without serial | crates/tomoe/src/handlers.rs | native/activation.c | Done |
| 51 | `honor_xdg_activation_with_invalid_serial` | crates/tomoe/src/handlers.rs, lua.rs | native/activation.c, src/api.lisp `settings` | Done |
| 52 | ext-foreign-toplevel-list | crates/tomoe/src/handlers.rs | native/window.c `foreign_toplevels_refresh` | Done |
| 53 | wlr-foreign-toplevel-management requests | crates/tomoe/src/protocols/wlr_foreign_toplevel.rs | native/window.c `foreign_refresh_wlr`, builtins/desktop.lisp `wm` | Done |
| 54 | wlr-screencopy with cursor overlay | crates/tomoe/src/protocols/screencopy.rs, capture.rs | native/backend.c, native/space.c, patches/wlroots-screencopy-buffer.patch | Done |
| 55 | ext-image-copy-capture for outputs and toplevels | crates/tomoe/src/capture.rs | native/protocols.c, native/window.c `toplevel_capture_request` | Done |
| 56 | Screenshot UI, `screenshot`/`screenshot-screen`, `screenshot_freeze` | crates/tomoe/src/ui/screenshot_ui.rs, screenshot.rs | none | Missing |
| 57 | ext-session-lock | crates/tomoe/src/lock.rs | native/lock.c | Done |
| 58 | Gamma control | crates/tomoe/src/protocols/gamma_control.rs | native/protocols.c `gamma_apply`, native/output.c | Done |
| 59 | Tearing control, `tearing` setting | crates/tomoe/src/protocols/tearing_control.rs, backend/tty.rs | native/window.c `windows_want_tearing`, native/output.c, src/api.lisp `settings` | Done |
| 60 | Idle notify and idle inhibit | crates/tomoe/src/state.rs, handlers.rs | native/protocols.c `idle_refresh`, native/input.c | Done |
| 61 | Primary selection | crates/tomoe/src/handlers.rs | native/protocols.c | Done |
| 62 | wlr and ext data-control | crates/tomoe/src/handlers.rs | native/protocols.c | Done |
| 63 | Clipboard selection | crates/tomoe/src/handlers.rs | native/input.c | Done |
| 64 | Viewporter, xdg-output | crates/tomoe/src/state.rs | native/backend.c | Done |
| 65 | linux-dmabuf | crates/tomoe/src/backend/tty.rs | native/backend.c `wlr_renderer_init_wl_display` | Done |
| 66 | linux-drm-syncobj | crates/tomoe/src/backend/tty.rs | none | Missing |
| 67 | libseat session pause/resume | crates/tomoe/src/backend/tty.rs | wlroots session via native/backend.c | Done |
| 68 | DRM hotplug | crates/tomoe/src/backend/tty.rs | native/output.c | Done |
| 69 | Modes preferred / max / WxH, `@Hz` / `@max` | crates/tomoe/src/lua.rs, backend/tty.rs | src/api.lisp `configure-output`, native/output.c | Partial |
| 70 | VRR per output | crates/tomoe/src/backend/tty.rs | native/output.c | Done |
| 71 | Output mirroring | crates/tomoe/src/lua.rs | native/output.c | Done |
| 72 | Output disable and explicit position | crates/tomoe/src/lua.rs | native/output.c | Done |
| 73 | `wait_for_frame_completion` | crates/tomoe/src/backend/tty.rs | native/space.c `render_scene_buffer`, src/api.lisp `settings` | Done |
| 74 | `--drm_device` render GPU override | crates/tomoe/src/main.rs | src/main.lisp `primary-drm-devices` | Done |
| 75 | `winit_size` nested window size (1280×800) | crates/tomoe/src/backend/winit.rs | native/output.c `outputs_request_nested_size`, src/api.lisp `settings :nested-size` | Done |
| 76 | `--backend winit\|tty` names | crates/tomoe/src/main.rs | src/main.lisp `run-cli` | Done |
| 77 | `-h` / `-V` | crates/tomoe/src/main.rs | src/main.lisp `run-cli` | Done |
| 78 | Action strings (`quit`, `quit!`, `close-window`, `reload-config`, `spawn …`) | crates/tomoe/src/input.rs | builtins/desktop.lisp commands | Partial |
| 79 | Bind descriptions for the hotkey overlay | crates/tomoe/src/lua.rs, ui/widgets.rs | none | Missing |
| 80 | `tomoe.spawn` with activation token | crates/tomoe/src/lua.rs | src/api.lisp `spawn`, src/processes.lisp | Done |
| 81 | `tomoe.quit()` opens the exit dialog | crates/tomoe/src/lua.rs, state.rs | src/api.lisp `quit` (immediate) | Partial |
| 82 | `clear_focus`, `windows`, `window`, `focused_window` | crates/tomoe/src/lua.rs | `(focus nil)`, `:windows`, `:focus` context | Done |
| 83 | Window reads and writes (geometry, show/hide, focus, raise, fullscreen, maximize, close) | crates/tomoe/src/lua.rs | src/api.lisp | Done |
| 84 | `outputs`, `usable_area`, `view`, `set_view` | crates/tomoe/src/lua.rs | `:outputs`, `:workareas`, `:view`, `set-view` | Done |
| 85 | Window open/close, focus change, outputs changed hooks | crates/tomoe/src/lua.rs | reducers over `:windows`, `:focus`, `:outputs` | Done |
| 86 | `on_window_request` full set with native defaults | crates/tomoe/src/lua.rs, handlers.rs | native/window.c, native/activation.c (subset) | Partial |
| 87 | `on_reload` save/restore | crates/tomoe/src/lua.rs | src/runtime.lisp `configure` | Improved: mounted state survives reload automatically; no save/restore hooks to write |
| 88 | Window rules and `rules_for` | crates/tomoe/src/lua.rs | src/api.lisp `window-rule`, src/rules.lisp, src/patterns.lisp | Done |
| 89 | `process.once` with the id as default command | crates/tomoe/src/lua.rs | src/api.lisp `run-once` (command required) | Partial |
| 90 | `process.service`, `process.spawn` | crates/tomoe/src/lua.rs, process.rs | src/api.lisp `service`, `spawn` | Done |
| 91 | Shutdown stops supervised processes | crates/tomoe/src/process.rs | src/processes.lisp | Improved: session-owned one-shot children are reaped too |
| 92 | `ipc.serve`, `ipc.broadcast` | crates/tomoe/src/lua.rs | src/api.lisp `serve-state`, `serve-method`, `ipc-reply`, `broadcast`, `announce` | Done |
| 93 | `tomoe.ui` confirm / menu / toast / sheet and `close` | crates/tomoe/src/lua.rs, ui/widgets.rs | none | Missing |
| 94 | Exit confirm dialog | crates/tomoe/src/state.rs | none | Missing |
| 95 | Hotkey overlay (`Mod+Shift+/`) | crates/tomoe/src/state.rs, ui/widgets.rs | none | Missing |
| 96 | Config-error banner | crates/tomoe/src/state.rs | src/runtime.lisp `record-error` (stderr only) | Missing |
| 97 | `watchdog_ms` | crates/tomoe/src/lua.rs | src/runtime.lisp (fixed 25 ms reducer budget) | Partial |
| 98 | Default tiling WM (wm.lua) | resources/wm.lua, resources/init.lua | builtins/desktop.lisp `wm`, `commands` | Partial |
| 99 | Zoomer canvas WM | resources/zoomer.lua | none | Missing |
| 100 | Special workspaces | resources/special.lua | none | Missing |
| 101 | Screencast source picker | resources/screencast.lua | none | Missing |
| 102 | Default config: notifications, screencast, shadow and border defaults | resources/init.lua | builtins/desktop.lisp | Partial |
| 103 | JSON IPC socket path and discovery | crates/tomoe-ipc/src/lib.rs | src/main.lisp `json-socket-path` | Done |
| 104 | JSON IPC framing, wire 2 | crates/tomoe-ipc/src/lib.rs | src/ipc-transport.lisp | Done |
| 105 | `version`, `windows`, `outputs`, `view`, `subscribe`, `quit` methods | crates/tomoe/src/ipc.rs | src/ipc.lisp | Done |
| 106 | `window_open`/`window_close`/`focus_change`/`outputs_changed` events | crates/tomoe/src/ipc.rs | src/ipc.lisp | Done |
| 107 | `screencast_select` answered by policy, deferrable | crates/tomoe/src/ipc.rs, lua.rs | src/ipc.lisp (always `fallback`) | Partial |
| 108 | `tomoe msg` CLI | crates/tomoe/src/main.rs | src/main.lisp, src/ipc-transport.lisp | Done |
| 109 | Session environment import (systemd, D-Bus), `XDG_CURRENT_DESKTOP=tomoe` | crates/tomoe/src/main.rs | none | Missing |
| 110 | tomoe-session.target start/stop and portal restart | crates/tomoe/src/main.rs | none | Missing |
| 111 | In-process shell surfaces | crates/tomoe/src/shell.rs, crates/moonshell-* | src/ui.lisp, native/ui.c | Done |
| 112 | Element vocabulary (row/column/text/icon/button/separator/progress/image/stack) | crates/moonshell-runtime/src/element.rs | src/ui.lisp | Done |
| 113 | `slider`, `scroll`, `input` elements | crates/moonshell-runtime/src/element.rs | none | Dropped: the old parser rejected all three |
| 114 | `ui.when`, `ui.map`, `ui.fragment`, utils | resources/moonshell/stdlib.lua, utils.lua | none | Dropped: plain Lisp covers them |
| 115 | Theme palette, presets, `theme:set`, spacing tokens | resources/moonshell/theme.lua, shell_ext.lua | src/ui.lisp (fixed defaults) | Partial |
| 116 | `ui.bar_layout` | resources/moonshell/stdlib.lua | none | Missing |
| 117 | Workspaces widget | resources/moonshell/widgets/workspaces.lua | none | Missing |
| 118 | Clock widget | resources/moonshell/widgets/clock.lua | none | Missing |
| 119 | Battery widget | resources/moonshell/widgets/battery.lua | examples/battery.lisp | Done |
| 120 | Network widget | resources/moonshell/widgets/network.lua | examples/network.lisp | Done |
| 121 | MPRIS label widget | resources/moonshell/widgets/mpris.lua | examples/media.lisp | Done |
| 122 | Media panel | resources/moonshell/widgets/media_panel.lua | none | Missing |
| 123 | Volume panel | resources/moonshell/widgets/volume_panel.lua | none | Missing |
| 124 | Sysinfo service | resources/moonshell/services.lua | src/runtime.lisp placeholder | Dropped: the old service was a placeholder fixed at 0 |
| 125 | Battery, network, MPRIS services | crates/moonshell-services | support/*.c, src/battery.lisp, network.lisp, mpris.lisp | Done |
| 126 | Notification daemon | crates/moonshell-services/src/notifications.rs | support/notifications.c, src/notifications.lisp | Done |
| 127 | Notification popups | resources/moonshell/notifications.lua | builtins/desktop.lisp `notification-popups` | Done |
| 128 | Tray watcher and host | crates/moonshell-services/src/tray.rs | support/tray.c, src/tray.lisp, examples/tray.lisp | Done |
| 129 | Standalone moonshell layer-shell binary | crates/moonshell | none | Dropped: only ever run by a boot check; the in-process shell replaced it |
| 130 | ScreenCast portal (monitor and window, hidden/embedded cursor, PipeWire) | crates/xdg-desktop-portal-tomoe | none | Missing |
| 131 | Portal env fallbacks and `TOMOE_PORTAL_CHOOSER` | crates/xdg-desktop-portal-tomoe/src/screencast.rs | none | Missing |
| 132 | tomoe.portal, tomoe-portals.conf, tomoe-session.target, D-Bus service install | resources/, flake.nix | none | Missing |
| 133 | run-tty.sh | run-tty.sh | dev.sh | Dropped: `nix run . -- --backend drm` replaces it |
| 134 | Pure-Lisp backend | none | backend/ (deleted) | Improved: nothing shipped loaded it; deleting it removed a second backend contract |
