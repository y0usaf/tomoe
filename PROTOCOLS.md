# Protocols: the wlroots map

Phase 2 of replacing wlroots. Every protocol Tomoe serves, grouped by what it
depends on, in the order each group moves onto libwayland-server. A group
moves as one step when its members share wlroots state that cannot be split.

## Done

| Protocol | Now | Replaced |
|---|---|---|
| X11 | xwayland-satellite as the `xwayland` builtin `service` | `wlr_xwayland`, the XWM patch, xcb |
| wlr-screencopy-v1 | `native/capture.c` from Tomoe's presented frames | `wlr_screencopy_v1`, the screencopy patch |
| ext-image-copy-capture, output and toplevel sources | `native/capture.c`, `render_window_buffer` | wlroots' managers, its output swapchain use and scene-node source |
| wlr-gamma-control-v1 | `native/gamma.c`, applied as the frame's color transform | `wlr_gamma_control_v1` |
| tearing-control-v1 | `native/tearing.c`, double-buffered through `wlr_surface_synced` | `wlr_tearing_control_v1` |
| wlr-foreign-toplevel-management, ext-foreign-toplevel-list | `native/foreign.c`, fed each step from `foreign_toplevels_refresh` | both wlroots managers |
| xdg-decoration, KDE server-decoration | `native/decoration.c`, mode sent with each xdg configure | both wlroots managers |
| xdg-activation | `native/activation.c`, whose token records are the tokens | `wlr_xdg_activation_v1` and the mirror records Tomoe kept beside it |
| ext-session-lock | `native/lock.c`, a `wlr_surface` role until the surface core moves | `wlr_session_lock_v1` |
| wlr-layer-shell v4 | `native/layer_shell.c`, planned by `native/layer.c` | `wlr_layer_shell_v1`, `wlr_scene_layer_surface_v1` |
| xdg-shell v3 | `native/xdg_shell.c`; positioner math from wlroots' pure `wlr_xdg_positioner_rules` functions until phase 3 | `wlr_xdg_shell`, `wlr_scene_xdg_surface` |

## Seat

wl_seat, data-device and drag and drop, primary selection, wlr and ext data
control, relative pointer, pointer constraints, virtual keyboard and pointer,
idle notify and inhibit. wlroots ties all of them to `wlr_seat`, so they move
together. This removes the two keyboard patches: Tomoe's logical keyboard
already tracks every held key and builds its own enter arrays. xdg popup grabs
move from `wlr_seat`'s grab API onto Tomoe's seat in the same step.

## Surface core

wl_compositor, subcompositor, viewporter, fractional-scale, presentation-time,
linux-dmabuf, wl_shm and linux-drm-syncobj, and `wlr_scene` with them. Last in
phase 2 because every group above sits on `wlr_surface`. After this the
renderer's wlroots interface serves only cursors and the DRM backend.

## Phase 3

wl_output and xdg-output stay with `wlr_output` until DRM/KMS moves.
