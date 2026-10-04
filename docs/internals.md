# Internals

How Tomoe is put together, where each part lives in the tree, and what is
not implemented yet.

## Architecture

Tomoe is a Common Lisp Wayland compositor. SBCL runs the compositor loop, extension
runtime, window-management policy, and control server. C code linked into
SBCL's runtime serves the Wayland protocols on libwayland-server and drives
rendering (EGL/GLES2 on GBM), outputs (DRM/KMS on libseat, nested Wayland,
headless, and virtual outputs beside any of them), and input (libinput). The package's `bin/tomoe` is that runtime
with the saved Lisp image appended: one executable, no wrapper. The shell,
shipped policy, fallback font configuration and helper programs it uses are
store paths fixed when the image is built.

Every desktop behaviour is a mountable extension. The shipped window manager,
drag, and command units use the same API a user file does, and the
host has no special case for their names. Layer-shell panels, fullscreen and
maximize state, pointer-driven move and resize, and output configuration are all
visible to policy as context and owned effects.

The native side sits behind the ABI header `native/backend.h`, split into C
modules by concern.

The earlier Rust/Smithay and Lua implementation is not in this tree; it stays
reachable in the repository history (commit `6de3ba6` and earlier).
[parity.md](parity.md) maps each of its features to this one.

## Source and limits

- `src/`: Common Lisp API, state/effect runtime, native bindings, control, CLI.
- `native/backend.h`: the native ABI. No pointers cross into extension snapshots.
- `native/presentation.c`: candidate scene/input state and publication for policy
  transactions, with candidate rendering when outputs change.
- `native/internal.h`: the shared server struct and the seams between modules.
- `native/event.c`: the serialized event queue Lisp drains.
- `native/output.c`: output lifecycle and staged mode/scale/position commits.
- `native/space.c`: physical transforms, rendering, hit testing, output membership,
  and frame callbacks. Scene trees retain client lifetime and stacking; frames
  redraw in full.
- `native/window.c`: xdg toplevels and popups.
- `native/layer.c`: layer-shell surfaces and arrangement.
- `native/ui.c`, `src/ui.lisp`: retained shell textures, text/vector rasterization,
  declarative layout, and clipped hit targets.
- `native/ui-assets.c`: source-owned PNG/JPEG/SVG decoding and retained asset lifetimes.
- `native/sound.c`: WAV decoding and PipeWire playback for owned sounds.
- `native/input.c`: pointer routing, keyboards, key bindings, pointer grabs.
- `native/buffer.c`: wl_shm and linux-dmabuf client buffers.
- `native/base.c`: buffers, boxes, regions, transforms, format sets, syncobj
  timelines, xdg positioner math, xcursor themes, logging.
- `native/surface.c`: wl_surface, subsurfaces, regions, viewports, fractional
  scale, presentation feedback, and explicit sync.
- `native/node.c`: the stacking tree surfaces and shell chrome render from.
- `native/screen.c`: outputs, their state and commits, frame scheduling,
  wl_output, xdg-output, and the cursor image.
- `native/kms.c`, `native/session.c`: DRM/KMS on libseat, with udev hotplug.
- `native/nested.c`: the nested Wayland backend.
- `native/headless.c`: offscreen outputs, the headless backend's and declared virtual
  ones, each paced by its own frame clock.
- `native/seat.c`: wl_seat, pointer and keyboard focus, grabs, cursor role.
- `native/selection.c`: clipboard, primary selection, data control, drag and drop.
- `native/virtual.c`: virtual keyboard and pointer devices, output mapping lifetime.
- `native/backend.c`: server lifetime — create, step, destroy.
- `support/executions.c`: Linux pidfd process-group helpers for policy-owned
  commands (requires Linux 6.9+).
- `support/watches.c`, `src/watches.lisp`: Linux inotify file observation,
  owned watch preparation, bounded content delivery, and cleanup.
- `builtins/desktop.lisp`: replaceable default policy.
- `examples/`: alternative policies, each mountable on its own.
- `flake.nix`, `build.lisp`, `dev.sh`: native compilation linked into SBCL's
  runtime (`sbcl.o`), and the saved executable or a development session.
- `release.sh`: `nix run .#release -- VERSION [--publish]`, which versions,
  checks, builds, tags and publishes a release from a clean main.
- `CHANGELOG.md`: release notes, one section per version.

Implemented protocols cover xdg-shell windows and popups (constrained to the
output), xdg-decoration and KDE server decoration, shared-memory and DMA-BUF
buffers, subsurfaces, clipboard, primary selection, wlr and ext data control,
drag and drop, viewporter, fractional-scale-v1, xdg-output, layer-shell,
session lock, idle notify and inhibit, gamma control, wlr output power management,
presentation time,
tearing control, linux-drm-syncobj, relative pointer, pointer constraints,
virtual pointer and keyboard, xdg-activation, wlr and ext foreign toplevels,
wlr-screencopy-v1 and ext-image-copy-capture for outputs and toplevels (Tomoe's
own, in `native/capture.c`; a toplevel session that asks for cursors gets the cursor
while the pointer is over the window; pointer cursor sessions report stopped),
ext-background-effect-v1, and X11 clients through
xwayland-satellite. The ScreenCast portal is the Rust `xdg-desktop-portal-tomoe` in
`portal/`, carried over from the previous tomoe and installed with its
`.portal`, `portals.conf` and D-Bus service files. Each share is a portal
session object: the app closing it or leaving the bus, xdg-desktop-portal
leaving the bus, the cast window closing or the cast output going away ends
its stream and removes its PipeWire node. Input methods, touch and
tablets are missing. A policy that never releases a grab keeps the pointer until
the grabbed surface disappears. A layer surface's anchors, margins, and size
stay the client's request.

Tomoe and ShojiWM informed the separation of mechanism from policy and explicit
ownership of reactive effects. wlroots 0.20, which Tomoe was built on
until it replaced each layer, informed native API use and fallback paths.
