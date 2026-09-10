# Lisp backend M1 checkpoint

## Status
Verified. A real Wayland terminal (foot) maps a window and draws into it on a
compositor whose server, event loop, protocol handling, and object lifetimes are
SBCL with `libwayland-server`. No wlroots, no hand-written C, no compiled shim.
`lisp/native/` still exists and still backs the packaged compositor.

## Goal and constraints
Replace the wlroots C shim with a backend written in Common Lisp, reachable
through the same 11-function contract the policy runtime already calls, without
breaking the packaged build or the Finix input at `~/finix/flake.nix:197`.
No tests. No system rebuild.

## What exists now
- `backend/gen-protocols.py`. Turns a wayland-protocols XML file into Lisp
  interface tables (opcodes, signatures, interface references).
- `backend/core-protocols.lisp`, `backend/xdg-shell.lisp`. Generated data for
  18 core interfaces and 5 xdg-shell interfaces.
- `backend/wl.lisp` (222 lines). The only file that calls C: 16 alien routine
  declarations, foreign memory layout as byte offsets, interface struct
  building, argument marshalling and decoding.
- `backend/server.lisp` (457 lines). One object registry, one dispatcher
  callable, one bind callable, one destroy callable, table-driven handlers,
  and the backend contract (`open-backend`, `%step`, `%event`, `%place`,
  `%focus`, `%close`, `%bind`, `%clear-bindings`, `%keysym`, `%destroy`).
- `scratch/m1.lisp` (27 lines). The runner used for verification.

## Key decisions
- Protocol tables are generated from XML, never transcribed. The signature
  encoding is wayland-scanner's: version prefix, `?` for nullable, then the
  type letter. Hand-written tables would have missed `?oii` on
  `wl_surface.attach` and produced the "NULL object on non-nullable type"
  failure foot hit.
- Objects are plists in one hash table, mutated through `set-object`. Setting a
  new key can move a plist head, so the record is written back. Handlers are
  stored as symbols and looked up per request, so reloading these files
  redefines behaviour without touching live clients, the socket, or globals.
- Globals advertise conservative versions (wl_compositor 4, wl_shm 1,
  wl_output 2, wl_seat 5, wl_data_device_manager 3, xdg_wm_base 1) so no client
  can call a request that is not implemented.
- `struct wl_interface` and `struct wl_message` are built from byte offsets.
  `sb-alien:define-alien-type` struct specs fail to parse anywhere outside
  CL-USER ("unknown alien type"), which was verified in isolation.
- `libwayland-server` passes the *resource* as a dispatcher's target, not the
  data pointer. The registry id travels as the resource's user data.
- Interface state that already works stays: `src/api.lisp`, `src/runtime.lisp`,
  `src/control.lisp`, `builtins/`, `examples/` are untouched.

## Evidence
- `wayland-info` binds all seven globals and reads our output geometry
  (`make: 'tomoe', model: 'nested'`), mode, scale, seat name, and shm formats.
- `foot sleep 4` maps a window: `(:TYPE :MAP :ID 14 :TITLE "" :APP-ID "foot"
  :WIDTH 1280 :HEIGHT 672)`, in the exact event shape `dispatch-event` expects.
- Client pixels are readable from Lisp: `/tmp/tomoe-m1.ppm` is 1280x672 P6
  (2580496 bytes) written from the mapped client buffer.
- 23 interfaces built from generated tables; 54 handler table entries.
- Graceful destroy removes the Wayland socket. libwayland's own `.lock` file
  is left behind.

## Limits
- No input: `wl_seat` advertises zero capabilities, so no keyboard, pointer,
  or keymap. `%bind` and `%keysym` accept and validate, and nothing dispatches.
- No presentation: pixels are read, not shown. There is no nested output and no
  DRM path, so the compositor cannot yet draw a client anywhere.
- Clipboard is accepted (`wl_data_device_manager` exists, set_selection is
  recorded) but no offer is ever sent, so pasting does nothing.
- No reload or file watcher yet. The structure supports both; nothing calls
  them.
- Not packaged: `lisp/flake.nix` still builds `native/`, and the new files sit
  outside its fileset. Runtime needs `libwayland-server.so.0` and
  `libxkbcommon.so.0` on the library path.
- No Git repository exists here, so nothing is committed.

## Dev loop and shipping
- `lisp/dev.sh` runs the compositor from the working tree, no Nix build and no
  saved image. The Lisp backend needs no compiled shim; the wlroots backend
  compiles `native/backend.c` into `build/` on first use.
- `~/tomoe-lisp-dev` wraps that in a launcher, so edits take effect on the next
  run. `~/tomoe-lisp` still launches the installed profile package.
- Source is pushed to https://github.com/y0usaf/tomoe-lisp (private). Finix
  consumes it through the local path input, so iteration does not need a push.
- `nh os switch` activated the configuration on 2026-09-10; the system package
  is `/nix/store/hchqnb1wikk98lb3dkby2w67yash7sl2-tomoe-lisp-0.1.0`.

## Next
1. Input: libinput-free path first (a parent compositor's wl_seat in nested
   mode), keymap via xkbcommon, then `%bind` dispatch through the runtime.
2. Presentation: nested output as a client of the parent (`libwayland-client`
   plus `wl_shm`), so a window is visible without DRM.
3. Swap: point `src/main.lisp` at this backend, delete `lisp/native/` in the
   same change, and add the new files plus their libraries to the flake.
4. Reload: re-evaluate the backend files at a safe point in the loop, then a
   mtime watcher for automatic updates.
