# Tomoe

A new Common Lisp Wayland compositor. SBCL runs the compositor loop, extension
runtime, window-management policy, and control server. A C library serves the
Wayland protocols on libwayland-server and drives rendering (EGL/GLES2 on GBM),
outputs (DRM/KMS on libseat, nested Wayland, headless), and input (libinput).

Every desktop behaviour is a mountable extension. The shipped window manager,
drag, and command units use the same API a user file does, and the
host has no special case for their names. Layer-shell panels, fullscreen and
maximize state, pointer-driven move and resize, and output configuration are all
visible to policy as context and owned effects.

The native side sits behind the ABI header `native/backend.h`, split into C
modules by concern.

The earlier Rust/Smithay and Lua implementation is not in this tree; it stays
reachable in the repository history (commit `6de3ba6` and earlier).
[PARITY.md](PARITY.md) tracks the remaining behavior and verification needed to
reach that version's feature coverage and composability requirements.

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
`tomoe-0` under `XDG_RUNTIME_DIR`. It does not change your systemd, D-Bus,
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

The package includes Foot as its terminal and Fuzzel as its launcher. Shipped bindings use Super:

| Binding | Command |
| --- | --- |
| Super+Return | Open a terminal |
| Super+d | Open Fuzzel |
| Super+j or Super+Tab | Focus the next window |
| Super+k | Focus the previous window |
| Super+f | Toggle fullscreen for the focused window |
| Super+1 through Super+9 | Switch workspace |
| Super+Shift+1 through Super+Shift+9 | Move the focused window to a workspace |
| Super+q | Ask the focused client to close |
| Super+Shift+r | Reload configured extension files |
| Super+Shift+e or Super+Shift+Escape | Quit |

Holding Super with the left mouse button moves the window under the pointer;
Super with the right button resizes it. Both drags end when the button is
released. The default WM has nine workspaces and an eight-pixel gap. It tiles
the active workspace inside the first output's usable area, repeatedly splitting
the longer remaining side in half (dwindle). Workspace lists retain insertion
order; switching selects the destination's last window. Outputs and all window
coordinates are available to Lisp, so a replacement can use the others.

The WM consumes `:workspace`, `:fullscreen`, and `:focus` rule properties when a
window first maps. An inactive destination stays hidden; its fullscreen and
focus properties are ignored. An active fullscreen rule focuses the window even
when `:focus` is explicitly `nil`, matching the former Rust WM. Later metadata
changes do not repeat admission. An xdg buffer detach retains the admitted
window, workspace and rule state; remapping the same role does not readmit it.
Role destruction ends that lifetime. Reload preserves workspace order and state.

Client fullscreen requests are denied by default; unfullscreen requests are
always honored. Maximize requests are acknowledged while retaining tiled
geometry. Configure these defaults with an ordinary owner:

```lisp
(define-extension "desktop-settings" (:reads ()) (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (publish-state :wm-settings
                               '(:gaps 8 :workspace-count 9 :honor-client-fullscreen t)))
          nil))
```

The WM publishes `:wm-state` in `:data`, with `:active` and a `:workspaces` list
of `(:id N :windows COUNT)` records. This is available through snapshots and
`inspect`, and as the JSON `wm_state` method/event. The shell service facade
remains absent.

## Output resolution and pixel mapping

Startup loads `$XDG_CONFIG_HOME/tomoe/init.lisp`, falling back to
`~/.config/tomoe/init.lisp`. `--config FILE` replaces that default file;
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
  For example, 60 also matches 59.94 Hz. Unavailable advertised modes fall back
  to the preferred mode, or the first advertised mode when none is preferred,
  matching Rust. Headless/nested outputs can accept custom dimensions.
- `:scale`, from 1/4 through 8, rounded to 1/120 increments. The default is 1.
- `:position '(X Y)`, in physical screen pixels. Omit it for automatic
  horizontal placement. Disconnected output names remain configured for hotplug.
- `:disabled t`, to turn off a connected output and withdraw its Wayland global.
  It stays in `:connectors` so a policy can discover and re-enable it.
- `:mirror "OUTPUT"`, to use an active, non-mirroring output's physical origin.
  This overrides `:position` and consumes no additional automatic layout space.
  Missing, disabled, self, or mirroring targets fall back to automatic placement
  after ordinary outputs.
- `:vrr t`, to request adaptive sync when the backend advertises support.
  Adaptive sync stays off on unsupported outputs; a supported backend's rejection rolls back
  the transaction. Hardware VRR operation remains unverified.

These fields belong to the same output declaration. A later owner replaces the
whole declaration; removal restores the preceding owner or native baseline.
Disabling every output is supported. Output and connector consumers see the
prospective topology before commit, and failed transactions retain accepted
policy, shell resources and Wayland output identities.
Startup and hotplug policies settle before an output's first enable commit or
Wayland global advertisement. A predeclared disable rule keeps the connector
inactive throughout admission; the first enabled frame includes its settled
shell. Connectors expose a lifetime `:id`, advertised `:modes`, and `:pending`
admission status. A backend admission failure keeps the arriving connector
inactive and records its name and message in `:output-errors`, while preserving
the requested declaration and allowing other outputs and owners to settle.
Changing or removing the declaration, or reconnecting the port, permits recovery.

Backend mode, scale and transform requests also settle through the complete
presentation transaction. Multiple requests received before settlement coalesce;
accepted output geometry remains unchanged until publication. Connector
`:request-id` and `:request-pending` fields identify these proposals independently
of `:outputs`. Existing declarations retain precedence, while successful requests
update the underlying backend settings restored on owner removal. Requests only
change the fields they supply. Reducer or backend rejection retains the accepted
geometry and baseline, reports `:output-errors`, and allows a later request or
declaration to recover.

Window placement uses integer physical world pixels, as in the Rust version.
Output positions and layer geometry use physical screen pixels. An output at
3840x2160 still occupies that many pixels at scale 2; its protocol description
and client configure sizes use logical units at the conversion boundary.
Requested window sizes round to integer logical sizes, then back to achievable
physical sizes in `:layout`. Exact halves round away from zero. Positions are
never rounded through a logical scene position.

`:window-geometry` reports committed client sizes at the candidate output scale
and world location. A client can acknowledge a configure later or choose another
size, so this rectangle can differ from `:layout`. Geometry readers see policy
placement, visibility, camera and output changes during settlement; client size
changes arrive when the surface commits. Size-only commits leave `:windows`
readers idle.

Viewporter, fractional-scale-v1, and xdg-output let compatible clients render
buffers at the requested scale while keeping logical window sizes and input
coordinates consistent. The renderer projects physical destination edges, and
hit testing inverts those same rectangles, including rounded edges. Clients without fractional-scale support may render
at an integer scale and be resampled.

The pointer keeps a physical screen position independently of the logical output
layout. Each output's cursor is positioned from that value, so overlapping
logical rectangles at different scales cannot redirect input or duplicate the
cursor. Camera and scene changes update pointer focus even without mouse motion.
Relative device deltas use logical compositor units and convert at the starting
output's scale, matching Rust; crossing an output changes the next event's scale.
The wlr virtual-pointer protocol follows the same seat path as backend devices;
absolute virtual pointers can select an output, and nested backend pointers use
their named output.

Later-mounted output policies win per output. Unmount restores the previous
owner or the output's initial mode, scale, and automatic placement. The backend
validates the full configuration before committing and attempts to restore the
previous hardware state if a commit fails. A failed hardware rollback stops the
compositor. During policy settlement, `:outputs` consumers see the candidate
mode, scale, and physical layout before native commit. Their placement sizes
are quantized against those candidate scales. Matching native confirmations
do not rerun consumers; output revisions discard older queued confirmations
after a newer commit. External output changes still notify consumers.
Layer geometry and usable output areas resolve in the same dependency rounds.
`output-power` turns a display off without leaving the layout: the CRTC shuts
down so the monitor sleeps, the output stops rendering and sending frame
callbacks, and windows keep their places. Power on renders a fresh frame through
a full modeset. Connectors report `:power`, and wlr-output-power-management
clients such as `wlopm` drive the same state. A session lock does not wait on a
dark output.
The native ABI is 30; the additive inspect fields keep control wire version 1.

## X11 clients

X11 clients run through xwayland-satellite, which presents each X11 window to
Tomoe as an ordinary xdg toplevel. At startup the host picks the first display
whose `/tmp/.X<n>-lock` file is absent or names a dead process and exports it
as `DISPLAY` to this process and its children, never to systemd, D-Bus or the
surrounding session. The shipped `xwayland` extension runs `tomoe-xwayland` on
that display as a `service`, restarting it when it exits: it takes the lock,
listens on the display's sockets, and on the first X11 connection becomes
`xwayland-satellite -listenfd`, so Xwayland only starts once an X11 client
connects. `--bare` has no X11 until a policy declares that service. The
package puts `tomoe-xwayland` and `xwayland-satellite` on the wrapper's
`PATH`.

To policy, an X11 window is an xdg window: its title and app id come from
satellite, `place` sends a configure, and fullscreen and maximize go through
xdg state. Menus, tooltips and other override-redirect windows are satellite's
popups and subsurfaces. Satellite's own stderr passes through to this
compositor's stderr.

## Live control

These commands attach to a running instance and then exit:

