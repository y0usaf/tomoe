# Tomoe Lisp

A new Common Lisp Wayland compositor. SBCL runs the compositor loop, extension
runtime, window-management policy, and control server. A C library connects it
to wlroots 0.20 for Wayland protocols, rendering, outputs, and input devices.

Every desktop behaviour is a mountable extension. The shipped tiling, focus,
window-state, drag, and command units use the same API a user file does, and the
host has no special case for their names. Layer-shell panels, fullscreen and
maximize state, pointer-driven move and resize, and output configuration are all
visible to policy as context and owned effects.

This is a working prototype, not a complete desktop compositor or a pure-Lisp
Wayland implementation.

Two backends satisfy one contract, so the policy runtime does not know which one
is loaded. Features land in the wlroots backend; the pure-Lisp one is an
experiment and does not constrain them.

| Backend | Native side | State |
| --- | --- | --- |
| `native/` | wlroots 0.20 through a 29-line C header and a 1150-line C file | Packaged. xdg-shell, layer-shell, output configuration, pointer grabs. Verified with real clients, nested and headless |
| `backend/` | `libwayland-server` through `sb-alien`, no wlroots, no C of ours | Early. foot maps a window, the policy places it, `inspect` reports it |

The earlier Rust/Smithay and Lua implementation is not in this tree; it stays
reachable in the repository history (commit `6de3ba6` and earlier).

## Run

From the repository root:

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
| Super+f | Toggle fullscreen for the focused window |
| Super+m | Toggle maximize for the focused window |
| Super+q | Ask the focused client to close |
| Super+Shift+r | Reload configured extension files |
| Super+Shift+Escape | Quit |

Holding Super with the left mouse button moves the window under the pointer;
Super with the right button resizes it. Both drags end when the button is
released. The default layout tiles horizontally on the first output, inside the
area that layer-shell panels with an exclusive zone leave free. Outputs and all
window coordinates are available to Lisp, so a replacement can use the others.

## Output resolution and pixel mapping

Startup loads `$XDG_CONFIG_HOME/tomoe-lisp/init.lisp`, falling back to
`~/.config/tomoe-lisp/init.lisp`. `--config FILE` replaces that default file;
`--bare` skips it unless you also supply `--config`. An absent file is fine.
Output policy is an ordinary extension, with the same mount/reload/unmount
behavior as window policy:

```lisp
(define-extension "displays" () (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (configure-output "DP-4" :mode '(5120 1440)
                                        :scale 1 :position '(0 0))
                (configure-output "HDMI-A-2" :mode '(1920 1080 60)
                                             :scale 1 :position '(5120 0)))
          nil))
```

`configure-output` accepts:

- `:mode :preferred`, the monitor's advertised preferred mode, the default.
- `:mode :max`, the largest advertised pixel area at its highest refresh.
- `:mode '(WIDTH HEIGHT)`, that pixel resolution at its highest refresh.
- `:mode '(WIDTH HEIGHT HZ)`, the closest advertised refresh within 1 Hz.
  For example, 60 also matches 59.94 Hz. Unsupported modes are rejected, not
  silently replaced. Headless/nested outputs can accept custom dimensions.
- `:scale`, from 1/4 through 8, rounded to 1/120 increments. The default is 1.
- `:position '(X Y)`, in logical desktop coordinates. Omit it for automatic
  horizontal placement. Disconnected output names remain configured for hotplug.

Physical resolution counts monitor pixels. Window geometry, output positions,
and pointer coordinates use logical units, like Niri. At scale 1, one logical
unit is one physical pixel. At scale 2, a 3840x2160 output provides 1920x1080
logical units. Tomoe's physical-coordinate policy is different; it is not copied
into this logical-coordinate compositor.

Viewporter, fractional-scale-v1, and xdg-output let compatible clients render
buffers at the requested scale while keeping logical window sizes and input
coordinates consistent. Fractional scale does not guarantee every logical edge
falls on a physical pixel. Clients without fractional-scale support may render
at an integer scale and be resampled.

Later-mounted output policies win per output. Unmount restores the previous
owner or the output's initial mode, scale, and automatic placement. The backend
validates the full configuration before committing and attempts to restore the
previous hardware state if a commit fails. A failed hardware rollback stops the
compositor. Output changes then notify `:outputs` consumers to retile clients.
The native ABI is 4; the additive inspect fields keep control wire version 1.
The experimental pure-Lisp backend does not yet support output configuration.

## X11 clients

