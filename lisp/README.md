# Tomoe Lisp

A new Common Lisp Wayland compositor. SBCL runs the compositor loop, extension
runtime, window-management policy, and control server. A C library connects it
to wlroots 0.20 for Wayland protocols, rendering, outputs, and input devices.

This is a working prototype, not a complete desktop compositor or a pure-Lisp
Wayland implementation. It does not wrap the Rust/Lua implementation next door.

## Run

From this directory:

```sh
nix build
nix run .
```

The default backend is `auto`. It opens a nested window when a parent Wayland
display is available; otherwise it uses DRM for a direct session from a TTY.
An explicit `WAYLAND_DISPLAY` takes priority. When it is unset or empty, startup
looks for a live `wayland-N` socket under `XDG_RUNTIME_DIR`, skipping stale
sockets and lock files. No manual environment export is needed for discovery.

`--backend nested` uses the same display discovery but fails if no parent is
available. It never falls back to hardware. The compositor creates
`tomoe-lisp-0` under `XDG_RUNTIME_DIR`. It does not change your systemd, D-Bus,
display-manager, or surrounding desktop environment.

Explicit modes:

```sh
nix run . -- --backend nested
nix run . -- --backend headless
nix run . -- --backend drm
nix run . -- --bare --backend headless
```

DRM mode takes direct control of outputs and input devices. Run it from an
appropriate TTY with seat access. It has not been verified on hardware.
`--bare` loads no extensions. Clients can still map and render at their own
initial size, without focus policy or shortcuts.

`XDG_RUNTIME_DIR` must be an owned directory with mode `0700`. Do not put live
runtime sockets inside this flake's source directory. Nix cannot copy sockets
into a source archive. `--socket NAME` permits independent instances. A socket
left behind by an exit that skipped cleanup is reclaimed on the next start; a
name a live instance answers on is refused.

The packaged default terminal is Foot. Shipped bindings use Super:

| Binding | Command |
| --- | --- |
| Super+Return | Open a terminal |
| Super+Tab | Focus the next window |
| Super+q | Ask the focused client to close |
| Super+Shift+r | Reload configured extension files |
| Super+Shift+Escape | Quit |

The default layout tiles horizontally on the first output. Outputs and all
window coordinates are available to Lisp, so a replacement can use the others.
Native output modes and physical output arrangement are not yet configurable
through Lisp.

## Live control

These commands attach to a running instance and then exit:

```sh
nix run . -- inspect
nix run . -- command commands terminal
nix run . -- command focus next
nix run . -- unmount tiles
nix run . -- mount "$PWD/examples/monocle.lisp"
nix run . -- unmount monocle
nix run . -- reload
nix run . -- quit
```

`inspect` prints versioned Lisp data containing live windows, outputs, resolved
geometry, focus, bindings, extension state, dispatch counts, and the last error.
Mutating commands print nothing on success and return a nonzero exit status on
failure. `command OWNER NAME` invokes an active binding through the same
extension dispatch as keyboard input.

Mounting a file replaces that file's units without reloading other files.
Unmounting removes one named unit and its state. Reloading rebuilds all configured
files, including units previously unmounted. It preserves state for matching
names in the same source file. Unmount before reload to reset a unit's initial
state. There is no automatic file watcher.

`examples/monocle.lisp` is an alternative layout that shows only the focused
window. Mount it after the default policy to override tiling. Removing it
restores the lower-priority layout without restarting clients.

## Write an extension

Pass an ordinary Common Lisp file with `--config /absolute/path/config.lisp`,
or mount it through control. Use `--bare --config ...` to replace every shipped
policy. `builtins/desktop.lisp` declares `tiles`, `focus`, and `commands` through
the same API as a user file. The host has no special cases for these names.

The public API is in `src/api.lisp`:

```lisp
(define-extension "name" (:reads (:windows :outputs) :state nil)
    (snapshot state event)
  ;; Return new state, the complete set of owned effects, then one-shot commands.
  (values state nil nil))

(context snapshot :windows)
(place id x y width height visible)
(focus id)                         ; NIL clears focus
(bind-key '(:super :shift) "r" :reload)
(launch "foot")                    ; argv, not a shell command string
(close-window id)
(quit)
(reload)
```

Reducers receive a copied pre-dispatch snapshot, copied private state, and an
event property list. A property list alternates keys and values, such as
`(:id 1 :width 1280)`. Mutating a supplied list or string cannot mutate host
state or another reducer's snapshot. Return state as data, not closures or
native objects.

Declare every context key read with `:reads`. `context` rejects undeclared
reads. Available keys:

- `:windows`: property lists with `:id`, `:title`, `:app-id`, `:width`, `:height`.
  Width and height are the client's initial mapped dimensions.
- `:outputs`: property lists with `:name`, `:x`, `:y`, `:width`, `:height`.
- `:layout`: resolved placement, with `:id`, coordinates, dimensions, and `:visible`.
- `:focus`: a window ID or `nil`.
- `:bindings`: resolved modifiers, keysyms, owner names, and command names.
- `:key` and `:button`: event subscriptions, not stored context values.