```sh
nix run . -- inspect
nix run . -- command commands terminal
nix run . -- command wm next
nix run . -- unmount wm
nix run . -- mount "$PWD/examples/monocle.lisp"
nix run . -- unmount monocle
nix run . -- reload
nix run . -- event '(:type :key :owner "commands" :command "terminal")'
nix run . -- quit
```

`inspect` prints versioned Lisp data containing live windows, outputs, resolved
geometry, focus, bindings, layer surfaces, extension state, dispatch counts, per
extension failures, and the last error. Its `:x-display` field is the `DISPLAY` this
instance exported for X11 clients. Mutating commands print nothing on
success and return a nonzero exit status on failure. `command OWNER NAME`
invokes an active binding through the same extension dispatch as keyboard input.

`event` sends one data property list to a live instance as an injected input
event: its `:type` must be `:key`, `:button`, or `:grab`. It exists so a policy
can be driven on a machine with no seat — a headless compositor, a test run, or
a scripted demonstration.

Mounting a file replaces that file's units without reloading other files.
Replacing or watching an existing source preserves its precedence; only a new
source is appended. Saving an earlier source cannot promote it over later ones.
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
source auto-reload off for one instance; owned `watch-file` effects still run.

`examples/monocle.lisp` is an alternative layout that shows only the focused
window. Mount it after the default policy to override tiling. Removing it
restores the lower-priority layout without restarting clients. The other
examples are `workspaces.lisp` (nine tags), `float.lisp` (floating windows moved
and resized by a Super drag), `layer-inset.lisp` (tiling that insets by
layer exclusive zones), and `zoomer.lisp` (a floating canvas with planes, where
Mod+drag moves, resizes or pans, Mod+scroll zooms around the cursor, Mod+Tab and
Mod+1..9 switch planes, and Mod+f fits a window to the view; publish
`:zoomer-settings` to change its step sizes). `special.lisp` adds scratchpad
workspaces over the default tiling: Mod+Shift+grave parks the focused window,
Mod+grave shows or hides it centered at three quarters of the work area, and a
window rule property `:special "name"` parks a window on open. Parked windows
are published as `:wm-exclude`, which the default `wm` leaves out of tiling.

## Write an extension

Pass an ordinary Common Lisp file with `--config /absolute/path/config.lisp`,
or mount it through control. Use `--bare --config ...` to replace every shipped
policy. `builtins/desktop.lisp` declares `wm`, `drag`, and `commands` through the
same API as a user file. The host has no
special cases for these names.

The public API is in `src/api.lisp`:

```lisp
(define-extension "name" (:reads (:windows :outputs) :state nil)
    (snapshot state event)
  ;; Return new state, the complete set of owned effects, then one-shot commands.
  (values state nil nil))

(context snapshot :windows)
(previous-context snapshot :focus) ; context before this transaction; requires :FOCUS
(publish-state :status '(:ready t))
(state-value snapshot :status)     ; requires :DATA; optional default for absent names
(place id x y width height visible)
(focus id &key (raise t))           ; NIL clears focus; :RAISE NIL leaves ordering to other effects
(raise-window id)
(show-window id)
(hide-window id)
(bind-key '(:super :shift) "r" :reload)
(configure-keyboard :layout "us" :options nil :repeat-rate 25 :repeat-delay 600)
(layer id &key layer exclusive-zone keyboard visible)
(fullscreen id flag)
(maximize id flag)
(grab id mode)                     ; :move or :resize
(launch "foot")                    ; argv, not a shell command string
(spawn '("./worker" "--foreground") :cwd "bin" :env '(("MODE" . "desktop")))
(close-window id)
(output-power :toggle)             ; :on, :off or :toggle; optional connector name
(quit)
(reload)
```

`:state` is an expression evaluated when the file is loaded, so an initial
property list is written `(list :tag 1)` or `'(:tag 1)`, not `(:tag 1)`.

Reducers receive a copied candidate snapshot, copied private state, and an
event property list. A property list alternates keys and values, such as
`(:id 1 :width 1280)`. Mutating a supplied list or string cannot mutate host
state or another reducer's snapshot. Return state as data, not closures or
native objects.

`:admission t` opts a reducer into recalculation after ordinary reducers and
rule applications. Every dependency round starts from that transaction's
original private state and event; it replaces the previous proposal's state,
effects, and commands. Descendant rule scopes inherit this recalculation: a new
child receives `:mount` against its final declaration's initial state, and an
existing child uses its transaction-start state and first delivered event.
Only the final proposal commits. This lets admission
consume rule properties that are refined during settlement without permanently
remembering an early assignment. `previous-context` uses the same declared reads
as `context` and exposes the stable context from before the transaction. Use it
when interpreting input against an existing focus or rectangle, so recalculation
does not apply a toggle or relative action to its own earlier result. Cycles
still reject the candidate after sixteen rounds.

Declare every context key read with `:reads`. `context` rejects undeclared
reads. Available keys:

- `:windows`: property lists with `:id`, `:title`, `:app-id`, `:width`, `:height`,
  `:fullscreen`, and `:maximize`. Width and height are the client's initial
  mapped physical dimensions; these two flags are acknowledged state.
  `:buffered` says whether the admitted window currently has client content.
  An xdg window retains its row and initial dimensions through buffer detach.
  `:buffer-generation` identifies the current buffer lifetime; it advances on
  admission or remap, including when policy rejects the accompanying event.
  Resizes and metadata updates keep the same generation.
  `:fullscreen-requested` and `:maximize-requested` preserve client intent,
  including requests before the first map. `:output` names a requested xdg
  fullscreen output when one was supplied. Fullscreen/maximize metadata requests
  include `:requested t/nil` for their explicit direction.
- `:window-geometry`: visible window rectangles with `:id`, physical world `:x`,
  `:y`, `:width`, and `:height`. `(window-geometry snapshot ID)` returns a copied
  rectangle without its ID, or `nil` for a hidden/unknown window; it also accepts
  a window record. Declare `:window-geometry` to use this helper. Size comes from
  the latest committed client geometry, independently of requested dimensions.
  Size-only commits deliver `(:type :geometry :id ID :width W :height H)` with
  logical dimensions; use the snapshot for their resolved physical projection.
- `:data`: an alist of named values from `publish-state`. Each keyword has one
  resolved owner; later declarations replace its entire value, including `nil`.
  Omission/unmount restores the preceding contribution. `state-value` returns
  copied bounded data and distinguishes an absent name from a published `nil`.
- `:services`: session producer facts, independent of mounted effects.
  `service-state` returns a copied named service snapshot; notifications,
  MPRIS, battery, network and tray expose the records described below.
  As in Rust, `:audio` supplies static `(:volume 1.0d0 :muted nil)` defaults and
  `:sysinfo` supplies static `(:cpu-percent 0 :memory-percent 0)` defaults;
  these records have no live hardware producer or control actions.
- `:outputs`: active displays with `:name`, physical screen `:x`, `:y`, `:width`, `:height`, plus
  `:physical-width`, `:physical-height`, `:refresh-mhz`, `:scale-120`, and the
  Wayland `:transform` enum. Divide `:scale-120` by 120 for the scale.
  `:modes` lists advertised pixel dimensions, refresh in mHz, and `:preferred`.
  `:adaptive-sync-supported` reports backend capability and `:adaptive-sync`
  reports the accepted state.
- `:connectors`: every connected port, including disabled ones, with `:name`,
  `:enabled`, `:adaptive-sync-supported`, and `:adaptive-sync`. These facts
  participate in prospective settlement and selective dependency updates.
  `:render` describes the output buffer ring: `:format`, the allocated
  `:modifier`, `:implicit` when explicit modifiers failed the backend test,
  `:width`, `:height`, and `:fenced` when each frame's page flip waits on a
  render-done fence rather than a CPU wait. It is `nil` for a disabled output.
- `:output-config`: resolved requested settings, including disconnected names.
- `:workareas`: one physical usable rectangle per output, with `:name`, `:x`,
  `:y`, `:width`, and `:height`. The native layer planner applies visible
  reservations on their own outputs before rounding the resulting boundaries.
  Use these rectangles for tiling instead of adding rounded zones and margins.
- `:view`: the resolved camera, `(:x X :y Y :zoom ZOOM)`.
- `:layout`: resolved requested placement, with `:id`, coordinates, quantized
  dimensions, `:visible`, `:fullscreen`, and `:maximize`.
- `:stacking`: visible managed window IDs in bottom-to-top order. Fullscreen
  windows share this stack; top and overlay layer surfaces remain above it.
- `:rules`: one `(:id ID :properties ALIST)` record per known window, containing
  the merged properties of its matching rules. `(rules-for snapshot ID)` returns
  a copy of that alist; it also accepts a window record in place of the ID.
- `:layers`: layer-shell surfaces, each a property list with `:id`, `:namespace`,
  `:layer` (`:background`, `:bottom`, `:top`, or `:overlay`), `:anchors`,
  `:exclusive-zone`, `:margin`, `:width`, `:height`, `:keyboard`, `:visible`,
  `:output`, `:scale-120`, `:x`, `:y`, and `:exclusive-edge`. Positions and
  dimensions describe the planned configure geometry, which can precede the
  client's buffer acknowledgment. Layer rendering uses the same output grid,
  so rounding margins and sizes separately cannot accumulate pixel errors.
  Dimensions, margins, and zones are physical pixels;
  an owned zone reports its achievable size after protocol quantization.
  The layer, exclusive zone, keyboard interactivity, and visibility shown are the
  resolved values, which are the client's request unless a unit overrides them.
