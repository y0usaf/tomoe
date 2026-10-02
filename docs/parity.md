# Rust parity inventory

Baseline: the Rust/Smithay + Lua tree at `6de3ba6^` (old paths are relative to
that tree). New paths are relative to this repository. Status is one of Done,
Partial, Missing, Improved (with why), or Dropped (with why).

| # | Feature | Old source | New source | Status |
|---|---|---|---|---|
| 1 | Scale snapping to N/120 | crates/tomoe/src/coords.rs | native/space.c `snapped_scale` | Done |
| 2 | Configure-size quantization (logical round trip) | crates/tomoe/src/coords.rs | native/space.c, src/runtime.lisp `%quantize-layout` | Done |
| 3 | Integer physical world coordinates | crates/tomoe/src/space.rs | native/space.c | Done |
| 4 | Global `scale` setting for outputs without their own | crates/tomoe/src/lua.rs (settings.scale) | src/runtime.lisp `default-output-config`, src/api.lisp `settings :scale` | Done |
| 5 | Fractional-scale and preferred buffer scale | crates/tomoe/src/state.rs `send_scale` | native/space.c `set_surface_scale` | Done |
| 6 | Infinite canvas camera, screen = (world − offset)·zoom | crates/tomoe/src/space.rs | native/space.c `tomoe_set_view`, src/api.lisp `set-view` | Done |
| 7 | Zoom clamp 1/16..16 | crates/tomoe/src/space.rs | native/space.c, src/api.lisp | Done |
| 8 | Zoom scales windows only; layers, UI, cursor stay screen-fixed | crates/tomoe/src/render/mod.rs | native/space.c `make_leaf` | Done |
| 9 | World-space hit testing | crates/tomoe/src/space.rs, input.rs | native/space.c `physical_hit_test` | Done |
| 10 | Output enter/leave by overlap | crates/tomoe/src/space.rs `refresh` | native/space.c `refresh_leaf` | Done |
| 11 | Off-output window culling | crates/tomoe/src/render/mod.rs | native/space.c `render_leaf` | Done |
| 12 | Clear color 0.05 grey | crates/tomoe/src/backend/winit.rs, tty.rs | native/space.c `render_scene_buffer` | Done |
| 13 | `tomoe.pointer()` world/screen pointer position | crates/tomoe/src/lua.rs | `:x :y :sx :sy` on every binding event, native/input.c `pointer_fields` | Done |
| 14 | Spring/ease animation engine, `window_move` spring (1.0, 800) | crates/tomoe/src/animation.rs | native/animation.c, native/window.c `window_animate_move` | Done |
| 15 | `window_open` fade, 150 ms ease_out_expo, also on show | crates/tomoe/src/animation.rs, state.rs | native/window.c `window_animate_open`, native/space.c `render_leaf` | Done |
| 16 | `animations` setting (false/true/per-property spec, bezier curves) | crates/tomoe/src/lua.rs | src/api.lisp `settings :animations` | Done |
| 17 | Window border ring, width 2, #7aa2f7 / #3b4261, none when fullscreen | crates/tomoe/src/render/border.rs, layout.rs | native/effects.c `effect_border`, native/space.c `decorate`, src/api.lisp `settings :border` | Done |
| 18 | Rounded window corners (`border.radius`), none when fullscreen | crates/tomoe/src/render/clipped_surface.rs | native/effects.c `effect_texture`, native/space.c `render_leaf`, `settings :border :radius` | Done |
| 19 | Drop shadow, range 12, #00000099, power 3 | crates/tomoe/src/render/shadow.rs | native/effects.c `effect_shadow`, native/space.c `decorate`, src/api.lisp `settings :shadow` | Done |
| 20 | Dual-kawase blur behind listed layer namespaces | crates/tomoe/src/render/blur.rs, render/mod.rs | native/space.c `decorate_layer`, `settings :blur :layer-namespaces` | Done |
| 21 | ext-background-effect-v1 blur regions | crates/tomoe/src/protocols/background_effect.rs | native/background.c, native/space.c `decorate_layer` | Done |
| 22 | Per-window blur behind (zoom 1, not fullscreen) | crates/tomoe/src/render/mod.rs `render_masked` | native/effects.c `effect_blur`, native/space.c `decorate`, `window-properties :blur`, `settings :blur` | Done |
| 23 | `Window:set_properties` radius/tearing/blur/border | crates/tomoe/src/lua.rs | src/api.lisp `window-properties`, native/presentation.c `tomoe_present_window_style` | Done |
| 24 | Damage tracking (redraw only damaged regions) | crates/tomoe/src/backend/winit.rs, tty.rs | native/space.c `scene_damage`, native/render.c `oplist_damage` | Done: draw-list diff with per-commit client damage, buffer age, blur widening |
| 25 | Direct scanout for fullscreen windows | crates/tomoe/src/backend/tty.rs | native/space.c `scanout_surface`, native/output.c `output_frame` | Done |
| 26 | wp-presentation-time feedback | crates/tomoe/src/state.rs, tty.rs | native/surface.c `surface_presented`, native/space.c `surfaces_textured` | Done |
| 27 | Drag-and-drop with drag icon | crates/tomoe/src/handlers.rs, render/mod.rs | native/selection.c `start_drag`, native/protocols.c `drag_icons_refresh` | Done |
| 28 | `XCURSOR_THEME` / `XCURSOR_SIZE` | crates/tomoe/src/cursor.rs | native/base.c `xcursor_load` | Done |
| 29 | Block cursor when no theme loads | crates/tomoe/src/state.rs, render/mod.rs | native/input.c `cursor_default` | Done: an 8×16 white block, scaled, when no xcursor theme loads |
| 30 | Client cursor surfaces, hidden cursor | crates/tomoe/src/render/mod.rs | native/input.c `request_cursor` | Done |
| 31 | XKB keymap and repeat settings | crates/tomoe/src/lua.rs, state.rs | src/api.lisp `configure-keyboard`, native/input.c | Done |
| 32 | Key bindings with configurable `mod` | crates/tomoe/src/input.rs, lua.rs | src/api.lisp `bind-key :mod`, `settings :mod`, src/runtime.lisp `materialize` | Done |
| 33 | Hold bindings (press/release latched by keycode) | crates/tomoe/src/input.rs | src/api.lisp `bind-key :release`, native/input.c | Done |
| 34 | VT switching (XF86Switch_VT_1..12) | crates/tomoe/src/input.rs, backend/tty.rs | native/input.c `keyboard_key`, native/backend.c session | Done |
| 35 | libinput settings: touchpad/mouse classes and per-device overrides | crates/tomoe/src/backend/tty.rs, lua.rs | native/libinput.c, src/api.lisp `settings :touchpad :mouse :devices` | Done |
| 36 | `on_pointer_button` with consume and named buttons | crates/tomoe/src/lua.rs, input.rs | src/api.lisp `bind-button`, native/input.c `pointer_binding_button`; unbound presses still arrive as `:button` events | Improved: consumption is declared as a binding matched in C, so a consumed press never waits on Lisp or leaks to the client |
| 37 | `on_pointer_axis` with consume | crates/tomoe/src/lua.rs | src/api.lisp `bind-scroll`, native/input.c `pointer_binding_axis` | Improved: consumed scrolling is a declared binding matched in C, like buttons |
| 38 | `on_pointer_enter` / `on_pointer_leave` | crates/tomoe/src/lua.rs, input.rs | native/input.c `hover_event` → `:pointer` events | Done |
| 39 | `focus_follows_mouse` (sloppy) | crates/tomoe/src/input.rs | builtins/desktop.lisp `wm`, src/api.lisp `settings :focus-follows-mouse` | Done |
| 40 | `grab_pointer` / `ungrab_pointer` for arbitrary motion (pan) | crates/tomoe/src/lua.rs | src/api.lisp `grab nil :pointer`, native/input.c `grab_motion` | Done |
| 41 | Pointer constraints (lock/confine, cursor hint) | crates/tomoe/src/handlers.rs, input.rs | native/pointer.c, native/input.c | Done |
| 42 | Relative pointer | crates/tomoe/src/input.rs | native/pointer.c, native/input.c `motion` | Done |
| 43 | `keyboard_activity` IPC event | crates/tomoe/src/ipc.rs | src/runtime.lisp `:activity` | Done |
| 44 | xdg move/resize/minimize requests from clients | crates/tomoe/src/handlers.rs | native/window.c `window_move`, `window_resize`, `window_minimize` | Done |
| 45 | xdg popup unconstraining and popup grabs | crates/tomoe/src/handlers.rs | native/window.c `popup_unconstrain`; native/xdg_shell.c grabs on native/seat.c | Done |
| 46 | xdg-decoration, `force_server_side_decorations` | crates/tomoe/src/handlers.rs, lua.rs | native/decoration.c, src/api.lisp `settings` | Done |
| 47 | KDE server-decoration | crates/tomoe/src/handlers.rs | native/decoration.c | Done |
| 48 | Layer shell | crates/tomoe/src/handlers.rs | native/layer_shell.c, native/layer.c | Done |
| 49 | Xwayland | crates/tomoe/src/xwayland.rs (xwayland-satellite) | builtins/desktop.lisp `xwayland` service, src/main.lisp `free-x-display` | Done: xwayland-satellite started on the first X11 connection by support/xwayland.c, supervised through the public `service` API |
| 50 | xdg-activation, 10 s tokens, urgent without serial | crates/tomoe/src/handlers.rs | native/activation.c | Done |
| 51 | `honor_xdg_activation_with_invalid_serial` | crates/tomoe/src/handlers.rs, lua.rs | native/activation.c, src/api.lisp `settings` | Done |
| 52 | ext-foreign-toplevel-list | crates/tomoe/src/handlers.rs | native/foreign.c, native/window.c `foreign_toplevels_refresh` | Done |
| 53 | wlr-foreign-toplevel-management requests | crates/tomoe/src/protocols/wlr_foreign_toplevel.rs | native/foreign.c, native/window.c `window_foreign_request`, builtins/desktop.lisp `wm` | Done |
| 54 | wlr-screencopy with cursor overlay | crates/tomoe/src/protocols/screencopy.rs, capture.rs | native/capture.c on libwayland-server | Done |
| 55 | ext-image-copy-capture for outputs and toplevels | crates/tomoe/src/capture.rs | native/capture.c on libwayland-server, native/space.c `render_window_buffer` | Done |
| 56 | Screenshot UI, `screenshot`/`screenshot-screen`, `screenshot_freeze` | crates/tomoe/src/ui/screenshot_ui.rs, screenshot.rs | native/screenshot.c, `(screenshot)`/`(screenshot :screen)`, `:screenshot-freeze`, builtins/desktop.lisp "screenshot-clipboard" | Done |
| 57 | ext-session-lock | crates/tomoe/src/lock.rs | native/lock.c | Done |
| 58 | Gamma control | crates/tomoe/src/protocols/gamma_control.rs | native/gamma.c on libwayland-server, native/output.c | Done |
| 59 | Tearing control, `tearing` setting | crates/tomoe/src/protocols/tearing_control.rs, backend/tty.rs | native/tearing.c, native/window.c `windows_want_tearing`, native/output.c, src/api.lisp `settings` | Done |
| 60 | Idle notify and idle inhibit | crates/tomoe/src/state.rs, handlers.rs | native/idle.c `idle_refresh`, native/input.c | Done |
| 61 | Primary selection | crates/tomoe/src/handlers.rs | native/selection.c | Done |
| 62 | wlr and ext data-control | crates/tomoe/src/handlers.rs | native/selection.c | Done |
| 63 | Clipboard selection | crates/tomoe/src/handlers.rs | native/selection.c | Done |
| 64 | Viewporter, xdg-output | crates/tomoe/src/state.rs | native/backend.c | Done |
| 65 | linux-dmabuf | crates/tomoe/src/backend/tty.rs | native/buffer.c | Done |
| 66 | linux-drm-syncobj | crates/tomoe/src/backend/tty.rs | native/surface.c, native/space.c `render_leaf` | Done |
| 67 | libseat session pause/resume | crates/tomoe/src/backend/tty.rs | native/session.c, native/kms.c `kms_pause`/`kms_resume` | Done |
| 68 | DRM hotplug | crates/tomoe/src/backend/tty.rs | native/output.c | Done |
| 69 | Modes preferred / max / WxH, `@Hz` / `@max` | crates/tomoe/src/lua.rs, backend/tty.rs | native/output.c `pick_output_mode`, src/api.lisp `configure-output :refresh` | Done |
| 70 | VRR per output | crates/tomoe/src/backend/tty.rs | native/output.c | Done |
| 71 | Output mirroring | crates/tomoe/src/lua.rs | native/output.c | Done |
| 72 | Output disable and explicit position | crates/tomoe/src/lua.rs | native/output.c | Done |
| 73 | `wait_for_frame_completion` | crates/tomoe/src/backend/tty.rs | native/space.c `render_scene_buffer`, src/api.lisp `settings` | Done |
| 74 | `--drm_device` render GPU override | crates/tomoe/src/main.rs | src/main.lisp `primary-drm-devices` | Done |
| 75 | `winit_size` nested window size (1280×800) | crates/tomoe/src/backend/winit.rs | native/output.c `outputs_request_nested_size`, src/api.lisp `settings :nested-size` | Done |
| 76 | `--backend winit\|tty` names | crates/tomoe/src/main.rs | src/main.lisp `run-cli` | Done |
| 77 | `-h` / `-V` | crates/tomoe/src/main.rs | src/main.lisp `run-cli` | Done |
| 78 | Action strings (`quit`, `quit!`, `close-window`, `reload-config`, `spawn …`) | crates/tomoe/src/input.rs | commands returned by reducers: `quit`, `close-window`, `reload`, `spawn`; builtins/desktop.lisp `commands` | Done |
| 79 | Bind descriptions for the hotkey overlay | crates/tomoe/src/lua.rs, ui/widgets.rs | src/api.lisp `bind-key :description`, `:bindings` context `:order` `:declared` | Done |
| 80 | `tomoe.spawn` with activation token | crates/tomoe/src/lua.rs | src/api.lisp `spawn`, src/processes.lisp | Done |
| 81 | `tomoe.quit()` opens the exit dialog | crates/tomoe/src/lua.rs, state.rs | builtins/desktop.lisp `commands` (`:quit` opens the dialog; `(quit)` is the immediate form) | Done |
| 82 | `clear_focus`, `windows`, `window`, `focused_window` | crates/tomoe/src/lua.rs | `(focus nil)`, `:windows`, `:focus` context | Done |
| 83 | Window reads and writes (geometry, show/hide, focus, raise, fullscreen, maximize, close) | crates/tomoe/src/lua.rs | src/api.lisp | Done |
| 84 | `outputs`, `usable_area`, `view`, `set_view` | crates/tomoe/src/lua.rs | `:outputs`, `:workareas`, `:view`, `set-view` | Done |
| 85 | Window open/close, focus change, outputs changed hooks | crates/tomoe/src/lua.rs | reducers over `:windows`, `:focus`, `:outputs` | Done |
| 86 | `on_window_request` full set with native defaults | crates/tomoe/src/lua.rs, handlers.rs | native/window.c, native/activation.c `:request` events (maximize, fullscreen, minimize, move, resize, activate, urgent, close); defaults in builtins/desktop.lisp "wm" | Done |
| 87 | `on_reload` save/restore | crates/tomoe/src/lua.rs | src/runtime.lisp `configure` | Improved: mounted state survives reload automatically; no save/restore hooks to write |
| 88 | Window rules and `rules_for` | crates/tomoe/src/lua.rs | src/api.lisp `window-rule`, src/rules.lisp (exact app-id, substring title, `:match` predicate) | Improved: plain Lisp matching replaces 235 lines of Lua pattern emulation; richer matching lives in the :match predicate |
| 89 | `process.once` with the id as default command | crates/tomoe/src/lua.rs | src/api.lisp `run-once`, `service` (COMMAND optional, defaults to the name) | Done |
| 90 | `process.service`, `process.spawn` | crates/tomoe/src/lua.rs, process.rs | src/api.lisp `service`, `spawn` | Done |
| 91 | Shutdown stops supervised processes | crates/tomoe/src/process.rs | src/processes.lisp | Improved: session-owned one-shot children are reaped too |
| 92 | `ipc.serve`, `ipc.broadcast` | crates/tomoe/src/lua.rs | src/api.lisp `serve-state`, `serve-method`, `ipc-reply`, `broadcast`, `announce` | Done |
| 93 | `tomoe.ui` confirm / menu / toast / sheet and `close` | crates/tomoe/src/lua.rs, ui/widgets.rs | src/dialogs.lisp `confirm-dialog` `menu-dialog` `sheet-dialog` `toast`, src/api.lisp `keyboard-grab`, native/input.c `ui_hover` | Done |
| 94 | Exit confirm dialog | crates/tomoe/src/state.rs | builtins/desktop.lisp `commands` via `confirm-dialog` | Done |
| 95 | Hotkey overlay (`Mod+Shift+/`) | crates/tomoe/src/state.rs, ui/widgets.rs | builtins/desktop.lisp `commands` via `sheet-dialog`, `hotkey-label` | Done |
| 96 | Config-error banner | crates/tomoe/src/state.rs | src/runtime.lisp `report-config-error`, `:config-error` context, builtins/desktop.lisp `config-error` | Done |
| 97 | `watchdog_ms` | crates/tomoe/src/lua.rs | src/api.lisp `:watchdog-ms` setting (default 1000, 0 disables), src/runtime.lisp `invoke-extension` | Done |
| 98 | Default tiling WM (wm.lua) | resources/wm.lua, resources/init.lua | builtins/desktop.lisp "wm" | Done |
| 99 | Zoomer canvas WM | resources/zoomer.lua | examples/zoomer.lisp (mount over the default policy) | Done |
| 100 | Special workspaces | resources/special.lua | examples/special.lisp, `:wm-exclude` honored by builtins/desktop.lisp "wm" | Done |
| 101 | Screencast source picker | resources/screencast.lua | builtins/desktop.lisp "screencast" (rules `:screencast` nil/output, menu-dialog picker) | Done |
| 102 | Default config: notifications, screencast, shadow and border defaults | resources/init.lua | builtins/desktop.lisp ("commands", "wm", "screencast", "notification-popups"; `+settings+` defaults; hotkey sheet in init.lua order) | Done |
| 103 | JSON IPC socket path and discovery | crates/tomoe-ipc/src/lib.rs | src/main.lisp `json-socket-path` | Done |
| 104 | JSON IPC framing, wire 2 | crates/tomoe-ipc/src/lib.rs | src/ipc-transport.lisp | Done |
| 105 | `version`, `windows`, `outputs`, `view`, `subscribe`, `quit` methods | crates/tomoe/src/ipc.rs | src/ipc.lisp | Done |
| 106 | `window_open`/`window_close`/`focus_change`/`outputs_changed` events | crates/tomoe/src/ipc.rs | src/ipc.lisp | Done |
| 107 | `screencast_select` answered by policy, deferrable | crates/tomoe/src/ipc.rs, lua.rs | src/ipc.lisp `screencast_select` → `:screencast` event, `screencast-answer` command (deferrable; fallback when nothing reads `:screencast`) | Done |
| 108 | `tomoe msg` CLI | crates/tomoe/src/main.rs | src/main.lisp, src/ipc-transport.lisp | Done |
| 109 | Session environment import (systemd, D-Bus), `XDG_CURRENT_DESKTOP=tomoe` | crates/tomoe/src/main.rs | src/main.lisp `start-session` (drm only), `XDG_CURRENT_DESKTOP=tomoe` always | Done |
| 110 | tomoe-session.target start/stop and portal restart | crates/tomoe/src/main.rs | src/main.lisp `start-session`/`stop-session`, share/tomoe-session.target | Done |
| 111 | In-process shell surfaces | crates/tomoe/src/shell.rs, crates/moonshell-* | src/ui.lisp, native/ui.c | Done |
| 112 | Element vocabulary (row/column/text/icon/button/separator/progress/image/stack) | crates/moonshell-runtime/src/element.rs | src/ui.lisp | Done |
| 113 | `slider`, `scroll`, `input` elements | crates/moonshell-runtime/src/element.rs | none | Dropped: the old parser rejected all three |
| 114 | `ui.when`, `ui.map`, `ui.fragment`, utils | resources/moonshell/stdlib.lua, utils.lua | none | Dropped: plain Lisp covers them |
| 115 | Theme palette, presets, `theme:set`, spacing tokens | resources/moonshell/theme.lua, shell_ext.lua | src/dialogs.lisp `theme`, `+theme+`, `+theme-presets+` | Done |
| 116 | `ui.bar_layout` | resources/moonshell/stdlib.lua | src/dialogs.lisp `bar-layout` | Done |
| 117 | Workspaces widget | resources/moonshell/widgets/workspaces.lua | src/dialogs.lisp `workspaces-widget` | Done |
| 118 | Clock widget | resources/moonshell/widgets/clock.lua | src/dialogs.lisp `clock-text`, examples/bar.lisp | Done |
| 119 | Battery widget | resources/moonshell/widgets/battery.lua | examples/battery.lisp | Done |
| 120 | Network widget | resources/moonshell/widgets/network.lua | examples/network.lisp | Done |
| 121 | MPRIS label widget | resources/moonshell/widgets/mpris.lua | examples/media.lisp | Done |
| 122 | Media panel | resources/moonshell/widgets/media_panel.lua | examples/bar.lisp media panel | Improved: controls call the player over D-Bus; the old service actions were no-ops (hover tint dropped: UI hover reports enter only) |
| 123 | Volume panel | resources/moonshell/widgets/volume_panel.lua | examples/bar.lisp volume panel | Improved: reads and sets the real volume through wpctl; the old audio service was fixed at 100% (hover tint dropped: UI hover reports enter only) |
| 124 | Sysinfo service | resources/moonshell/services.lua | src/runtime.lisp placeholder | Dropped: the old service was a placeholder fixed at 0 |
| 125 | Battery, network, MPRIS services | crates/moonshell-services | support/*.c, src/battery.lisp, network.lisp, mpris.lisp | Done |
| 126 | Notification daemon | crates/moonshell-services/src/notifications.rs | support/notifications.c, src/notifications.lisp | Done |
| 127 | Notification popups | resources/moonshell/notifications.lua | builtins/desktop.lisp `notification-popups` | Done |
| 128 | Tray watcher and host | crates/moonshell-services/src/tray.rs | support/tray.c, src/tray.lisp, examples/tray.lisp | Done |
| 129 | Standalone moonshell layer-shell binary | crates/moonshell | none | Dropped: only ever run by a boot check; the in-process shell replaced it |
| 130 | ScreenCast portal (monitor and window, hidden/embedded cursor, PipeWire) | crates/xdg-desktop-portal-tomoe | portal/ (vendored unchanged), installed at libexec/xdg-desktop-portal-tomoe | Done |
| 131 | Portal env fallbacks and `TOMOE_PORTAL_CHOOSER` | crates/xdg-desktop-portal-tomoe/src/screencast.rs | portal/crates/xdg-desktop-portal-tomoe/src/screencast.rs | Done |
| 132 | tomoe.portal, tomoe-portals.conf, tomoe-session.target, D-Bus service install | resources/, flake.nix | share/, flake.nix (portal, portals.conf, session target, D-Bus service) | Done |
| 133 | run-tty.sh | run-tty.sh | dev.sh | Dropped: `nix run . -- --backend drm` replaces it |
| 134 | Pure-Lisp backend | none | backend/ (deleted) | Improved: nothing shipped loaded it; deleting it removed a second backend contract |
