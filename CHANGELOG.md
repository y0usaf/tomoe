# Changelog

## 0.1.0

The first release of Tomoe as a Common Lisp compositor. The earlier Rust and
Lua tomoe stays in the history at `6de3ba6`; nothing here is compatible with
its Lua configs.

- **Policy in Lisp.** Every desktop behaviour is an extension: a function from
  a snapshot, its state and an event to new state, owned effects and one-shot
  commands. Effects revert when their owner unmounts, a reload keeps state for
  matching names, and a source that fails to load leaves the last good policy
  running and shows the error.
- **The shipped desktop.** Dwindle tiling over nine workspaces with gaps,
  Super-drag to move and resize, a hotkey sheet, exit confirmation,
  notification popups, screenshots to the clipboard, a wallpaper picker, a
  screencast portal, and X11 through xwayland-satellite started on the first
  X11 connection. All of it uses the public API and can be replaced with
  `--bare`.
- **Examples.** Monocle, floating, deck columns, the zoomer canvas, tag
  workspaces, scratchpads, layer insets, a bar, service indicators, and five
  shader wallpapers (xmb, aurora, stars, towers, cubes).
- **Its own native core.** No wlroots: C on libwayland-server for the
  protocols, an EGL/GLES2 renderer on GBM with damage tracking, DRM/KMS on
  libseat with explicit sync, libinput, and nested and headless backends.
- **Outputs as code.** Modes, scale, placement, mirroring, adaptive sync, ICC
  profiles and power, declared by extensions and settled before they reach the
  hardware.
- **Shell surfaces.** Bars, panels and wallpapers from a declarative UI tree,
  drawn with Pango and Cairo or a GLSL shader, with rounded corners, shadows,
  blur and spring or eased animations.
- **Session services.** A notification server, MPRIS, battery, backlight
  through logind, sounds through PipeWire, NetworkManager, a StatusNotifier
  tray, and CPU, memory and GPU readings.
- **Control.** `tomoe inspect`, `mount`, `unmount`, `reload`, `command`,
  `event` and `quit`, plus JSON IPC (wire version 2) for bars and scripts.
- **One executable.** SBCL's runtime with the C core linked in and the Lisp
  image appended, shipping foot, fuzzel, xwayland-satellite and D-Bus.

Known limits: no input methods, touch or tablets; shell surfaces cannot take
keyboard focus; development happens on NVIDIA and an AMD Framework laptop under
DRM, so other GPUs see less testing and adaptive sync is unverified on hardware; Linux 6.9 or newer is
required; Nix is the only supported way to build.