- `:focus`: a window ID or `nil`.
- `:surfaces`: compositor shell rectangles with `:owner`, `:source-id`, `:name`,
  `:output`, `:x`, `:y`, `:width`, `:height`, and `:layer`. Geometry is physical.
- `:bindings`: resolved modifiers, keysyms, owner names, and command names.
- `:keyboard`: the resolved whole-seat XKB policy, with `:rules`, `:model`,
  `:layout`, `:variant`, `:options`, `:repeat-rate`, and `:repeat-delay`.
  Empty strings for `:rules`, `:model`, `:layout`, and `:variant` request the
  XKB defaults. `:options nil` uses `XKB_DEFAULT_OPTIONS`; `:options ""`
  explicitly disables those defaults. A later owner replaces the complete
  policy, and removing it restores the preceding owner or the session default.
- `:key`, `:button`, `:grab`, `:request`, and `:ui`: event subscriptions, not stored context values.

Layer surfaces are arranged by the compositor from the client's own anchors,
margins, and exclusive zone, and they are never in `:windows`, so a tiling policy
does not have to know about them. A layer surface that asks for exclusive
keyboard interactivity takes the keyboard while it is mapped, which is what a
launcher needs; hiding it gives the keyboard back to the focused window.

Events include `:map`, `:buffer`, `:unmap`, `:metadata`, `:geometry`, `:outputs`, `:layer`, `:key`,
`:button`, `:grab`, and `:request` in their `:type` field. Key events carry `:owner` and a
lowercase `:command` string. Button events carry `:id` (zero for empty space, a
layer surface id when a panel was hit), an evdev `:button` code, `:state`
(`:pressed` or `:released`), pointer `:x` and `:y` in physical world coordinates
for windows or physical screen coordinates for layers and empty space, and
the keyboard `:modifiers` mask. `:metadata` events carry the same fields as
`:map`, plus `:request` (`:fullscreen` or `:maximize`) when a client asked for a
state, which is the policy's chance to accept or ignore it. While a unit owns a
grab, pointer motion arrives as `:grab` events with `:id`, `:mode`, `:x`, `:y`,
and the delta since the previous event. Lifecycle evaluation receives `:mount`;
reactive reevaluation receives `:change` with the changed `:keys`. An owned timer
delivers `(:type :timer :name NAME)` to its declaring reducer; other reducers
receive only the resulting context changes.

Native xdg activation delivers `(:type :request :id ID :request :activate)` or
`:urgent`. These requests leave window facts unchanged and are delivered once.
The shipped WM accepts `:activate` by revealing the target's workspace and owning focus;
`:urgent` leaves focus alone. A replacement focus policy can ignore either
request or choose another target. Unmounting focus policy removes this response.
Failed reducers and rejected native publication retain the accepted focus and
do not replay the request on a later transaction.

Owned effects are `publish-state`, `place`, `focus`, `raise-window`, `show-window`, `hide-window`,
`bind-key`, `configure-output`, `layer`,
`fullscreen`, `maximize`, `grab`, `set-view`, `once`, `interval`, and
`exec-async`, `run-once`, `service`, and `window-rule`.
Return the complete desired set each time.
Later-mounted units win conflicts. Omitting an effect removes that unit's
contribution. `place` uses integer physical world coordinates and positive requested sizes up to
16384. Bindings accept `:super`, `:alt`, `:control`, and `:shift`, plus an XKB
keysym name such as `Return` or `Tab`. Native matching uses the active XKB
layout's base level, so Shift+1 is a `:shift` binding on `"1"`.

Geometry, visibility, focus, and stacking can have independent owners.
`show-window` and `hide-window` change visibility without replacing another
owner's placement; omitting them restores the preceding visibility contribution.
Hidden windows remain in `:windows` and `:layout`, and their rule instances and
resources remain alive. An xdg buffer detach also retains these owners, while
its visible committed rectangle becomes 0×0. `:buffer` reports `:attached t/nil`
and committed logical `:width`/`:height`; it updates `:windows` and
`:window-geometry`. `:map` admits an xdg window once, and `:unmap` withdraws it
when the role is destroyed. Layer unmap still withdraws its row.
Visibility and keyboard focus are independent: hiding a focused window retains
its focus, and a hidden live window can be focused without showing it. A focus
policy can explicitly clear or transfer focus. Buffer detach clears actual seat
focus while retaining its policy target; remap restores that target when no
exclusive layer or unmanaged window takes precedence.

`raise-window` moves a visible window to the top of the managed stack without
changing geometry or focus. `focus` also contributes a raise by default;
`:raise nil` contributes only focus. Effects resolve in mount/declaration order,
so a later focus or raise can supersede earlier stacking contributions.
Moving an already visible window leaves its rank alone. Showing a window hidden
by an earlier contribution appends it to the stack. Missing and hidden raise
targets have no effect. Unmount reconstructs the preceding owners' order,
falling back to map order. The resolved stack drives both candidate rendering
and live input, including publication during an output configuration change.

Ordinary client key events combine all physical keyboards: the first unconsumed
press of a keycode sends a down, and its last physical release sends an up.
Focus-enter carries the combined held-key set without duplicates or consumed
shortcuts, including when an already-focused client obtains a new keyboard
resource. Removing a device preserves keys held by surviving devices. Duplicate downs and unmatched ups
are ignored. A key's client/binding routing stays fixed until its physical
release, including when a new binding is installed while it is held. One logical
seat keyboard owns modifiers, locks, and the active layout group: Shift on one
device affects bindings on another, and Caps Lock or layout changes survive
device changes. Source-local modifier reports cannot clear another device's
held modifiers. Tomoe's own seat tracks all 768 evdev key codes.

`bind-key` owns one physical shortcut for its extension. Its form is
`(bind-key MODIFIERS KEYSYM PRESS &key release)`, where `PRESS` and the optional
`release` value are keyword command names. A native press event has
`:state :pressed`; a latched physical key-up invokes the release command with
`:state :released`, even if modifiers or the active layout changed. Native key
events also carry numeric `:device` and `:keycode` fields identifying the
physical lease. Numeric `:source-id` and `:binding-id` fields are opaque native
metadata used to reject queued events from a removed or replaced binding; an
extension should not persist or interpret them as public identities. Once a
binding lease consumes a key-down, its key-up is swallowed even when its owner
is removed and the release callback is canceled. Repeated presses while that
lease is held are ignored. The native lease is per device, rather than the
Rust baseline's global raw-keycode latch.

An unchanged equal binding in the same source generation retains its native
lease across unrelated transactions. Removing and re-adding it, changing
either command, or replacing the source cancels an existing release callback,
even if the new declaration is textually identical. A failed reload keeps the
previous binding and its leases. The `command OWNER NAME` control operation
first invokes a matching press command, or invokes a matching release command
when no press has that name; the synthetic release event has `:state :released`
but no `:device` or `:keycode`.

Reducers that track held keys should read `:bindings` and clear transient state
when another owner supersedes their shortcut. Clear it on `:mount` too, because
matching reducer state survives a successful source reload. This single-shortcut
example uses both lifetime signals:

```lisp
(define-extension "held" (:reads (:key :bindings) :state (list :keys nil))
    (snapshot state event)
  (cond
    ((or (eq (getf event :type) :mount)
         (not (find "held" (context snapshot :bindings)
                    :test #'equal :key (lambda (binding) (getf binding :owner)))))
     (setf (getf state :keys) nil))
    ((and (eq (getf event :type) :key)
          (equal (getf event :owner) "held"))
     (let ((key (list (getf event :device) (getf event :keycode))))
       (if (eq (getf event :state) :pressed)
           (pushnew key (getf state :keys) :test #'equal)
           (setf (getf state :keys)
                 (delete key (getf state :keys) :test #'equal))))))
  (values state (list (bind-key nil "F1" :press :release :release)) nil))
```

For multiple shortcuts, track ownership per declaration, using distinct command
names or comparing the resolved modifier/keysym/command entries. Another binding
from the same owner can remain active while a held shortcut is superseded.

`configure-keyboard` owns the seat's XKB keymap and repeat policy, including
keyboards added after the policy is accepted. It is one whole-policy effect:
the latest owner supplies every field rather than overriding individual fields.
`:rules`, `:model`, `:layout`, and `:variant` are strings whose empty value uses
the XKB default; `:options` is either `nil`, which uses `XKB_DEFAULT_OPTIONS`,
or a string, where `""` explicitly disables default options. Each supplied
string may contain at most 1024 characters and no NUL. `:repeat-rate` accepts
0 through 2147483647 (zero disables repeat), and `:repeat-delay` accepts 1
through 2147483647 milliseconds.

An invalid XKB candidate rejects the complete native policy transaction. The
previous accepted keymap and repeat settings remain active, with no partial
device or logical seat update. Equal maps retain held modifiers, lock state,
and the active layout group across repeat-only updates. A different keymap rebuilds state from
physically held keys and resets prior latched/locked modifiers and layout group.

`(set-view X Y ZOOM)` owns the canvas camera: screen positions are
`(world - offset) * zoom`. Zoom defaults to 1, accepts finite real numbers, and
clamps to 1/16 through 16. Windows, their popups, and subsurfaces follow the
camera; layers, outputs, and the cursor remain fixed on screen. Removing the
last view owner restores `(:x 0 :y 0 :zoom 1d0)`. The read-only control query
`(1 :hit-test X Y)` or CLI `hit-test X Y` accepts physical screen coordinates
and reports the hit ID plus screen, world, and client-local surface coordinates.
Its hit path is also used by native pointer input.

