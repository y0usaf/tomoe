# Tomoe Lisp startup repair

## Status
Complete. `~/tomoe-lisp` resolves through the existing user profile to the fixed package. No system build or activation, desktop takeover, commit, push, or new tests. Hardware DRM was not exercised.

## Goal and constraints
Make `~/tomoe-lisp` start without requiring a manual `WAYLAND_DISPLAY` export, matching Tomoe's automatic backend choice. Preserve unrelated Finix edits. Keep source outside Finix. Verify with real isolated Wayland processes, not hardware. Source is not a Git checkout.

## Root cause
The Lisp CLI defaulted unconditionally to nested mode and rejected an absent display variable. Original Tomoe defaults to automatic backend selection. In this session `WAYLAND_DISPLAY` and `DISPLAY` are absent; `/run/user/1001/wayland-1` exists but refuses connections. No live parent Wayland listener was found. Merely exporting `wayland-1` still failed.

## Changes
- `lisp/src/main.lisp:34`: use an explicit, nonempty `WAYLAND_DISPLAY` when present; otherwise scan standard `wayland-N` sockets in the validated runtime directory and reuse `socket-answering-p` to skip dead listeners. Lock/control files are excluded.
- `lisp/src/main.lisp:46`: `auto` uses nested mode when a parent is found, otherwise DRM. Explicit nested mode uses discovery but never falls back to hardware and reports how to choose a custom socket or other backend.
- `lisp/src/main.lisp:9` and `:90`: update CLI help and default backend to `auto`.
- `lisp/README.md:19`: document automatic selection and discovery.
- `/home/y0usaf/finix/flake.lock`: refreshed only the `tomoe-lisp` node. No nodes added or removed; every other node stayed identical to the immediate pre-update snapshot.
- `nix profile upgrade --no-update-lock-file tomoe-lisp` updated only the existing compositor entry. `~/tomoe-lisp` remains a symlink through `.nix-profile/bin/tomoe-lisp`.

## Verification
- `cd lisp && nix build -L && nix flake check --all-systems` exited zero. C and Lisp compilation reported no warnings. x86_64 package built; aarch64 outputs evaluated only.
- Fixed artifact: `/nix/store/i9wffpjp4346f6khib908f4spziv742x-tomoe-lisp-0.1.0`.
- Explicit nested mode in the original environment skipped the stale `wayland-1` socket and returned an actionable no-live-display error instead of a missing-variable error.
- Started a real isolated headless parent at `wayland-7`. With `WAYLAND_DISPLAY`, `DISPLAY`, `WAYLAND_SOCKET`, `WLR_BACKENDS`, and `WLR_RENDERER` unset, a no-argument launch discovered it and created nested output `WL-1`.
- The built-in terminal command launched a real Foot client. Inspect reported mapped/tiled geometry 1248x688, focus 1, and `:LAST-ERROR NIL`.
- Explicit nested mode also discovered the parent when `WAYLAND_DISPLAY` was an empty string.
- An explicit absolute `WAYLAND_DISPLAY` pointing at the first nested compositor took precedence over discovered `wayland-7`. The selected parent showed both Foot and the child compositor mapped with no runtime error.
- All compositor processes exited zero on `quit` and removed their Wayland/control/lock sockets. Foot's configured shell spawned an independent Ekko daemon in the isolated runtime directory; cleanup targets only that owned daemon and its temporary files.
- Foot emitted existing host Fontconfig and unsupported optional-protocol warnings. Parent and nested compositor logs were otherwise empty.
- `nix build --no-update-lock-file /home/y0usaf/finix#tomoe-lisp --no-link --print-out-paths` exited zero and returned the same fixed artifact.
- Installed `~/tomoe-lisp --help` reports the new `auto` default; `readlink -f ~/tomoe-lisp` points to the fixed artifact's launcher.

## Artifacts
Source and documentation baselines, pre-update Finix locks, profile manifest, and operational logs are under `/tmp/tomoe-lisp-startup.FyspGa`. `/tmp/tomoe-lisp-startup-backup-path` records that directory. Updated input NAR hash is `sha256-uXjV06e6ullroEUviXRiwt8OM2sQNENXDZEvl7fZCNM=`.

## Remaining limits
Hardware DRM and physical input remain unverified. Explicit nested mode still needs a real parent compositor; automatic discovery cannot turn a stale socket into one. No implementation or installation work remains for this request.
