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

## Surface roles

layer-shell, session-lock, xdg-activation, wlr and ext foreign-toplevel,
xdg-decoration and the KDE server-decoration. Each is a role or side object on
`wlr_surface`, served through wlroots' public surface-role API until the
surface core moves. Tomoe already plans layers, lock and activation itself.

## xdg-shell

Toplevels and popups. The largest fight: Tomoe parks windows at far
coordinates and walks `wlr_scene_xdg_surface` trees only for their surface
lists. Moves after the roles above so the role pattern is settled.

## Seat

wl_seat, data-device and drag and drop, primary selection, wlr and ext data
control, relative pointer, pointer constraints, virtual keyboard and pointer,
idle notify and inhibit. wlroots ties all of them to `wlr_seat`, so they move
together. This removes the two keyboard patches: Tomoe's logical keyboard
already tracks every held key and builds its own enter arrays.

## Surface core

wl_compositor, subcompositor, viewporter, fractional-scale, presentation-time,
linux-dmabuf, wl_shm and linux-drm-syncobj, and `wlr_scene` with them. Last in
phase 2 because every group above sits on `wlr_surface`. After this the
renderer's wlroots interface serves only cursors and the DRM backend.

## Phase 3

wl_output and xdg-output stay with `wlr_output` until DRM/KMS moves.