Each output frame redraws only what changed since the output buffer it reuses
last held. The frame is first walked without drawing, recording every draw
with a hash of its parameters. Draws that appear, disappear, move, change
parameters or change stacking order damage their boxes; a client commit damages
only its declared buffer damage. Blur that overlaps damage widens it to the
blur's sampled area. The union of damage over the buffer's age clips the real
pass. Captures, configuration previews, and buffers older than eight frames
draw in full. `(1 :frames)` or CLI `frames` reports each output's frame
counter, last drawn frame, buffer age, drawn and total pixels, draw count, and
direct scanout state. `TOMOE_DEBUG_DAMAGE=1` draws every frame in full and
tints the damage the frame computed.

`layer` overrides a layer surface without taking over its geometry: `:layer`
reassigns it, `:exclusive-zone` changes how
much of the output it reserves, `:keyboard` accepts `:none`, `:exclusive`, or
`:on-demand`, and `:visible nil` hides it and releases its reserved space.
Omitted or `nil` layer, zone, and keyboard fields inherit earlier owners, then
the client's live request. Removing the final owner releases the native
override, so later client changes still take effect. `fullscreen` and `maximize` set the
client-visible state; a policy that sets fullscreen usually also owns that
window's `place` so the window fills the output. `grab` claims the pointer for a
buffered window or mapped layer surface: while a unit owns one, motion and buttons are not delivered to clients,
and the owning unit is responsible for dropping the grab — normally on the
`:button` release event. Unmap or destruction clears native capture immediately.
Starting a grab balances held client buttons and clears pointer focus; ending
it restores focus and the client cursor even when the pointer stays still.
The latest owner with a mapped target wins; a stale target cannot mask a live
earlier owner. A retained contribution becomes eligible again when its target
remaps. For an interaction that must end with its current buffer, use
`(grab id :move :buffer-generation generation)`, taking the generation from its
window record. This guard also prevents accepted effects from regaining capture
after a rejected detach/remap. The shipped drag uses this guard and retires its
old private state when policy can next settle. Each unit may return only one
grab. `inspect` exposes the resolved
`:grab` and the backend's applied `:native-grab` as `(:id ID :mode MODE)`, or nil.

`(once :ready 100)` declares a one-shot timer; `(interval :refresh 1000)` declares
a repeating timer. Names are keywords, scoped to the declaring extension.
Milliseconds are integers from 0 through 2147483647; zero starts immediately,
then an interval repeats every millisecond. Timers begin after the transaction
commits and use monotonic time. Intervals schedule from handler completion,
coalescing late ticks without a catch-up burst. The unchanged declaration
keeps its schedule across reevaluation; a fired one-shot stays recorded so it
cannot accidentally rearm. Omit the effect to cancel; adding it again starts a
fresh timer. Each extension may own one timer of either kind per name.

Successful source reload starts fresh timers for that source while preserving
its copied reducer state. Failed reload retains its active timers. Unmount
removes every timer belonging to that owner, including already-due events.
Timer handlers use the same transaction and 25 ms timeout as other reducers.
A failed one-shot is consumed; an interval continues at its next tick. `inspect`
reports `:timers` with each owner, name, kind, milliseconds, and pending/fired
status. The runtime accepts at most 4096 timer records and bounds each delivery
pass to 64 events and an 8 ms budget, checked between handlers.

`(watch-file NAME PATH &key content-limit)` owns file-content notifications.
Names are keywords local to the declaring extension. Relative paths resolve
beside its source; the parent directory must exist when the effect is prepared,
but the file may be absent. The default content limit is 65536 bytes, with
accepted values from 1 through 65536. There is no initial callback. Closing a
written file or moving a replacement into its path delivers a private event:

```lisp
(:type :watch :name :settings :path "settings.txt"
 :content "new contents" :status :changed :error nil)
```

Content preserves whitespace and must be valid UTF-8. Unreadable, nonregular,
oversized, or invalid UTF-8 files deliver empty content with `:status :read-error`
and an error string. Deletion alone does not deliver a content event; recreating
the file and atomic editor saves remain observable. Parent directory loss
delivers `:unavailable`; the watcher retries every 250 ms and delivers
`:restored` with current content when it resumes. Kernel queue overflow delivers
`:overflow` with current content. A read error takes precedence over these
content statuses. Each bounded native batch coalesces matching events into one
current-content notification.

Equal declarations retain their watch across ordinary transactions. Successful
source reload replaces its watches while preserving copied reducer state;
failed reload or native publication retains accepted watches. Omission,
unmount, rule-instance retirement, and shutdown close their descriptors and
discard queued callbacks. A failed callback consumes its notification and keeps
watching. Other reducers can observe published state changes without receiving
the private watch event. Watch callbacks can return commands after publication.
These effects operate independently of source auto-reload and `--no-watch`.

`inspect` reports `:watches` with owner, name, path, content limit, and status.
The runtime bounds the registry to 256 declarations and 512 accepted/candidate
resources; kernel inotify and descriptor limits can reject preparation earlier.
Each pass services at most 64 resources within an 8 ms budget, checked between
handlers. The Linux helper uses separate nonblocking queues per watch and
releases every candidate descriptor on preparation or publication failure.

`(exec-async NAME COMMAND &key timeout output-limit)` runs a shell command owned
by the declaring extension. Names are keywords local to that extension; each
name identifies one declaration. The defaults are 30000 milliseconds and 65536
combined stdout/stderr bytes. Command strings may contain at most 65536
characters and no NUL; `timeout` is 1 through 2147483647 milliseconds and
`output-limit` is 1 through 65536 bytes. Captured stdout and stderr are separate,
decoded as UTF-8 with replacement for invalid bytes, and trimmed at both ends.

The declaring reducer receives one private completion event such as
`(:type :exec :name :check :status :exited :code 7 :stdout "ok" :stderr "" :error nil)`.
Completion statuses are `:exited`, `:signaled`, `:timeout`, `:output-limit`,
`:io-error`, and `:spawn-error`. `:code` is the direct child's exit code or signal
number, or nil when spawning failed; `:error` describes a runtime failure or is
nil. `inspect` exposes `:executions`, including `:pending`, `:running`, and
`:terminating` records, plus the `:retiring-executions` count.
Commands returned by an `:exec` handler pass the
same one-shot command gate as timer handlers and run after the successful
transaction. The completion is consumed before the handler runs, so a handler
error is recorded without replaying the completion.

Ownership is published with the accepted transaction, while spawning is
deferred until a service pass. An unchanged declaration retains its running
lease or, after delivery, its tombstone across ordinary transactions. A
successful source reload starts a new source generation; a failed reload keeps
the current lease. Omitting the effect, unmounting its owner, or replacing its
source cancels the old lease and reaps it before a replacement starts. A failed
group stop retains the pidfd and ownership for a later retry. The registry
allows 64 active declarations and 128 active plus retiring records. A rejected
transaction starts no new command and preserves current ownership. Unmount
cancels a command but cannot undo external work it already performed.

`(run-once NAME [COMMAND] &key cwd env run)` declares a session launch, while
`(service NAME [COMMAND] &key cwd env restart reload)` declares a supervised
owner-scoped process. Names are keywords local to the declaring owner; both
forms share the same process-name namespace. COMMAND defaults to the name as
the program. For example:

```lisp
(list
 (run-once :session "notify-send ready")
 (run-once :worker '("./worker" "--foreground")
           :cwd "bin" :run :once-per-config-version)
 (service :watcher '("watcher" "--foreground")
          :cwd "services" :env '(("MODE" . "desktop"))
          :restart :on-failure :reload :always-restart))
```

The command may be a shell string, which runs through the packaged `sh`, or an
argv list, whose element boundaries are preserved. Shell selection is independent
of child `PATH` overrides; direct argv lookup uses the child's `PATH`.
A relative `cwd` is resolved
against the declaring source's directory. Environment overrides replace
matching inherited variables, are canonicalized by name, and are passed as a
full child environment without mutating the compositor's environment. The
`run-once` default is `:once-per-session`; `:once-per-config-version` starts a
new session child for each successful source reload. Once-per-session history
uses the source, owner name, and process name, so omission, remount, or a changed
command does not replay a successful launch. `service` defaults to `:on-exit`
restart and `:keep-if-unchanged` reload behavior. Its restart choices are
`:never`, `:on-failure`, and `:on-exit`; `:always-restart` restarts even an
identical service declaration on successful reload.

Ownership is prepared before native publication and process creation is
deferred until the accepted transaction's service pass. Services are cancelled
when their effect is omitted or their owner is unmounted. Reload retains an
unchanged running service under `:keep-if-unchanged`; other replacements wait
for the old lease to be cancelled and reaped. Restarts
observe a one-second floor from the previous launch. Active work rotates across
declarations, with at most 64 jobs and an eight-millisecond budget checked
between jobs per pass. A failed service spawn is
suppressed until a source or declaration change; a failed run-once spawn leaves
its session stamp unused and retries at the next accepted transaction. A failed
source reload preserves the current process and policy. Started run-once
processes become session-owned and survive owner unmount and reload, including
background members that remain in their private process group; compositor
shutdown cancels each session group and reaps its direct child. Allocation,
encoding, environment-size, and OS launch failures can occur after commit;
they appear as `:spawn-error` without undoing accepted policy.
`inspect` reports `:managed-processes` records, `:retiring-processes` and
`:once-processes` counts, and the `:process-history` slot count. The session count
also includes imperative `spawn` and `launch` children; `:pending-spawns` reports
reserved imperative commands. One-shot history
is bounded to 4096 identities and retains only data stamps across unmount.