Events include `:map`, `:unmap`, `:metadata`, `:outputs`, `:key`, and `:button`
in their `:type` field. Key events carry `:owner` and a lowercase `:command`
string. Button events carry `:id`, or zero for empty space, and an evdev
`:button` code. Lifecycle evaluation receives `:mount`; reactive reevaluation
receives `:change` with the changed `:keys`.

Owned effects are `place`, `focus`, and `bind-key`. Return the complete desired
set each time. Later-mounted units win conflicts. Omitting an effect removes
that unit's contribution. `place` uses integer logical coordinates and positive
sizes up to 16384. Bindings accept `:super`, `:alt`, `:control`, and `:shift`,
plus an XKB keysym name such as `Return` or `Tab`.

`launch`, `close-window`, `quit`, and `reload` are one-shot commands. Only key,
button, and explicit control command dispatch may return them. They execute
after effect validation and commit. They are not undoable. User-launched
applications belong to the session, survive extension unmount, and have their
direct child processes stopped on compositor shutdown.

## Extension lifecycle

`src/runtime.lisp` owns the context and the only extension-to-native write path.
Each dispatch works on candidate module state. Each reducer in a round sees the
same pre-round context. After the round, the runtime computes changed keys and
runs only their declared consumers. The transaction must settle within 16
rounds before it reaches the native scene.

A dispatch has a 25 ms SBCL timeout, at most 512 effects and 32 commands, bounded
data copying, and no host handles. Loading a source file has a one-second
timeout. Invalid results, undeclared reads, callback errors, and dependency
cycles retain the previous managed policy. Errors appear on stderr and through
`inspect`. Native allocation failure during commit stops the compositor rather
than pretending it rolled back. One-shot command failures cannot undo earlier
commands.

Unmount reconstructs geometry, visibility, stacking, focus, and bindings from
remaining owners. The preserved facts are live client identities and metadata,
initial client sizes, map order, and output descriptions. Unowned windows revert
to their mapped dimensions at the origin. A client's destruction is an external
fact, so failure recovery also removes dead IDs from the previous policy.

Extensions are trusted code, not a security sandbox. Common Lisp can call the OS,
redefine internals, change global variables, or create threads. Such side effects
are outside managed cleanup and reload rollback. SBCL timeouts are best-effort
runtime protection, not containment for hostile code or blocking foreign calls.
The compositor thread waits for bounded reducer dispatch; there is no separate
policy worker. Returning state and actions is the extension contract.

## Control protocol

The private Unix socket is `$XDG_RUNTIME_DIR/NAME.ctl`, mode `0600`. It accepts
one request per connection. A frame is a decimal character count, a newline,
then that many UTF-8-decoded characters of Lisp data. The maximum is 1048576
characters. Framing permits newlines inside window titles.

Requests are `(1 :inspect)`, `(1 :reload)`, `(1 :mount "path")`,
`(1 :unmount "name")`, `(1 :command "owner" "command")`, or `(1 :quit)`.
Replies are `(1 :ok result)` or `(1 :error "message")`. Version 1 is exact;
unknown versions fail explicitly. Reader evaluation and dispatch syntax such as
`#.` and circular object labels are disabled. This is a data protocol, not an
unauthenticated REPL.

## Verification

Observed on x86_64 Linux:

- `nix build` and `nix flake check --all-systems` completed successfully. The
  aarch64 package was evaluated, not built.
- Two headless outputs hosted three real Foot windows. Mounting monocle,
  cycling focus, unmounting layouts, reloading, and closing clients worked.
- Removing every extension left the clients alive at their initial dimensions,
  with no bindings or focus. Unrelated units did not rerun on layout removal.
- A second, bare compositor ran nested inside that isolated parent. A real Foot
  client attached buffers and received frame callbacks without any policy units.
  Both instances removed their sockets and stopped on quit.

Hardware DRM, physical input, timeout/failure recovery, and exhaustive protocol
behavior remain unverified. No automated test suite was added.

## Source and limits

- `src/`: Common Lisp API, state/effect runtime, native bindings, control, CLI.
- `native/backend.h`: the native ABI. No pointers cross into extension snapshots.
- `native/backend.c`: wlroots lifetimes, scene rendering, xdg-shell, input routing.
- `builtins/desktop.lisp`: replaceable default policy.
- `examples/monocle.lisp`: independent layout using declared dependencies.
- `flake.nix`, `build.lisp`: native compilation and saved SBCL executable.

Implemented protocols cover ordinary xdg-shell windows and popups, shared-memory
buffers, subsurfaces, and clipboard selection. Missing desktop features include
layer-shell, XWayland, fullscreen/maximize policy, interactive dragging/resizing,
output configuration, fractional scaling, session locking, primary selection,
screencopy, portals, touch/tablets, and input methods. Popup placement does not
constrain menus to output bounds. Do not use this as a secure daily desktop.

Tomoe and ShojiWM informed the separation of mechanism from policy and explicit
ownership of reactive effects. Local reference clones live in `../ref/`, which
is listed in the workspace `.gitignore`. The wlroots tinywl example and 0.20
headers informed native API use. This implementation does not copy either
reference compositor's Rust code.
