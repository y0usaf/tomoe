# Desktop increment: layer-shell, window state, grabs, live reload, end-to-end check

## Status
Complete and verified through Nix. `nix build` and `nix flake check` both exit
zero; the end-to-end check passes 40 of 40 assertions inside the Nix sandbox and
from the working tree. No system rebuild, activation, push, or change to the
user's running desktop session.

## Goal and constraints
Take the Lisp compositor from a runnable prototype to a compositor that can be
driven as a desktop, without giving up the property that makes it interesting:
every desktop behaviour is an ordinary Lisp extension over the same API a user
file gets. Work landed in `lisp/` only; `ref/` stayed untouched.

User decisions during the work: no unit tests; end-to-end checks only.

## Delivered
- `lisp/native/backend.c`, `lisp/native/backend.h`: native ABI 3. wlroots
  layer-shell with per-output arrangement, exclusive zones, keyboard
  interactivity, popups and visibility; xdg fullscreen and maximize state with a
  fullscreen scene layer between the top and overlay layers; a policy-owned
  pointer grab that suppresses client delivery and reports deltas. The scene is
  now four layer trees, a window tree, and a fullscreen tree, so raising windows
  no longer lifts them above panels.
- `lisp/src/api.lisp`, `lisp/src/runtime.lisp`, `lisp/src/native.lisp`: the
  policy surface. Effects `layer`, `fullscreen`, `maximize`, `grab`; context
  keys `:layers` and `:grab`; `:fullscreen`/`:maximize` on windows and resolved
  layout entries; events `:layer` and `:grab`, `:button` with state,
  coordinates and modifiers, `:metadata` with `:request`. Resolution is
  last-mounted-owner-wins per effect key, and `commit-context` writes window
  state, layer overrides and the grab only when a resolved value changed.
- `lisp/src/runtime.lisp`: the `extension-error` condition, so a failure is
  attributed to the unit that caused it. `inspect` reports per-extension
  `:failures` and `:last-error`; whole-transaction rollback is unchanged.
- `lisp/src/main.lisp`: source watching — `--watch`/`--no-watch`, a sweep four
  times a second, reload after two identical consecutive samples, silent on
  success, previous policy kept on failure.
- `lisp/src/control.lisp`, `lisp/src/main.lisp`: `(1 :event "PLIST")` and the
  `event` subcommand, which injects `:key`, `:button`, or `:grab` events into a
  live instance so a policy can be driven without a seat.
- `lisp/builtins/desktop.lisp`: the shipped policy is now `tiles` (inset by
  layer exclusive zones), `focus`, `window-state` (Super+f, Super+m, accepts or
  drops client requests), `drag` (Super+left moves, Super+right resizes, grab
  owned for the duration), and `commands`.
- `lisp/examples/`: `workspaces.lisp` (nine tags), `float.lisp` (floating with a
  Super drag), `layer-inset.lisp` (bar-aware tiling), alongside `monocle.lisp`.
- `lisp/tests/`, `lisp/flake.nix`, `.github/workflows/ci.yml`: one end-to-end
  check, built as a Nix check and run by CI.
- `lisp/README.md`, `README.md`: both documents describe the surface above.

## Key decisions
- The project keeps end-to-end checks only. The unit-test suite that a first cut
  of this increment produced was deleted, together with the two manual
  verification runners that lived in `lisp/scratch/`; `lisp/tests/` and its Nix
  check are the whole automated suite.
- Mechanism stays native, policy stays Lisp: layer surfaces are arranged from
  the client's own anchors, margins and exclusive zone, and policy can only
  reassign the layer, change the exclusive zone, change keyboard interactivity,
  or hide the surface. Window geometry for a fullscreen window is policy's job
  through `place`.
- Events carry external facts, never policy-derived state. This was a real bug
  in the first cut: `:layer` events reported the resolved override, so the
  runtime stored the override as the client's request and an unmount could never
  restore it. The end-to-end check caught it.
- The watcher's baseline is the content the runtime loaded, recorded inside
  `configure`, not the first thing the watcher sees. A source's stamp combines
  write date, size, and a content digest, because the write date resolves to one
  second and a same-length edit inside that second is otherwise invisible.
  Sweeping reads each source, which is cheap for files that must stay in the
  page cache, and is the honest price for correctness without inotify.
- The control reader now delegates list reading to the standard reader, keeping
  the depth bound. It previously rejected the cons dot that extension state
  naturally contains, which made `inspect` fail for any policy keeping an alist.
- nixpkgs' wlroots ships no generated protocol headers, so `flake.nix` and
  `dev.sh` generate `wlr-layer-shell-unstable-v1-protocol.h` from
  `pkgs.wlr-protocols` with `wayland-scanner` before compiling the backend.
- The end-to-end check compiles its own Wayland client from `tests/client.c`
  instead of shelling out to `foot`, so it is deterministic, needs no fonts, no
  pty, no GPU, no D-Bus and no network, and runs inside the Nix sandbox.

## Verification evidence
- `cd lisp && nix build . -L` exited zero; artifact
  `/nix/store/83xp1gackxafnmkv3r4srp46bj2ybs3v-tomoe-lisp-0.1.0`.
- `cd lisp && nix flake check -L .` reported `40 passed, 0 failed` and
  `all checks passed!`. The package built with `-Wall -Wextra -Werror` and Lisp
  modules compiled without warnings.
- The same check from the working tree, driving `build/tomoe-lisp` and a locally
  compiled client: `40 passed, 0 failed`.
- A throwaway smoke run (`/tmp/smoke.sh`, not part of the repository, 24
  assertions) covered what the check does not: an owned fullscreen effect sizing
  a real xdg client to 1280x720 and reverting to 320x240 on unmount; an owned
  grab started by an injected key, moved by an injected `:grab` delta, cleared
  by an injected button release; a same-length source edit reloaded with no
  command; and the four example policies mounting with no runtime error.
- The pure-Lisp backend still loads: `dev.sh --backend lisp --socket pl0` plus
  `inspect` returned the shipped policy's bindings and the M1 output, with no
  native error beyond its documented lack of output configuration.

## Fixes made during verification (all found by running, not by reading)
1. Layer events published overridden state, making overrides irreversible.
2. `source-stamp` could not see a same-length edit in the same second.
3. The watcher used its first sweep as the baseline, so an edit made while a
   mount was still settling was lost.
4. The control reader rejected dotted pairs, breaking `inspect` for any
   extension with alist state (the `workspaces` example).
5. `examples/workspaces.lisp` and `examples/float.lisp` passed an unquoted
   property list as `:state`, which `define-extension` evaluates; documented in
   `lisp/README.md` and fixed in both files.
6. `${pkgs.wayland-scanner}/bin/...` does not exist for the pinned nixpkgs; the
   scanner lives in the package's `bin` output.
7. The check's fileset root and a missing C compiler in `nativeBuildInputs`.

## Remaining limits
Hardware DRM and physical input are still unverified. The pointer grab is
verified through injected events and the native suppress path, not by a real
pointer. A policy that never drops a grab keeps the pointer until the grabbed
surface disappears. Layer surfaces cannot be placed freely by policy. XWayland,
session locking, screencopy, portals, input methods, touch and tablets, output
rotation and mirroring, and window decorations are not implemented. Failure
recovery for native allocation failure is not exercised.
