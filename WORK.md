# Lisp compositor checkpoint

## Status
Complete as a runnable prototype. No compositor or client started during this task remains running. Hardware and failure-recovery limits are documented, not claimed verified.

## Goal and constraints
Build a new runnable Wayland compositor in Lisp with replaceable, reloadable policy. User permitted Tomoe and ShojiWM references under gitignored `ref/`. Preserve concurrent Rust/Lua work, moved by another actor into root `rust/`. Keep this implementation under `lisp/`. Do not add tests, change the user's desktop session, or rebuild the system. Do not promise perfect extensibility.

## Delivered
- `src/api.lisp`: Common Lisp extension declarations, declared context reads, copied data, validated effects and commands.
- `src/runtime.lisp`: staged reducer dispatch, dependency propagation, state preservation, mount/reload/unmount, reconstructed effects, session-owned application processes.
- `src/native.lisp`, `native/backend.h`, `native/backend.c`: narrow native ABI and wlroots backend for protocol lifetimes, scene rendering, outputs, pointer/keyboard routing, xdg-shell windows/popups, clipboard.
- `src/control.lisp`, `src/main.lisp`: version-1 framed Lisp-data IPC, inspect/control client, startup and shutdown.
- `builtins/desktop.lisp`: three ordinary extensions for tiling, focus, commands. `examples/monocle.lisp`: separately mountable focus-reactive layout.
- `flake.nix`, `flake.lock`, `build.lisp`: pinned Nix build and SBCL executable, with Foot available for the terminal command. `README.md`: usage, API, lifecycle, trust limits, and verification.
- Root `.gitignore` lists `/ref/` and build/result outputs. No root Git repository was present; no commits or repository initialization performed.

## Key decisions
- Common Lisp/SBCL owns the compositor loop, policy, runtime, and control. C/wlroots owns native Wayland/device machinery. This is not a pure-Lisp protocol stack or a port of either reference compositor.
- Effects are complete per-unit placement/visibility, focus, and binding contributions. Last-mounted owner wins. Unmount reconstructs map order, initial client sizes, and remaining contributions. Clients and output facts survive policy changes.
- Reducers receive isolated copies and return state, effects, and one-shot user commands. Individual dispatch has a 25 ms SBCL timeout; reactive propagation has a 16-round limit. Source load timeout is one second. SBCL TIMEOUT inherits SERIOUS-CONDITION, not ERROR; handlers account for that.
- Extensions are trusted Common Lisp, not sandboxed. OS calls, global mutations, redefinitions, foreign calls, and threads are outside managed rollback. Commands are irreversible and run after effect commit. Applications belong to the session, not an extension.
- Mounting a file does not reload unrelated files. Reload reinstates configured units and preserves matching state by name/source. Unmount removes a unit's state.
- Nixpkgs e52c192be9d7b2c4bd4aed326c8731b35f8bb75c supplies wlroots 0.20.1 and SBCL 2.6.4. Disable strip so the appended SBCL image survives. Use wayland-protocols' installed enum headers.
- IPC prints base strings with escaped printing, not SBCL's readable #A representation. Reader evaluation and dispatch syntax are disabled.
- Runtime sockets must stay outside the flake source tree. Nix path input ingestion rejects sockets even when the package fileset excludes them.

## Verification evidence
- `cd lisp && nix fmt -- flake.nix` succeeded.
- Final `nix build ./lisp -L --out-link lisp/result` succeeded. C compiled with -Wall -Wextra -Werror. Lisp modules and shipped policy compiled without warnings.
- Final `nix flake check ./lisp --all-systems` succeeded. x86_64-linux built; aarch64-linux evaluated only.
- Final artifact: `/nix/store/73ay4jcnb10qh86icsi1w6fw2khwxf3m-tomoe-lisp-0.1.0`.
- Real operation via `nix run ./lisp` used an isolated runtime directory and two headless outputs. Three Foot clients mapped, tiled, and acquired focus.
- Unmounting tiling restored original 700x482 dimensions. Mounting monocle did not remount tiling. Three focus cycles updated monocle visibility correctly. Unmounting monocle restored all clients' baseline visibility/dimensions. Unrelated unit dispatch counts stayed unchanged on layout removal.
- Reload rebuilt configured units without restarting clients. Closing the focused client removed its ID and moved focus to a remaining client.
- Unmounting all four units left two real clients alive with initial geometry, NIL focus, NIL bindings, and NIL extensions.
- `nix develop ./lisp --command wayland-info` connected and reported Wayland globals and both 1280x720 outputs.
- A bare nested instance ran inside the isolated parent with no extensions. A real Foot client connected, attached buffers, received frame callbacks, exited, and disappeared from context. Native and Lisp error logs were empty.
- Final build reran headless with a real Foot client plus mounted monocle; inspect showed valid geometry/focus and no runtime error. Graceful quit exited zero and removed all Wayland/control/lock sockets.
- No automated test suite was added. Physical keyboard/pointer input, hardware DRM, timeout/fault rollback paths, and exhaustive protocol/lifecycle behavior were not exercised.
- Foot emitted host Fontconfig and optional-protocol warnings. Core runtime inspect reported no error.

## References and artifacts
- `ref/tomoe`: dee1bdcd4ef6edf8cd7337e17ab1941c6851187a, public origin https://github.com/y0usaf/tomoe.git.
- `ref/shojiwm`: a6c5faac5c0f82d667f371d4156d9afc247c5625, public origin https://github.com/bea4dev/ShojiWM.git.
- Read applicable skills, reference architecture, wlroots 0.20 headers/tinywl, SBCL socket docs, timeout condition hierarchy, and Nix SBCL packaging conventions.
- Ignored `build/` holds compile/session/protocol logs and stale PID/path records. The private runtime directory was emptied by graceful shutdown.

## Pending
No implementation work pending within the prototype scope. README lists unsupported desktop features. No system activation or push requested or performed.