The runtime accepts 64 process declarations in total and reserves at most 128
live or pending processes, including retiring services, session one-shots, and
imperative commands. Their slots are reserved together before publication, even
when a command list contains a reload before a later spawn.
The Lisp boundary allows 128 argv entries, 64
environment overrides, and 65536 combined command, argv, cwd, and override
characters. The native helper bounds each argv/environment vector to 1 MiB,
argv to 128 entries, the full environment to 4096 strings, and `PATH` lookup to
256 segments.

On Linux, `support/executions.c` exports separate ABI 1 helpers for captured
`exec-async` commands and inherited-stdio managed processes. Both use private
process groups and pidfds; the managed process API supplies `/dev/null` stdin,
inherits stdout/stderr, and reaps only its direct child. Group liveness remains
observable after a shell leader exits, and cancellation covers members that
remain in that group. Descendants that detach, create another process group, or
daemonize are outside this lease. The helper needs Linux 6.9 or newer for
process-group pidfd signals; the native backend is ABI 30.

`spawn`, `launch`, `close-window`, `output-power`, `quit`, and `reload` are one-shot commands. Only key,
button, UI click, timer, watch, exec, request, IPC, and explicit control command dispatch may return them.
They execute after effect validation and commit. They are not undoable. User-launched
applications belong to the session, survive extension unmount, and use the same
private process-group cleanup as `run-once`.

`(spawn COMMAND &key cwd env)` accepts a shell string or literal argv, with the
same source-relative cwd and environment options as `run-once`. Existing
`(launch EXECUTABLE &rest ARGUMENTS)` preserves its literal argv interface.
On the native backend, both commands mint a fresh xdg activation token after
commit and export it as `XDG_ACTIVATION_TOKEN` and `DESKTOP_STARTUP_ID`.
Explicit environment entries override either variable independently. A failed
spawn revokes its generated token and reports a runtime error; it does not undo
accepted policy. Declarative services and run-once launches do not mint tokens.

Activation tokens expire after ten seconds and are consumed on use. Client
tokens without input serials request urgency; serial-bearing tokens must pass
seat, focus, and serial validation at token creation. Later focus changes do not
invalidate an accepted token. Requests received before the first map wait
for window adoption, retaining their urgency class and original deadline.
Destroying the target releases pending requests. Rust's option to accept invalid
serials is not yet available. The native backend bounds server-issued tokens to
64, total tracked token records to 128, and pending target surfaces to 64.
Excess requests are ignored; exhausted token issuance is a post-commit spawn
failure.

## Retained shell surfaces

`(ui KIND &rest PROPERTIES)` constructs copied declarative UI data. Return
`(shell-surface NAME TREE &key width height anchors margin layer exclusive-zone
visible output background color font-size)` as an owned effect. Names are keywords
scoped to the declaring extension. [examples/shell.lisp](examples/shell.lisp)
is a working bar with a private click counter; its state survives reload.

The supported kinds are `:row`, `:column`, `:stack`, `:button`, `:text`, `:rect`,
`:spacer`, `:separator`, `:progress`, `:circular-progress`, `:image`, and `:icon`.
Containers take `:children`; rows, columns, and buttons also take `:gap`, `:padding`, `:align`,
and `:justify`. Common properties include `:width`, `:height`, `:grow`,
`:background`, `:border-color`, `:border`, `:radius`, `:key`, and `:on-click`.
Text takes `:text`, `:font`, `:size`, `:line-height`, and `:color`.
Progress values range from zero to one, with `:color` and `:track`; circular
progress also takes `:size` and `:thickness`. Separators take `:orientation`,
`:thickness`, and `:color`. Colors accept `#rgb`, `#rrggbb`, `#rrggbbaa`, or an
integer in RRGGBBAA order. Unsupported kinds and properties reject the proposal.

Images take `:src` for a PNG or JPEG file. Their intrinsic dimensions are file
pixels; explicit `:width` and `:height` are logical units scaled per output.
The image stretches to its allocated rectangle. Icons take `:name`, optional
SVG `:path`, logical `:size` (default 16), and optional `:color` tint. They draw
in a centered square inside their allocated rectangle. An omitted or empty
path searches `XDG_DATA_DIRS` for scalable SVGs in hicolor, Adwaita, then breeze;
an explicit path does not fall back to a theme. A missing image is transparent,
and a missing icon displays its name. Relative file paths resolve beside the
extension source, so separately mounted policies can use the same filenames.

```lisp
(ui :row :children
  (list (ui :image :src "assets/avatar.png" :width 32 :height 32)
        (ui :icon :name "audio-volume-high" :size 20 :color "#cdd6f4")))
```

Decoded assets and failed lookups belong to the extension's source generation.
Redraws reuse the captured files; successful source reload refreshes them.
Rejected proposals preserve accepted assets, and removing the last surface
using an asset releases it. Files are read only during preparation. The loader
accepts regular files up to 16 MiB for PNG/JPEG or 1 MiB for SVG; decoded rasters
and SVG paint targets are limited to 16384 pixels per axis and 64 MiB each.
The pool permits 256 assets and 128 MiB across live and candidate generations,
counting decoded raster bytes and encoded SVG bytes, not SVG library overhead.
Limit or allocation failures reject the proposal. Missing or malformed files
are cached fallbacks. SVGs cannot load external file or URL references.

A surface `:background` is a color, `(:image PATH :fit FIT)`, or
`(:shader PATH :fps FPS)`. An image paints a PNG or JPEG beneath the tree. FIT
is `:cover` (the default; fills and crops), `:contain` (letterboxes), or `:fill`
(stretches). A worker thread decodes the file and scales it once to the
surface's physical size, so the compositor thread never decodes and only a
surface-sized texture stays resident. Until a new image is ready, and when it is
missing or malformed, the surface keeps showing its previous background and the
failure is logged. Files may reach 128 MiB and decode to 32767 pixels per axis
and 512 MiB; the scaled result counts toward the asset pool. A surface whose
tree draws nothing keeps no canvas texture.
[examples/wallpaper.lisp](examples/wallpaper.lisp) shuffles a directory with Mod+w:

```lisp
(shell-surface :wallpaper (ui :stack)
  :anchors '(:top :right :bottom :left) :layer :background
  :background '(:image "/home/me/Pictures/wall.png"))
```

A shader paints a GLSL ES 3.00 fragment shader beneath the tree, in Shadertoy's
convention: the file defines `void mainImage(out vec4 fragColor, in vec2 fragCoord)`,
with `fragCoord` in physical pixels from the surface's bottom-left corner. Tomoe
declares `iResolution` (the surface size), `iTime` (seconds since the shader
compiled), `iFrame`, and an always-zero `iMouse`; shaders that read channels or
other inputs fail to compile. Output alpha is ignored. `iTime` advances in steps
of 1/FPS (default 30, from 0 to 1000; 0 draws one still frame), and the shader
runs only to repaint its area after a step, not on every frame. A fullscreen
client on direct scanout stops it entirely. The file compiles when the
declaration is prepared, on the compositor thread; a compile error logs the GLSL
message and keeps the previous background. Shader files may reach 256 KiB.

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    fragColor = vec4(0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4)), 1.0);
}
```

Explicit UI dimensions, margins, padding, font sizes, and reservations are logical units,
scaled once on each output. A surface defaults to every connected output,
top/left/right anchors, intrinsic unstretched dimensions, zero margins and zone,
layer `:top`, background `#1e1e2e`, text `#cdd6f4`, and font size 13.
Opposing anchors stretch across the output; other zero surface dimensions use
the tree's intrinsic size. Positive exclusive zones reduce `:workareas` after
external layer reservations, without adding margins. Surfaces remain fixed to
the screen as the camera moves. `:background` and `:bottom` surfaces render just
above external layer surfaces of the same layer, so windows cover them; `:top`
and `:overlay` surfaces render above every client, below the cursor. Hit testing
follows that same order and the drawing clips.

Set `:on-click` to a keyword command and optionally set a stable `:key` on the
element. Visible click keys must be unique within a surface; omitted keys use
`node-N` from full-tree preorder, including clipped nodes. Use explicit keys
when dynamically reordering nodes.
A left press delivers a private `:ui` event to the owning reducer with
lowercase `:command`, `:surface`, `:output`, and `:element` strings, surface-local
physical `:x`/`:y`, `:button`, and `:modifiers`. The deepest eligible element
handles the click; a parent handles uncovered descendants. On `:top` and
`:overlay` surfaces, blank regions and other buttons consume both edges without a
callback. On `:background` and `:bottom` surfaces, input outside elements with
`:on-click` or `:on-hover` passes through to bindings and the layers below. A
held press retains only its consumed edge after unmount; stale callbacks cannot
enter a replacement source. Other reducers observe resulting context changes, without receiving the
private click. Command effects returned by this event run after publication.