Xwayland is available in every instance, lazily: the X socket appears at startup,
the Xwayland process starts when the first X11 client connects, stops ten
seconds after the last one disconnects, and starts again on the next
connection. The X11 window manager is wlroots' own, so no separate process and
no extra program on `PATH` is involved; wlroots carries the absolute path to
the Xwayland binary it was built against.

`DISPLAY` is set for this process and its children only, never for systemd,
D-Bus, or the surrounding session. A terminal launched by a policy command gets
the right display; one started by hand does not. `--bare` still offers X11,
because this is mechanism, not policy.

To policy, an X11 window is an ordinary window: it appears in `:windows` with
the window's title and its `WM_CLASS` as `:app-id`, `place` sends a configure
the client may decline, `focus` sets the X input focus, `close-window` sends
`WM_DELETE_WINDOW`, and `fullscreen` and `maximize` set `_NET_WM_STATE`.
`:width` and `:height` are the dimensions the window had when it mapped.
Override-redirect windows — menus, tooltips, drop-downs — are the exception:
they keep their own coordinates, are never reported to policy, and sit above
windows and below the overlay layer. One that wants the keyboard, such as an X11
launcher, holds it until it unmaps, like an exclusive layer surface.

Limits: an X11 client owns its geometry, so an application that resizes itself
fights the tiling policy. There is no X11-specific effect, icon, or startup
notification, and an X11 window that dies while a configure is in flight leaves
a BadWindow line from wlroots' xcb error handler. Xwayland's own stderr (glamor
and xkbcomp messages) passes through to this compositor's stderr.

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
nix run . -- event '(:type :key :owner "commands" :command "terminal")'
nix run . -- quit
```

`inspect` prints versioned Lisp data containing live windows, outputs, resolved
geometry, focus, bindings, layer surfaces, extension state, dispatch counts, per
extension failures, and the last error. Mutating commands print nothing on
success and return a nonzero exit status on failure. `command OWNER NAME`
invokes an active binding through the same extension dispatch as keyboard input.

`event` sends one data property list to a live instance as an injected input
event: its `:type` must be `:key`, `:button`, or `:grab`. It exists so a policy
can be driven on a machine with no seat — a headless compositor, a test run, or
a scripted demonstration.

Mounting a file replaces that file's units without reloading other files.
Unmounting removes one named unit and its state. Reloading rebuilds all configured
files, including units previously unmounted. It preserves state for matching
names in the same source file. Unmount before reload to reset a unit's initial
state.

## Live reload

Configured sources are watched and reloaded when they change, four times a
second. A reload waits until a source has looked the same twice in a row, so a
half-written file is not loaded. The baseline is the content the runtime loaded,
not the first thing the watcher sees, so an edit made while a mount is still
settling is still caught. Success prints nothing: the new policy is visible in
`inspect` as a higher `:generation`. A source that fails to load keeps the
previous policy mounted and reports through the last error. `--no-watch` turns
watching off for one instance.

`examples/monocle.lisp` is an alternative layout that shows only the focused
window. Mount it after the default policy to override tiling. Removing it
restores the lower-priority layout without restarting clients. The other
examples are `workspaces.lisp` (nine tags), `float.lisp` (floating windows moved
and resized by a Super drag), and `layer-inset.lisp` (tiling that insets by
layer exclusive zones).

## Write an extension

Pass an ordinary Common Lisp file with `--config /absolute/path/config.lisp`,
or mount it through control. Use `--bare --config ...` to replace every shipped
policy. `builtins/desktop.lisp` declares `tiles`, `focus`, `window-state`,
`drag`, and `commands` through the same API as a user file. The host has no
special cases for these names.

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
(layer id &key layer exclusive-zone keyboard visible)
(fullscreen id flag)
(maximize id flag)
(grab id mode)                     ; :move or :resize
(launch "foot")                    ; argv, not a shell command string
(close-window id)
(quit)
(reload)
```

`:state` is an expression evaluated when the file is loaded, so an initial
property list is written `(list :tag 1)` or `'(:tag 1)`, not `(:tag 1)`.

Reducers receive a copied pre-dispatch snapshot, copied private state, and an
event property list. A property list alternates keys and values, such as
`(:id 1 :width 1280)`. Mutating a supplied list or string cannot mutate host
state or another reducer's snapshot. Return state as data, not closures or
native objects.

Declare every context key read with `:reads`. `context` rejects undeclared
reads. Available keys:

- `:windows`: property lists with `:id`, `:title`, `:app-id`, `:width`, `:height`,
  `:fullscreen`, and `:maximize`. Width and height are the client's initial
  mapped dimensions; the two flags are the state the client has acknowledged.
