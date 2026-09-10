# Output resolution and pixel mapping

## Goal and completion criteria
Add Tomoe/Niri-style output mode, refresh, position, and scale configuration to the installed Lisp compositor. Support sharp fractional-scale clients and report physical versus logical output dimensions. Carry over the user's native panel configuration, build through Nix, verify real isolated sessions, and update the installed user-profile package without restarting the active desktop.

## Constraints
Preserve existing startup repair and concurrent pure-Lisp backend/dev launcher work. No tests, system rebuild/activation, active desktop takeover, commits, or pushes. One edit call per existing file. Runtime and control sockets stay outside the Nix source tree. Hardware mode changes require a later compositor restart and are not exercised from this agent session.

## Findings
- Running artifact is the previous fixed wlroots build, PID 31689. Live inspect reports DP-4 at 3840x1080.
- `/home/y0usaf/finix/modules/hosts/y0usaf-desktop/ui.nix:8` documents a 5120x1440@239.761 panel whose EDID preferred mode is 3840x1080@60. Tomoe explicitly requests 5120x1440 at highest refresh; HDMI-A-2 is configured for 1920x1080@60 at x=5120.
- Lisp currently always picks `wlr_output_preferred_mode`, uses automatic horizontal placement, and exposes only logical geometry.
- Tomoe exposes physical-pixel layout with per-output client scales; Niri separates logical layout from physical output mode and snaps scale to N/120. Keep Lisp's existing logical-coordinate layout rather than changing all window/input coordinates.
- wlroots 0.20 scene helpers already handle client buffer scales/transforms, output enter/leave, viewport mapping, and fractional-scale notification once protocol managers are enabled.
- User source contains new `backend/`, `scratch/`, `dev.lisp`, and `dev.sh` work. Do not overwrite it. The pure-Lisp backend has no hardware outputs yet.

## Direction
Use an ordinary owned extension effect for output policy, keeping the existing single runtime commit path. Resolve unsupported modes before applying changes; use wlroots backend-wide preflight and restore prior state on an actual commit failure. Unmount restores baseline output settings. Expose mode lists, physical size, logical size, refresh in mHz, and scale in 120ths through inspect. Enable viewporter, fractional-scale-v1, and xdg-output support. Add a standard optional user init file for persistent per-machine policy.

## Pending
1. Finish native API details and snapshot affected files.
2. Implement API/runtime/native output configuration and update documentation.
3. Build and run existing Nix checks; operate isolated headless/nested instances with real clients for scaling and policy removal.
4. Add user's DP-4/HDMI-A-2 policy, refresh only Finix's Lisp input, and upgrade only its user-profile entry.
5. Confirm the active compositor remains unchanged, record artifacts and limits, and report how the next launch picks up the new build.