Pango shapes text, and Cairo rasterizes text, vectors, images, and icons before
publication. Unchanged plans reuse their native textures, and frame rendering
runs no extension code.
Handler tokens survive text, style, and geometry changes while the source,
surface, output, click key, and command remain the same. Already queued clicks
therefore reach the same handler even if the first click redraws or resizes it.
Removal, command changes, source replacement, and output destruction retire the
old token. Failed generations preserve accepted resources. Removing a
surface releases its textures and callback entries; native output destruction
immediately retires its textures and callbacks before Lisp drains the output
notification. Output invalidations force presentation even if a same-name
replacement has identical geometry, restoring the shell with fresh callbacks.
`inspect` includes `:native-ui` resource and rasterization counters, with
`:assets`, `:asset-bytes`, and cumulative `:asset-loads` for the asset pool;
`hit-test` adds optional `:ui` metadata. Trees, dimensions, surface counts, and
retained memory have explicit bounds. Shell keyboard ownership, the remaining service widgets,
and the complete Rust shell ceremony remain unfinished parity work.

## Notifications

The compositor hosts `org.freedesktop.Notifications` on the session bus when
the name is free. An absent bus or an existing daemon leaves this service
unavailable without preventing startup. The producer belongs to the session,
including under `--bare`; policy reload and popup unmount leave it running.

Declare `:reads (:services)` and call `(service-state snapshot :notifications)`
to receive a copied plist with `:available` and `:notifications`. Each record
contains `:id`, `:app`, `:summary`, `:body`, and `:urgent`, ordered oldest first.
Equal snapshots do not invalidate consumers. These external facts survive a
failed consumer transaction; accepted effects remain, with an update owed
against the latest facts on subsequent settlement.

The shipped `notification-popups` extension renders ordinary shell surfaces
on every output, at the top right with an eight-pixel margin. Cards use a
summary (falling back to the app name), optional body, and an urgent background.
It reserves no workarea and has no click handlers. Unmounting it releases its
surfaces; remounting reads the retained notifications. An empty list creates
no surfaces.
The preview shows at most two summary lines (256 characters) and eight body
lines (512 characters). Its card limit accounts for output scale, total canvas
space, and duplicated UI data. Full notification text remains in the service
snapshot.

The daemon implements `GetServerInformation`, `GetCapabilities`, `Notify`,
and `CloseNotification`, advertising only `body`. Icons, actions, and hints
other than byte-valued urgency are ignored, matching the previous daemon.
Replacement reuses its ID and moves the record to the end without a close
signal. Timeout zero persists, negative timeouts select five seconds, and
positive values use milliseconds. A replacement cancels the preceding expiry;
fresh IDs skip occupied IDs. Known explicit closes emit `NotificationClosed`
with reason 3, expiration uses reason 1, and unknown closes are harmless.
The producer retains at most 64 records and 256 KiB of text, with 4096 UTF-8
bytes per text field. Actions and hints each have a 64-entry limit. A rejected
notification leaves the current records and their deadlines intact.

Bus processing is bounded between compositor turns; it does not run reducers
inside a D-Bus method callback. Bus failure clears the service facts and closes
the producer without restarting it. `service-state` is distinct from the
`service` effect, which owns a managed child process.

## Media players

The session observes `org.mpris.MediaPlayer2.*` players through MPRIS without
claiming a bus name. Declare `:reads (:services)` and call
`(service-state snapshot :mpris)` to read the selected player. Its plist has
`:available`, `:player-name`, `:status`, `:title`, `:artist`, `:album`,
`:art-url`, `:length`, `:position`, and `:volume`. Availability describes the
bus connection; an available service can have no players. Empty defaults are
empty strings, zero length and position, and volume `1.0d0`.

Selection prefers a playing player, then the most recently active player,
with name ordering as a final tie-break. Discovery and signals update activity;
late read replies do not make an older observation newly active. The name
omits the MPRIS bus prefix.
Artist arrays are joined with `", "`; metadata replacement clears omitted
fields. Length and position are integer seconds. Position is sampled after
status or metadata transitions and updated by `Seeked`; it stays fixed between
events. This service observes players and does not provide playback commands.

Mount `examples/media.lisp` for an ordinary owned media label. Unmounting the
label releases its surfaces while the observer retains current player facts.
The preview uses at most 30 metadata characters in a 240-by-28 logical canvas;
full metadata remains available in the service snapshot.
Reload and consumer failure follow the same ownership and recovery rules as
notifications. Discovery and property reads are asynchronous, with bounded
pending work. Owner changes retire old queries; newer property and seek events
invalidate stale replies. Newer reads also fence older replies for the same
field, even when no intervening signal was emitted. Bus failure clears the
snapshot and closes the observer without restarting it.

The observer tracks at most 64 players and 128 names, with 16 aliases per
player. Text fields and joined artists have a 4096-byte UTF-8 limit, artist
arrays allow 32 entries, and property/metadata dictionaries allow 64 entries.
Initial discovery scans at most 1024 bus names; invalidation lists allow 16 entries.
At most 256 requests are outstanding, each with a two-second timeout. Reads
denied by temporary capacity remain pending for retry. Malformed or oversized
updates leave the prior player state intact.

## Battery

Declare `:reads (:services)` and call `(service-state snapshot :battery)` for
`(:available BOOL :percent INTEGER :charging BOOL)`. Availability means a
battery is present. An absent battery reports `nil`, 100 and `nil` respectively.
The producer belongs to the compositor session, including under `--bare`;
consumer unmount, reload and failure follow the same rules as other services.

UPower's aggregate `DisplayDevice` supplies the primary reading over the system
bus. Percentage is rounded and clamped to 0–100, and only UPower's charging
state sets `:charging`. Property changes and invalidations update the snapshot;
owner and read revisions prevent delayed replies from restoring retired facts.
Daemon replacement reseeds from its new owner. An authoritative UPower reading
with no battery keeps the absent defaults.
Failed property reads preserve accepted UPower facts. Before the first valid
reading, failure activates sysfs while retaining the daemon's identity for
later recovery. The two sources keep separate caches, so a partial UPower
update cannot inherit sysfs percentage, presence or charging state.

When UPower is unavailable, the fallback reads the first name in lexical order
under `/sys/class/power_supply` whose `type` is `Battery`. Capacity is an integer
clamped to 100, with malformed or missing values defaulting to 100; status must
be exactly `Charging` after trimming whitespace. The fallback reads immediately
and then every 30 seconds, rescanning to replace a removed battery. It stops
polling when no battery remains. As in Rust, an initially empty fallback does
not poll for later insertion. System-bus loss closes that connection and leaves
the fallback running when a battery exists.

`TOMOE_POWER_SUPPLY_ROOT` can select an alternate power-supply mount before
startup. `DBUS_SYSTEM_BUS_ADDRESS` selects the system bus independently of the
session bus used by notifications and MPRIS. Mount `examples/battery.lisp` for
an ordinary owned percentage and charge indicator; unmounting it releases its
surfaces while the producer retains current facts.

Bus setup and individual reads have two-second deadlines. Each poll drains
at most 64 messages, checking a four-millisecond budget between messages;
at most 32 reads remain outstanding. Property dictionaries and invalidation
lists allow 32 entries. Sysfs attributes are limited to 256 bytes and scans
to 256 directory entries per attempt, with one retry if the selected directory
changes during a sample. Malformed and nonfinite UPower values leave facts intact.

## Network

Declare `:reads (:services)` and call `(service-state snapshot :network)` for
`(:connected BOOL :ssid STRING-OR-NIL :strength INTEGER)`. Disconnected defaults
are `nil`, `nil` and 0. The producer belongs to the compositor session, including
under `--bare`; consumer removal or replacement leaves current facts available.

The system-bus observer follows NetworkManager's primary active connection and
its access point. State 50 and above means connected, including local-only
connectivity. Wired connections have no SSID and strength 0. Hidden Wi-Fi SSIDs
also report `nil` but retain the access point's strength. Strength preserves the
reported byte, normally 0–100. SSID byte arrays use lossy UTF-8 decoding like Rust;
embedded NUL remains part of the Lisp string and crosses JSON IPC as an escape.

Connection and access-point changes withdraw downstream details immediately.
Owner, path-lifetime and property/read revisions fence delayed replies;
invalidated properties trigger fresh reads. Failed reads retain accepted facts,
while an unsuccessful initial root read activates the independent sysfs fallback.
Daemon replacement clears the old chain and seeds from its new owner.

The observed access point comes from the active connection's
[`SpecificObject`](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.Connection.Active.html),
as in Rust. NetworkManager defines this as the object used during activation;
this chain does not independently follow a device's later roaming access point.

The fallback scans `/sys/class/net`, excluding exactly `lo`, and reports connected
if any interface's trimmed `operstate` is exactly `up`. It supplies no SSID and
strength 0. A readable directory is sampled immediately and every 30 seconds,
even when empty, so later interfaces can be discovered. If the directory is
unreadable at startup, no polling source is installed. A directory that becomes
unreadable later reports disconnected while its existing cadence continues.
Fatal system-bus loss closes that connection and starts the fallback without
reconnecting the bus.

`TOMOE_NETWORK_SYSFS_ROOT` selects an alternate network-class mount before
startup, and `DBUS_SYSTEM_BUS_ADDRESS` selects the system bus. Mount
`examples/network.lisp` for an ordinary owned status/SSID indicator. Its bounded
single-line label replaces control characters for display; full service facts
remain available to other consumers.

Bus setup and individual reads have two-second deadlines. A poll handles at
most 64 messages and checks a four-millisecond budget between messages; pending
reads are capped at 64. Paths, text and raw SSIDs allow 4096 bytes, and property
dictionaries and invalidation lists allow 32 entries. Sysfs scans visit at most
256 entries and read at most 4096 bytes per `operstate`, rejecting NUL, oversized
or nonregular attributes and checking that the sampled directory still matches.
Failed or malformed reads wait for a subsequent signal, invalidation or owner
change to request fresh data, so a persistently broken reply cannot cause a
request loop.