- `:outputs`: `:name`, logical `:x`, `:y`, `:width`, `:height`, plus
  `:physical-width`, `:physical-height`, `:refresh-mhz`, `:scale-120`, and the
  Wayland `:transform` enum. Divide `:scale-120` by 120 for the scale.
  `:modes` lists advertised pixel dimensions, refresh in mHz, and `:preferred`.
- `:output-config`: resolved requested settings, including disconnected names.
- `:layout`: resolved placement, with `:id`, coordinates, dimensions,
  `:visible`, `:fullscreen`, and `:maximize`.
- `:layers`: layer-shell surfaces, each a property list with `:id`, `:namespace`,
  `:layer` (`:background`, `:bottom`, `:top`, or `:overlay`), `:anchors`,
  `:exclusive-zone`, `:margin`, `:width`, `:height`, `:keyboard`, and `:visible`.
  The layer, exclusive zone, keyboard interactivity, and visibility shown are the
  resolved values, which are the client's request unless a unit overrides them.
- `:focus`: a window ID or `nil`.
- `:bindings`: resolved modifiers, keysyms, owner names, and command names.
- `:key`, `:button`, and `:grab`: event subscriptions, not stored context values.

Layer surfaces are arranged by the compositor from the client's own anchors,
margins, and exclusive zone, and they are never in `:windows`, so a tiling policy
does not have to know about them. A layer surface that asks for exclusive
keyboard interactivity takes the keyboard while it is mapped, which is what a
launcher needs; hiding it gives the keyboard back to the focused window.

Events include `:map`, `:unmap`, `:metadata`, `:outputs`, `:layer`, `:key`,
`:button`, and `:grab` in their `:type` field. Key events carry `:owner` and a
lowercase `:command` string. Button events carry `:id` (zero for empty space, a
layer surface id when a panel was hit), an evdev `:button` code, `:state`
(`:pressed` or `:released`), pointer `:x` and `:y` in logical coordinates, and
the keyboard `:modifiers` mask. `:metadata` events carry the same fields as
`:map`, plus `:request` (`:fullscreen` or `:maximize`) when a client asked for a
state, which is the policy's chance to accept or ignore it. While a unit owns a
grab, pointer motion arrives as `:grab` events with `:id`, `:mode`, `:x`, `:y`,
and the delta since the previous event. Lifecycle evaluation receives `:mount`;
reactive reevaluation receives `:change` with the changed `:keys`.

Owned effects are `place`, `focus`, `bind-key`, `configure-output`, `layer`,
`fullscreen`, `maximize`, and `grab`. Return the complete desired set each time.
Later-mounted units win conflicts. Omitting an effect removes that unit's
contribution. `place` uses integer logical coordinates and positive sizes up to
16384. Bindings accept `:super`, `:alt`, `:control`, and `:shift`, plus an XKB
keysym name such as `Return` or `Tab`.

