# moonshell

> **Superseded (2026-07-24, FUSION.md F0).** moonshell merged into
> tomoe as its in-process shell subsystem; the standalone,
> compositor-agnostic client described below is retired (a transitional
> binary remains until F6). See `../FUSION.md` for the decision and
> tracker, `../DESIGN.md` for the record. This document is kept as
> history of the standalone project.

A Lua-scriptable Wayland desktop shell (bars, launchers, OSDs,
notifications, lock screens) built for footprint: CPU-rendered into
wl_shm, no GPU in the process, single-threaded calloop, LuaJIT. Target:
idle RSS under 25 MB where AGS/QuickShell sit at 100–250 MB.

Configs are programs: `~/.config/moonshell/init.lua`, using the
`shell.*` / `ui.*` API inherited from nur (moonshell is its successor).
It was compositor-agnostic — tomoe, niri, Hyprland, and Sway as IPC
backends — and the first external consumer of tomoe's `tomoe-ipc` wire
protocol; both roles ended with the fusion.

See `moonshell-DESIGN.md` (vision, doctrine conformance, locked
decisions, roadmap) and `moonshell-PLAN.md` (work tracker).