## Tray

Declare `:reads (:services)` and call `(service-state snapshot :tray)` for
`(:items (...))`, initially `(:items nil)`. Each item is a copied plist with
`:service`, `:path`, `:id`, `:title`, `:status` and `:icon-name` strings, in
registration order. Metadata starts empty until the first successful read.
The service/path pair distinguishes multiple items from the same application;
the path field is an addition to the Rust Lua facade. Status remains the item's
reported string, including passive or unfamiliar values.

The compositor owns `org.kde.StatusNotifierWatcher` at `/StatusNotifierWatcher`
on the session bus and acts as its own host. Another watcher or an unavailable
bus leaves empty facts; startup does not replace or queue behind an existing
watcher. The producer belongs to the compositor session, including under
`--bare`. Removing or reloading a consumer leaves registrations and current
facts available for surviving and replacement consumers.

Registration accepts a bus name, a name followed by an object path, or a path
relative to the caller's unique bus name. An empty argument uses the caller
and `/StatusNotifierItem`. Exact service/path duplicates are idempotent.
The watcher resolves the service owner before publishing the item. Owner loss
withdraws its rows; ownership transfer clears old metadata and reads the new
owner. Delayed replies are fenced by registration lifetime, unique owner and
property/read revisions. Item update signals must match the owner and path.
`NewIcon`, `NewTitle`, `NewStatus` and `NewToolTip` request fresh metadata;
property-change signals also update facts, and invalidations withdraw their
fields while requesting another read. Missing or wrongly typed fields preserve
accepted values. Strings retain their Unicode and control characters in service
facts; display consumers choose their own sanitized copies.

