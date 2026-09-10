# Tomoe Lisp

A Wayland compositor whose main loop, policy runtime, and protocol handling are
Common Lisp. SBCL runs the compositor; a narrow native boundary owns the parts
that need C.

Two backends satisfy one contract, so the policy runtime does not know which one
is loaded.

| Backend | Native side | State |
| --- | --- | --- |
| `native/` | wlroots 0.20 through a 29-line C header and a 1150-line C file | Packaged. xdg-shell, layer-shell, output configuration, pointer grabs. Verified with real clients, nested and headless |
| `backend/` | `libwayland-server` through `sb-alien`, no wlroots, no C of ours | M1. foot maps a window, the policy places it, `inspect` reports it |

Extensions are ordinary Common Lisp files. They declare the context keys they
read, receive a copied snapshot, and return state plus owned effects
(`place`, `focus`, `bind-key`, `configure-output`, `layer`, `fullscreen`,
`maximize`, `grab`) and one-shot commands. Unmounting a unit reconstructs the
layout, focus, bindings, output settings, layer overrides, window state, and the
grab from the remaining owners.

## Run it

```sh
nix build && nix run .               # packaged compositor (wlroots backend)
```

From the working tree, without a build or a saved image:

```sh
nix develop ./lisp -c ./lisp/dev.sh --backend lisp --socket dev0
```

That starts the Lisp backend with the shipped policy. Configured sources are
watched: edit a `.lisp` file and the running instance reloads it, no command and
no restart. Control commands work the same way against a running instance:

```sh
nix develop ./lisp -c ./lisp/dev.sh --socket dev0 inspect
nix develop ./lisp -c ./lisp/dev.sh --socket dev0 quit
```

## Check it

```sh
nix flake check ./lisp
```

builds the package, then runs one end-to-end check: a headless compositor, two
xdg clients, one layer-shell client, an injected command, an unmount, and a
clean quit. The driver and its fixtures live in `lisp/tests/`, and the same
check runs from the working tree:

```sh
nix develop ./lisp -c ./lisp/tests/run-integration.sh
```

## Layout

- `lisp/src/` owns the package, the extension API, the state and effect runtime,
  the control protocol, and the CLI.
- `lisp/native/` is the wlroots backend.
- `lisp/backend/` is the Lisp backend. `gen-protocols.py` generates interface
  tables from wayland-protocols XML, `wl.lisp` is the only file that calls C,
  and `server.lisp` holds the object registry, dispatcher, and handlers.
- `lisp/builtins/desktop.lisp` is the default policy, mounted like any other
  extension: `tiles`, `focus`, `window-state`, `drag`, and `commands`.
- `lisp/examples/` holds alternative policies: `monocle.lisp`, `workspaces.lisp`,
  `float.lisp`, and `layer-inset.lisp`.
- `lisp/tests/` is the end-to-end check, its fixtures, and its Wayland client.
- `LISP-BACKEND.md`, `FINIX.md`, `STARTUP.md`, `OUTPUTS.md`, `DESKTOP.md` are
  working checkpoints, written for a machine that has the rest of the context.
  They name local paths.

## Limits

The Lisp backend has no input and no presentation yet: a window maps, its
pixels are readable, and nothing is drawn to a screen. The wlroots backend has
both, plus a documented list of missing desktop features in `lisp/README.md`
(XWayland, session locking, screencopy, portals, input methods, touch and
tablets, output rotation and mirroring).

The control socket is a data protocol, not a REPL, and extensions are trusted
code rather than a sandbox.
