# tomoe

A Wayland compositor where the window manager is yours to rewrite.

The Rust/[Smithay](https://github.com/Smithay/smithay) core exposes mechanism
— windows, outputs, input, a camera over an infinite canvas — and all window
management policy is Lua, hot-reloaded on save. A tiling WM and a
zoomable-canvas WM ship as plain Lua modules; extend or replace them
wholesale. Geometry is integer physical pixels, so rendering stays
pixel-exact at any scale. JSON IPC (`tomoe msg`), declarative process
management, screencasting, session lock, VRR, tearing control, XWayland via
xwayland-satellite.

Under active development, not yet stable.

## Build

With Nix: `nix build`, or `nix develop` for a dev shell.

Without: a stable Rust toolchain, `pkg-config`, `libclang`, and dev headers
for EGL/GBM (mesa), wayland, libinput, libseat, libxkbcommon, libudev,
libdisplay-info, dbus, and pipewire. LuaJIT is vendored — no Lua needed.

```sh
cargo build --release            # target/release/{tomoe,xdg-desktop-portal-tomoe}
cargo install --path crates/tomoe
```

Run nested inside an existing session with `tomoe --backend winit`, or from a
TTY with `tomoe --backend tty`. For screencasting, install the portal binary
and the files in `resources/` (`tomoe.portal`, `tomoe-portals.conf`, D-Bus
service) as the flake's `postInstall` does.

## Configure

`~/.config/tomoe/init.lua`; without one, a built-in default keeps the session
usable. Full API reference: [docs/lua-api.md](docs/lua-api.md). Point LuaLS
at `resources/meta/` for completion and type checking.

## Credits

An original implementation, designed by studying
[niri](https://github.com/YaLTeR/niri) (damage-driven rendering, pixel
exactness), [Hyprland](https://github.com/hyprwm/Hyprland) (the feature bar),
and [ShojiWM](https://github.com/bea4dev/ShojiWM) (config as a program).

## License

[AGPL-3.0-or-later](LICENSE).