`layer` overrides a layer surface without taking over its geometry: `:layer`
reassigns it (`nil` keeps the client's request), `:exclusive-zone` changes how
much of the output it reserves, `:keyboard` accepts `:none`, `:exclusive`, or
`:on-demand`, and `:visible nil` hides it. `fullscreen` and `maximize` set the
client-visible state; a policy that sets fullscreen usually also owns that
window's `place` so the window fills the output. `grab` claims the pointer for a
window: while a unit owns one, motion and buttons are not delivered to clients,
and the owning unit is responsible for dropping the grab — normally on the
`:button` release event. The compositor also clears a grab whose window or layer
surface disappears.

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
cycles retain the previous managed policy. A failure is attributed to the unit
that caused it: `inspect` reports that unit's `:failures` count and last error
next to the runtime-wide one, and the whole transaction is still discarded.
Errors appear on stderr and through `inspect`. Native allocation failure during
commit stops the compositor rather than pretending it rolled back. One-shot
command failures cannot undo earlier commands.

Unmount reconstructs geometry, visibility, stacking, focus, bindings, output
configuration, layer overrides, window state, and the grab from remaining
owners. The preserved facts are live client identities and metadata, initial
client sizes, map order, output descriptions, and the layer surfaces the
clients themselves described. Unowned windows revert to their mapped dimensions
at the origin, an unowned layer surface to its client's request, and an unowned
grab to none. A client's destruction is an external fact, so failure recovery
also removes dead IDs from the previous policy.

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
`(1 :unmount "name")`, `(1 :command "owner" "command")`, `(1 :event "PLIST")`,
or `(1 :quit)`. `:event` carries one string holding a data property list whose
`:type` must be `:key`, `:button`, or `:grab`; the compositor reads it and
dispatches it like a real input event. Replies are `(1 :ok result)` or
`(1 :error "message")`. Version 1 is exact; unknown versions fail explicitly.
Reader evaluation and dispatch syntax such as `#.` and circular object labels
are disabled, and the reader accepts exactly the data the printer emits,
including the cons dot an extension's own state may contain. This is a data
protocol, not an unauthenticated REPL.

## Verification

`nix flake check` builds the package and runs one end-to-end check, which is the
whole automated suite by project decision.

`tests/run-integration.sh` builds a small Wayland test client from `client.c`
and protocol code generated at build time, starts the packaged compositor on the
headless backend with `--bare --no-watch`, and drives it through its own control
client. It asserts that no policy is mounted for `--bare`, that the fixtures
mount cleanly, that two xdg clients map at the sizes they asked for and are
placed inside the output by the fixture layout, that a layer client keeps its
namespace, anchors, and height, that the fixture's layer override resolves and
returns to the client's request when an injected key command arrives, that
extension state containing a cons survives the control round trip, that
unmounting the layout returns both windows to their mapped size at the origin,
and that `quit` exits zero and removes both sockets.

From the working tree:

```sh
nix develop -c ./tests/run-integration.sh
```

Observed on x86_64 Linux, in addition to the check above:

- `nix build` and `nix flake check` completed successfully. The aarch64 package
  was evaluated, not built.
- Real Foot clients mapped, tiled inside a layer panel's exclusive zone, took
  focus, and were restored to their mapped dimensions when the layout unit was
  unmounted.
- An owned fullscreen effect sized a real client to the output and reverting it
  restored the mapped size, with no native error.
- A policy-owned grab started from an injected key, moved a window by the
  reported delta, and released on the injected button release.
- Editing a mounted source reloaded it without a command, and making a source
  unreadable kept the previous policy and reported once.
- The pure-Lisp backend still loads and answers `inspect` with the shipped
  policy mounted.

- A real X11 client (xeyes) mapped through Xwayland as an ordinary window,
  carrying `WM_CLASS` as its app id, and was tiled; `xwininfo` confirmed the
  X server had applied the configure. `command commands close` made it exit
  through `WM_DELETE_WINDOW`. Xwayland stopped when the last X client left and
  restarted on the next connection, giving the window a new id. A probe window
  with `override_redirect` mapped and unmapped without ever appearing in
  `inspect`, and a child launched by policy reported `DISPLAY` and
  `WAYLAND_DISPLAY` of this instance. `quit` exited zero and removed the X
  socket along with the control and Wayland ones.

Hardware DRM, physical input (a real pointer grab, a real keyboard), failure
recovery for native allocation, and output rotation, mirroring, and VRR remain
unverified. X11 physical input, X11 windows on hardware, and the placement of an
X11 application that resizes itself are unverified too; Xwayland never received
a real key or button in these sessions.

## Source and limits

- `src/`: Common Lisp API, state/effect runtime, native bindings, control, CLI.
- `native/backend.h`: the native ABI. No pointers cross into extension snapshots.
- `native/backend.c`: wlroots lifetimes, scene layering, xdg-shell, layer-shell,
  input routing, pointer grabs.
- `builtins/desktop.lisp`: replaceable default policy.
- `examples/`: alternative policies, each mountable on its own.
- `tests/`: the end-to-end check, its fixtures, and its Wayland client.
- `flake.nix`, `build.lisp`: native compilation and saved SBCL executable.
- `DESKTOP.md`, `FINIX.md`, `LISP-BACKEND.md`, `OUTPUTS.md`, `STARTUP.md`, and
  `WORK.md` are historical checkpoints from the prototype work. They name local
  paths and predate the move to the repository root.

Implemented protocols cover ordinary xdg-shell windows and popups, shared-memory
buffers, subsurfaces, clipboard selection, viewporter, fractional-scale-v1,
xdg-output, and layer-shell, and X11 clients through wlroots' Xwayland and its
XWM. Missing desktop features include session
locking, screencopy, portals, input methods, touch and tablets, output rotation,
mirroring, and VRR configuration, primary selection, and window decorations.
Popup placement does not constrain menus to output bounds. A policy that never
releases a grab keeps the pointer until the grabbed surface disappears. A layer
surface's anchors, margins, and size stay the client's request: policy can move
it between layers, change its exclusive zone and keyboard interactivity, and
hide it, but not place it freely. Do not use this as a secure daily desktop.

Tomoe and ShojiWM informed the separation of mechanism from policy and explicit
ownership of reactive effects. Local reference clones live in `ref/`, which
is listed in the workspace `.gitignore`. The wlroots tinywl example and 0.20
headers informed native API use. This implementation does not copy either
reference compositor's Rust code.