The watcher provides typed `Get`/`GetAll` properties and registration signals
from the [KDE watcher interface](https://raw.githubusercontent.com/KDE/kstatusnotifieritem/master/src/org.kde.StatusNotifierWatcher.xml).
It reports itself as a host and protocol version 0. These property replies
repair the Rust implementation's incomplete watcher discovery responses.
Fatal bus loss withdraws all items and closes the producer; it does not reconnect
or retry acquiring the watcher name.

Mount `examples/tray.lisp` for an ordinary owned icon row. The service preserves
all registered items while the row bounds its display. Like Rust, observation
uses `Id`, `Title`, `Status` and `IconName`; pixmap arrays and menu/activation
controls are outside this service. The optional row supplies consumer and asset
ownership that can be exercised independently of the producer.

Bus setup and individual reads have two-second deadlines. A poll handles at
most 64 messages and checks a four-millisecond budget between messages. There
are at most 64 item reservations and 128 pending calls. Names allow 255 bytes,
paths and individual metadata fields 4096 bytes, and dictionaries/invalidation
lists 64 entries. Retained service/path and metadata text is capped at 256 KiB;
an oversized candidate preserves accepted facts. Failed discovery releases its
reservation, and a failed metadata read waits for a fresh signal. Neither is
retried indefinitely. Consumer IPC exports also obey the ordinary JSON limits.

## Window rules

`window-rule` declares a named, owned rule. `:app-id` must equal the window's
app id and `:title` must occur in its title, both case-sensitive; anything
richer goes in the `:match` predicate. All supplied matchers must match.
Omitting them matches every known window.

```lisp
(define-extension "editor-rules" (:reads () :state nil)
    (snapshot state event)
  (declare (ignore snapshot event))
  (values state
    (list
      (window-rule :editor :app-id "org.example.Editor"
        :title "Project"
        :properties '((:workspace . 2) (:border . "#89b4fa"))
        :state '(:ticks 0)
        :apply (lambda (window snapshot local event)
          (declare (ignore snapshot))
          (when (eq (getf event :type) :timer)
            (incf (getf local :ticks)))
          (values local
                  (list (place (getf window :id) 80 80 960 640)
                        (interval :refresh 1000))
                  nil))))
    nil))
```

The full form is `(window-rule NAME &key app-id title match properties reads
state apply)`. `NAME` is a keyword local to its owner. `:properties` is an alist
of copied data; later matching declarations replace equal keys, including an
explicit `nil` value. Matcher and callback fields are excluded. Properties have
no built-in meaning: a layout can read `:rules` and use `rules-for` to interpret
them. The shipped WM consumes workspace/fullscreen/focus at admission; a border
property still requires a rendering policy that consumes it.

`:match` receives `(window snapshot)` and returns a truth value. Declare its
context dependencies in the rule's `:reads`. `:apply` receives
`(window snapshot state event)` and returns new private state, its complete
owned effects, and one-shot commands. Its window record includes `:properties`,
the same merged alist returned by `rules-for`. Applications implicitly read
`:windows` and `:rules`; additional subscriptions such as `:outputs` or `:key`
belong in the rule's `:reads`.

Ordinary extension reducers run before rule applications. Applications then
refine their effects in declaration order. Each matching owner/rule/window
combination has independent state and its own names for timers, file watches, async execution,
services, and bindings. Ordinary updates preserve that identity. New matches
receive `:mount`; successful source replacement reapplies to existing windows
with fresh rule state. Rule events also contain `:rule`, `:window`, and `:parent`.
Timer, file watch, and execution events stay private to their instance; key events
target its binding owner, and button/request events target its window.
`inspect` lists the internal owners and state under `:rule-instances`.

Omitting a rule, losing its match, withdrawing its window, or unmounting its owner
withdraws the entire instance and cancels its owned resources. Session run-once
processes retain their documented shutdown lifetime. Predicates and applications
run inside the candidate transaction: callback errors, dependency cycles, and
native rejection preserve the accepted rules and resources. External window
destruction still removes dead scopes even if policy evaluation fails.
An xdg buffer detach keeps the same rule generation, private state and resource
leases. Applications can read `:buffered` to suspend work that needs content;
the shipped drag policy ends its interactive grab when that buffer disappears.
Applications may return one-shot commands on `:mount` as well as the ordinary
input/timer/watch/execution/request events; commands run only after successful
settlement and only while their originating scope still exists.

Keep predicates and reducers pure apart from their returned values. They can
run more than once during dependency settlement. Nested rules are allowed and
are discovered in later reconciliation passes; settlement permits 16 rounds
and at most 4096 matches. Callback evaluation and pattern work are bounded.
These limits reject a candidate instead of leaving partially installed rules.

## Extension lifecycle

`src/runtime.lisp` owns the context and the only extension-to-native write path.
Each dispatch works on candidate module state. Each reducer in a round sees the
same pre-round context. After the round, the runtime computes changed keys and
runs only their declared consumers. The transaction must settle within 16
rounds before it reaches the native scene. Output effects are previewed each
round without changing the live outputs. A dependency cycle involving an
output mode or scale is rejected before any native policy writes.

Each reducer has a 25 ms SBCL timeout, at most 512 effects and 32 commands, bounded
data copying, and no host handles. Loading a source file has a one-second
timeout. Invalid results, undeclared reads, callback errors, and dependency
cycles retain the previous managed policy. A failure is attributed to the unit
that caused it: `inspect` reports that unit's `:failures` count and last error
next to the runtime-wide one, and the whole transaction is still discarded.
Errors appear on stderr and through `inspect`. Native preparation and binding
allocation failures preserve accepted policy. Detected failures after native
publication stop the compositor. One-shot command failures cannot undo earlier
commands.

Client and output facts survive a failed transaction. Their consumers remain
pending until a later transaction commits successfully, including a key,
mount, reload, or unmount transaction. `inspect` reports these invalidations in
`:pending-context`, bounded to `:windows`, `:window-geometry`, `:layers`, `:outputs`,
and derived `:workareas`. Read-only
inspection and idle time do not retry callbacks. Recovery uses the latest
snapshot and the new transaction's event; failed events and commands are never
replayed. Derive persistent window adoption and removal from snapshots, then
apply the current input action. Transient actions tied only to a failed event,
such as accepting a client state request, are discarded with that transaction.

The main loop yields after 64 ordinary native events or about 4 ms, between
complete transactions. Queued window and layer observations form a FIFO fence:
they settle before private callbacks, including facts queued by an earlier
callback in the same service pass. Resource membership is checked again after
reconciliation. This fence can extend the drain budget; a transaction is never
interrupted midway. Pending native events make the next backend poll nonblocking,
so event chains allow IPC, timers, watches, and processes to progress.

Unmount reconstructs geometry, visibility, stacking, focus, bindings, output
configuration, layer overrides, window state, camera, and the grab from remaining
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

## JSON IPC

`tomoe msg METHOD [JSON]` speaks Rust's JSON wire version 2 over the persistent
mode-0600 `$XDG_RUNTIME_DIR/tomoe.NAME.sock` socket. Children inherit its path in
`TOMOE_SOCKET`. The client uses that variable, then `WAYLAND_DISPLAY`; explicit
`--socket NAME` overrides both. It prints a pretty JSON result, returns nonzero
for errors, and keeps `subscribe` open to print compact event lines.

```sh
tomoe msg version
tomoe msg windows
tomoe msg wm_state
tomoe msg subscribe '{"events":["wm_state","focus_change"]}'
```

Each UTF-8 line is a request such as `{"id":1,"method":"windows"}`. Replies are
`{"id":1,"result":VALUE}` or `{"id":1,"error":"message"}`; omitted or null IDs
execute without a reply. IDs are unsigned 64-bit integers. Events are
`{"event":"NAME","payload":VALUE}`. Built-ins are `version`, `windows`, `outputs`,
`view`, `subscribe`, `quit`, and `screencast_select`. The portal's
`screencast_select` (params `app_id`, `types`) becomes a `(:type :screencast
:token N :app-id S :monitor B :window B)` event; an extension answers it, now or
from a later key or UI event, with `(screencast-answer N :output NAME)`,
`(screencast-answer N :window ID)` or `(screencast-answer N :deny)`. With no
extension reading `:screencast` the reply is `{"action":"fallback"}`. The
builtin `screencast` picker honours a window rule property `:screencast`
(`nil` denies, an output name casts it) for the requesting app, answers a single
candidate directly, and otherwise opens a menu.
Built-ins take precedence over extension methods.

`windows` returns ascending IDs, metadata, visible committed physical geometry,
actual seat focus, and acknowledged fullscreen/maximized flags. A pending client
configure changes neither the reported logical size nor these flags. Hidden geometry is null
and `mapped` is false. An exclusive layer surface can hold
the keyboard while policy retains a window focus target. `outputs` includes
physical geometry, usable areas, and scales. Core events are `window_open`,
`window_close`, `focus_change`, `outputs_changed`, and coarse `keyboard_activity`
with a `hand` of `left` or `right`. An xdg buffer detach emits neither
`window_close` nor another `window_open` on reattachment. A policy-visible
detached window remains `mapped:true` with its old world position and 0×0 size;
policy-hidden geometry remains null. X11 windows follow the same xdg lifetime
through xwayland-satellite.

Subscribe with omitted/null params, `{}`, or `{"events":[]}` for all events;
otherwise use an array of exact event names. Repeating subscribe replaces the
filter. There is no initial replay: fetch the relevant method for a snapshot.
Malformed lines and invalid request shapes are ignored. Duplicate JSON object
keys are rejected on server requests, including nested params. This is stricter
than Rust's last-value handling of duplicates inside params.

Extensions declare `(serve-state "name" JSON-VALUE)` or
`(serve-method "name" :command)` effects. Later owners win per method; omission,
unmount, and rule withdrawal restore the previous owner. A method call privately
delivers `(:type :ipc :owner NAME :method METHOD :command "command" :params VALUE)`
to its reducer. Declare `:ipc` in reads when using that event. Return one
`(ipc-reply VALUE)` command for a result, or omit it for JSON null. Replies and
`(broadcast "event" VALUE)` commands execute after accepted publication, in
command order. Place a reply before a quit command if both are needed.

`(announce "event" VALUE)` owns a continuous event snapshot. Only the winning
value is published after successful settlement. Value changes and replacement
source generations announce once; withdrawal restores an earlier owner or
announces null when none remains. Failed callbacks, dependency settlement, and
native preparation preserve the accepted endpoint/state and emit no proposed
announcements or commands. The shipped `wm` uses these same public effects.

Construct JSON with `(json-object (cons "key" VALUE) ...)` and
`(json-array VALUE ...)`; read an object with `(json-get OBJECT "key" DEFAULT)`.
`t`, `+json-false+`, and `nil` represent true, false, and null. Values remain
bounded copied data; objects and arrays have distinct empty representations.
There is no Lisp reader evaluation in JSON parsing.
The codec caps strings at 65,536 characters, numeric lexemes at 256 characters,
and JSON nesting/nodes at 64/32,768. The runtime's copied-data budgets also
count the lists representing JSON containers, so deeply nested or very large
values can hit those limits sooner when passed into reducers.

Server input and per-client queued output are capped at 1 MiB, with at most
128 clients. Oversized frames and stalled readers whose backlog fills are
disconnected. Each service pass caps accepts, bytes, and request counts and
rotates clients under a cooperative time budget. Parsing and reducer execution
complete within their own limits. Output resumes even without new input, and
half-closed peers receive replies for complete pending requests. Shutdown drains
queued output for at most 100 ms. The command-line client has no timeout.

## Control protocol

The private Unix socket is `$XDG_RUNTIME_DIR/NAME.ctl`, mode `0600`. It accepts
one request per connection. A frame is a decimal character count, a newline,
then that many UTF-8-decoded characters of Lisp data. The maximum is 1048576
characters. Framing permits newlines inside window titles.

Requests are `(1 :inspect)`, `(1 :hit-test X Y)`, `(1 :frames)`, `(1 :reload)`, `(1 :mount "path")`,
`(1 :unmount "name")`, `(1 :command "owner" "command")`, `(1 :event "PLIST")`,
or `(1 :quit)`. `:event` carries one string holding a data property list whose
`:type` must be `:key`, `:button`, or `:grab`; the compositor reads it and
dispatches it like a real input event. Replies are `(1 :ok result)` or
`(1 :error "message")`. Version 1 is exact; unknown versions fail explicitly.
Reader evaluation and dispatch syntax such as `#.` and circular object labels
are disabled, and the reader accepts exactly the data the printer emits,
including the cons dot an extension's own state may contain. This is a data
protocol, not an unauthenticated REPL.

## Source and limits

- `src/`: Common Lisp API, state/effect runtime, native bindings, control, CLI.
- `native/backend.h`: the native ABI. No pointers cross into extension snapshots.
- `native/presentation.c`: candidate scene/input state and publication for policy
  transactions, with candidate rendering when outputs change.
- `native/internal.h`: the shared server struct and the seams between modules.
- `native/event.c`: the serialized event queue Lisp drains.
- `native/output.c`: output lifecycle and staged mode/scale/position commits.
- `native/space.c`: physical transforms, rendering, hit testing, output membership,
  and frame callbacks. Scene trees retain client lifetime and stacking; frames
  redraw in full.
- `native/window.c`: xdg toplevels and popups.
- `native/layer.c`: layer-shell surfaces and arrangement.
- `native/ui.c`, `src/ui.lisp`: retained shell textures, text/vector rasterization,
  declarative layout, and clipped hit targets.
- `native/ui-assets.c`: source-owned PNG/JPEG/SVG decoding and retained asset lifetimes.
- `native/input.c`: pointer routing, keyboards, key bindings, pointer grabs.
- `native/buffer.c`: wl_shm and linux-dmabuf client buffers.
- `native/base.c`: buffers, boxes, regions, transforms, format sets, syncobj
  timelines, xdg positioner math, xcursor themes, logging.
- `native/surface.c`: wl_surface, subsurfaces, regions, viewports, fractional
  scale, presentation feedback, and explicit sync.
- `native/node.c`: the stacking tree surfaces and shell chrome render from.
- `native/screen.c`: outputs, their state and commits, frame scheduling,
  wl_output, xdg-output, and the cursor image.
- `native/kms.c`, `native/session.c`: DRM/KMS on libseat, with udev hotplug.
- `native/nested.c`, `native/headless.c`: the nested Wayland and headless backends.
- `native/seat.c`: wl_seat, pointer and keyboard focus, grabs, cursor role.
- `native/selection.c`: clipboard, primary selection, data control, drag and drop.
- `native/virtual.c`: virtual keyboard and pointer devices, output mapping lifetime.
- `native/backend.c`: server lifetime — create, step, destroy.
- `support/executions.c`: Linux pidfd process-group helpers for policy-owned
  commands (separate exec/process ABI 1; requires Linux 6.9+).
- `support/watches.c`, `src/watches.lisp`: Linux inotify file observation,
  owned watch preparation, bounded content delivery, and cleanup (watch ABI 1).
- `builtins/desktop.lisp`: replaceable default policy.
- `examples/`: alternative policies, each mountable on its own.
- `flake.nix`, `build.lisp`: native compilation and saved SBCL executable.
- `DESKTOP.md`, `FINIX.md`, `OUTPUTS.md`, `STARTUP.md`, and
  `WORK.md` are historical checkpoints from the prototype work. They name local
  paths and predate the move to the repository root.

Implemented protocols cover xdg-shell windows and popups (constrained to the
output), xdg-decoration and KDE server decoration, shared-memory and DMA-BUF
buffers, subsurfaces, clipboard, primary selection, wlr and ext data control,
drag and drop, viewporter, fractional-scale-v1, xdg-output, layer-shell,
session lock, idle notify and inhibit, gamma control, wlr output power management,
presentation time,
tearing control, linux-drm-syncobj, relative pointer, pointer constraints,
virtual pointer and keyboard, xdg-activation, wlr and ext foreign toplevels,
wlr-screencopy-v1 and ext-image-copy-capture for outputs and toplevels (Tomoe's
own, in `native/capture.c`; pointer cursor sessions report stopped),
ext-background-effect-v1, and X11 clients through
xwayland-satellite. The ScreenCast portal is the Rust `xdg-desktop-portal-tomoe` in
`portal/`, carried over unchanged from the previous tomoe and installed with
its `.portal`, `portals.conf` and D-Bus service files. Input methods, touch and
tablets are missing. A policy that never releases a grab keeps the pointer until
the grabbed surface disappears. A layer surface's anchors, margins, and size
stay the client's request.

Tomoe and ShojiWM informed the separation of mechanism from policy and explicit
ownership of reactive effects. Local reference clones live in `ref/`, which
is listed in the workspace `.gitignore`. wlroots 0.20, which Tomoe was built on
until it replaced each layer, informed native API use and fallback paths.
