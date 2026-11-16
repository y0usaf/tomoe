# Changelog

Each release lists what changed since the one before it. `nix run .#release`
cuts a candidate only from a commit whose `flake.nix` version has a section here.

## 0.1.0

The first release.

- A Wayland compositor with its own C core: it speaks the wire protocol and runs
  its event loop itself, drives KMS through the kernel's ioctls, renders with EGL
  and GLES2 on GBM, and runs on DRM from a TTY, nested in another compositor, or
  headless. No wlroots.
- The window manager, bar and wallpaper are Lisp extensions, in a Common Lisp
  dialect the C core carries, loaded from `~/.config/tomoe/init.lisp`, reloaded
  on save and live-patchable with `tomoe command patch`. A config that fails to
  load is refused and rolled back; the running one keeps going.
- One command table for key bindings, the `tomoe` command line and the JSON
  socket.
- Built-in X11 bridge (Xwayland), notification server, screencast portal, session
  services (media, battery, backlight, sounds, network, tray) and a session bus
  that is replaced when it dies; services die with the compositor.
