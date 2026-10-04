# Writing extensions

An extension is a function from what the compositor sees to what you want it
to do. This page is the whole API: the reducer contract, every context key and
effect, settings, the lifecycle, and live reload.

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
(place id x y width height visible) ; the window draws and takes input only inside this box
(focus id &key (raise t))           ; NIL clears focus; :RAISE NIL leaves ordering to other effects
(raise-window id)
(show-window id)
(hide-window id)
(bind-key '(:super :shift) "r" :reload)
(configure-keyboard :layout "us" :options nil :repeat-rate 25 :repeat-delay 600)
(sound :key '("tick-1.wav" "tick-2.wav") :gain -18) ; or :button, :open, :close
(layer id &key layer exclusive-zone keyboard visible)
(fullscreen id flag)
(maximize id flag)
(grab id mode)                     ; :move or :resize
(launch "foot")                    ; argv, not a shell command string
(spawn '("./worker" "--foreground") :cwd "bin" :env '(("MODE" . "desktop")))
(close-window id)
(output-power :toggle)             ; :on, :off or :toggle; optional connector name
(clipboard-copy path "image/png" :delete t) ; own the clipboard with a file's bytes
(media-control :play-pause)        ; :next, :previous
(adjust-brightness -5)             ; percent of the backlight's range
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
  `:audio` supplies static `(:volume 1.0d0 :muted nil)` defaults;
  that record has no live producer or control actions.
- `:system`: CPU, memory and GPU readings, sampled once a second only while a
  mounted extension reads this key: `(:cpu-total J :cpu-idle J
  :cpu-temperature C :memory-total KB :memory-available KB :gpus (...))`.
  CPU counters are /proc/stat's cumulative jiffies (idle includes iowait), so
  usage comes from the difference of two samples. The CPU temperature is the
  first k10temp, coretemp or zenpower hwmon's Tctl, Package id 0 or Tdie
  label, else its temp1. Each GPU is `(:name S :busy % :vram-used MiB
  :vram-total MiB :temperature C)`: every NVIDIA GPU through NVML, named
  `nvidia0` onwards and loaded from the driver's `libnvidia-ml.so.1` at first
  use, then every DRM card with `gpu_busy_percent` in sysfs. Temperatures are whole degrees Celsius
  or `nil`.
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
  `:output`, `:x`, `:y`, `:width`, `:height`, `:layer`, and `:click-through`.
  Geometry is physical.
- `:bindings`: resolved modifiers, keysyms, owner names, and command names.
- `:keyboard`: the resolved whole-seat XKB policy, with `:rules`, `:model`,
  `:layout`, `:variant`, `:options`, `:repeat-rate`, and `:repeat-delay`.
  Empty strings for `:rules`, `:model`, `:layout`, and `:variant` request the
  XKB defaults. `:options nil` uses `XKB_DEFAULT_OPTIONS`; `:options ""`
  explicitly disables those defaults. A later owner replaces the complete
  policy, and removing it restores the preceding owner or the session default.
- `:key`, `:button`, `:grab`, `:request`, `:ui`, and `:activity`: event subscriptions, not stored context values.

Layer surfaces are arranged by the compositor from the client's own anchors,
margins, and exclusive zone, and they are never in `:windows`, so a tiling policy
does not have to know about them. A layer surface that asks for exclusive
keyboard interactivity takes the keyboard while it is mapped, which is what a
launcher needs; hiding it gives the keyboard back to the focused window.

Events include `:map`, `:buffer`, `:unmap`, `:metadata`, `:geometry`, `:outputs`, `:layer`, `:key`,
`:button`, `:pointer`, `:grab`, and `:request` in their `:type` field. Key events carry `:owner` and a
lowercase `:command` string. Button events carry `:id` (zero for empty space, a
layer surface id when a panel was hit), an evdev `:button` code, `:state`
(`:pressed` or `:released`), pointer `:x` and `:y` in physical world coordinates
for windows or physical screen coordinates for layers and empty space, and
the keyboard `:modifiers` mask. Pointer events carry `:state` (`:enter` or
`:leave`), the window `:id`, and `:moved`, which is `t` when device motion
crossed the window's edge and `nil` when the window moved under a still pointer
or the compositor moved the pointer. `:metadata` events carry the same fields as
`:map`, plus `:request` (`:fullscreen` or `:maximize`) when a client asked for a
state, which is the policy's chance to accept or ignore it. While a unit owns a
grab, pointer motion arrives as `:grab` events with `:id`, `:mode`, `:x`, `:y`,
and the delta since the previous event. Lifecycle evaluation receives `:mount`;
reactive reevaluation receives `:change` with the changed `:keys`. An owned timer
delivers `(:type :timer :name NAME)` to its declaring reducer; other reducers
receive only the resulting context changes. Every physical key press delivers
`(:type :activity :hand "left")` or `"right"` to the reducers that read
`:activity`: the same coarse hand as the `keyboard_activity` IPC event, never
the key. While nothing reads it, a key press runs no transaction. Activity
events cannot return one-shot commands.

`(settings :focus-follows-mouse t)` makes the shipped WM focus the window the
pointer moves into. A window that a layout change slides under a still pointer
does not take focus. `(settings :pointer-follows-focus t)` moves the pointer to
the middle of a newly focused window unless the pointer is already over it.

A pointer lock or confinement, which games request for mouse look, is active
only while its surface has both pointer and keyboard focus. Focusing another
window releases the pointer, and a persistent constraint takes it again once
its surface has both.

Native xdg activation delivers `(:type :request :id ID :request :activate)` or
`:urgent`. These requests leave window facts unchanged and are delivered once.
The shipped WM accepts `:activate` by revealing the target's workspace and owning focus;
`:urgent` leaves focus alone. A replacement focus policy can ignore either
request or choose another target. Unmounting focus policy removes this response.
Failed reducers and rejected native publication retain the accepted focus and
do not replay the request on a later transaction.

Owned effects are `publish-state`, `place`, `focus`, `raise-window`, `show-window`, `hide-window`,
`bind-key`, `configure-output`, `virtual-output`, `layer`,
`fullscreen`, `maximize`, `grab`, `set-view`, `once`, `interval`, and
`exec-async`, `run-once`, `service`, `sound`, and `window-rule`.
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
lease is held are ignored. The native lease is per device, not a
global latch on raw keycodes.

An unchanged equal binding in the same source generation retains its native
lease across unrelated transactions. Removing and re-adding it, changing
either command, or replacing the source cancels an existing release callback,
even if the new declaration is textually identical. A failed reload keeps the
previous binding and its leases. The `command OWNER NAME` control operation
considers only the owner's key bindings: it first invokes a matching press
command, or invokes a matching release command when no press has that name; the
synthetic release event has `:state :released` but no `:device` or `:keycode`.
A name that only pointer bindings carry is an error.

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

An output draws and commits a frame only when something asks for one: a
client commit, a policy, shell or cursor change, an animation, a capture or
gamma request, or a shader background's next step. With nothing to show it
stays idle and commits nothing.

Frame callbacks fire with the output frame that shows their surface. A surface
on no output, such as a window placed outside the view, gets its waiting
callbacks answered in one batch when it commits, at most once a second. Without
that, a client that draws without waiting for callbacks, like OBS's preview at
swap interval 0, would get all of them at once on its return, overflow its
connection, and be dropped by libwayland.

Each output frame redraws only what changed since the output buffer it reuses
last held. The frame is first walked without drawing, recording every draw
with a hash of its parameters. Draws that appear, disappear, move, change
parameters or change stacking order damage their boxes; a client commit damages
only its declared buffer damage. Blur that overlaps damage widens it to the
blur's sampled area. The union of damage over the buffer's age clips the real
pass. Captures, configuration previews, and buffers older than eight frames
draw in full. `(1 :frames)` or CLI `frames` reports each output's frame
counter, last drawn frame, buffer age, drawn and total pixels, draw count,
direct scanout state, the wall time spent producing frames in `:frame-ns`, and
`:frame-times`, the number of frames that took under 0.25, 0.5, 1, 2, 4, 8 and
16 ms, then longer. `TOMOE_DEBUG_DAMAGE=1` draws every frame in full and
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

On Linux, `support/executions.c` provides helpers for captured
`exec-async` commands and inherited-stdio managed processes. Both use private
process groups and pidfds; the managed process API supplies `/dev/null` stdin,
inherits stdout/stderr, and reaps only its direct child. Group liveness remains
observable after a shell leader exits, and cancellation covers members that
remain in that group. Cancellation sends the group SIGTERM, then SIGKILL once
the leader has exited or a second has passed, so a stopped process can remove
what it created; a group whose leader has already exited gets SIGKILL at once. Descendants that detach, create another process group, or
daemonize are outside this lease. The helper needs Linux 6.9 or newer for
process-group pidfd signals; the native backend is ABI 35.

`spawn`, `launch`, `close-window`, `output-power`, `clipboard-copy`,
`media-control`, `adjust-brightness`, `quit`, and `reload` are one-shot commands. Only key,
button, UI click, timer, watch, exec, request, IPC, and explicit control command dispatch may return them.
They execute after effect validation and commit. They are not undoable. User-launched
applications belong to the session, survive extension unmount, and use the same
private process-group cleanup as `run-once`.

`clipboard-copy` reads its regular file, at most 64 MiB, on the compositor
thread when it runs, so it suits small local files such as a screenshot; the
read blocks the compositor for its duration. Each paste is then written from
memory as the receiver drains its pipe, and a receiver that closes early only
ends its own transfer.

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
Destroying the target releases pending requests. The
`:honor-xdg-activation-with-invalid-serial` setting accepts serial-bearing tokens
that fail that validation. The native backend bounds server-issued tokens to
64, total tracked token records to 128, and pending target surfaces to 64.
Excess requests are ignored; exhausted token issuance is a post-commit spawn
failure.

## Settings

`(settings KEY VALUE ...)` owns compositor settings. Later owners replace
individual keys, grouped keys merge field by field, and unmounting an owner
restores whatever came before it, down to the defaults. The types and defaults
are the `+settings+` table in `src/api.lisp`.

```lisp
(define-extension "look" (:reads ()) (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (settings :border '(:width 2 :radius 10 :focused "#89b4fa")
                          :shadow '(:range 24)
                          :animations '(:window-move (:spring (:stiffness 400)))))
          nil))
```

| Key | Default | What it does |
| --- | --- | --- |
| `:mod` | `:super` | The key `:mod` means in bindings: `:super`, `:alt`, `:control` or `:shift`. |
| `:scale` | 1 | Scale for outputs that set none, from 1/4 to 8. |
| `:border` | 2 px, square | `:width`, `:radius`, and the `:focused` and `:unfocused` colours, `#7aa2f7` and `#3b4261`. |
| `:shadow` | 12 px | `:range` in pixels, `:color` (`#00000099`), and `:power`, the falloff from 1 to 4. |
| `:blur` | off | `:enabled` blurs behind windows given `:blur t`, layer surfaces that ask for it through ext-background-effect, and layers whose namespace is in `:layer-namespaces`. `:passes`, `:offset` and `:anti-artifact-margin` shape it. |
| `:animations` | on | `t`, `nil`, or `(:window-move SPEC :window-open SPEC)`. A spec is `nil`, `t`, `(:spring (:damping-ratio 1 :stiffness 800))` or `(:ease (:duration-ms 150 :curve :ease-out-expo))`; a curve is `:linear`, `:ease-out-quad`, `:ease-out-cubic`, `:ease-out-expo` or four cubic Bézier values. By default windows move on a critically damped spring and open with a 150 ms ease. |
| `:focus-follows-mouse` | off | The shipped WM focuses the window the pointer moves into. |
| `:pointer-follows-focus` | off | Moves the pointer into a window that gains focus. |
| `:touchpad`, `:mouse` | libinput's | libinput options for every touchpad or mouse: `:tap`, `:natural-scroll`, `:accel-speed`, `:accel-profile`, `:dwt`, `:left-handed`, `:scroll-method`, `:click-method` and the rest of `+input-device-settings+`. |
| `:devices` | none | The same options for one device by name: `'(("Logitech G Pro" :accel-profile :flat))`. |
| `:tearing` | off | Lets a window that asks for asynchronous presentation tear. |
| `:nested-size` | 1280×800 | The window's size when Tomoe runs nested. |
| `:screenshot-freeze` | on | Freezes the picture while you select a screenshot region. |
| `:watchdog-ms` | 1000 | How long one extension may run on one event before it is stopped and its failure reported. |
| `:wait-for-frame-completion` | off | Waits for each frame's GPU work before going on, for measuring frame cost. |
| `:force-server-side-decorations` | off | Answers clients that ask to draw their own decorations with server-side mode. |
| `:honor-xdg-activation-with-invalid-serial` | off | Accepts activation tokens whose input serial fails validation. |

`(window-properties ID &key radius tearing blur border)` overrides these for one
window: `:radius`, `:tearing` and `:blur` (where a supplied `nil` denies), and
`:border '(:focused COLOR :unfocused COLOR)`. Omitted keys fall back to the
settings, and later owners replace each key.

## Extension lifecycle

`src/runtime.lisp` owns the context and the only extension-to-native write path.
Each dispatch works on candidate module state. Each reducer in a round sees the
same pre-round context. After the round, the runtime computes changed keys and
runs only their declared consumers. The transaction must settle within 16
rounds before it reaches the native scene. Output effects are previewed each
round without changing the live outputs. A dependency cycle involving an
output mode or scale is rejected before any native policy writes.

Each reducer runs under the `:watchdog-ms` time limit, one second by default,
with at most 512 effects and 32 commands, bounded
data copying, and no host handles. Loading a source file has a one-second
timeout. Invalid results, undeclared reads, callback errors, and dependency
cycles retain the previous managed policy. A failure is attributed to the unit
that caused it: `inspect` reports that unit's `:failures` count and last error
next to the runtime-wide one, and the whole transaction is still discarded.
Errors appear on stderr and through `inspect`, which also keeps the last 16
native error lines, libwayland's included, in `:native-errors` as
`(:count N :recent (LINE ...))`. Native preparation and binding
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

## Live reload

Configured sources are watched and reloaded when they change, four times a
second. A reload waits until a source has looked the same twice in a row, so a
half-written file is not loaded. The baseline is the content the runtime loaded,
not the first thing the watcher sees, so an edit made while a mount is still
settling is still caught. A source is known by the path it was given and read
through symlinks, so re-pointing a symlink, as a Nix switch does, reloads it
like an edit; relative paths in it still resolve beside the file the link
points to. Success prints nothing: the new policy is visible in `inspect` as
a higher `:generation`. `--no-watch` turns source auto-reload off for one
instance; owned `watch-file` effects still run.

Every load evaluates its source in a fresh package, which the file's
`(in-package #:tomoe-user)` names, so its functions and variables never replace
the ones a running policy calls. A source that fails to load, including one
that refers to a function or variable nothing defines, even behind an
`fboundp` check, keeps the previous policy mounted and running and reports
through the last error. Packages that no mounted extension came from are
deleted.

`examples/monocle.lisp` is an alternative layout that shows only the focused
window. Mount it after the default policy to override tiling. Removing it
restores the lower-priority layout without restarting clients. The other
examples are `workspaces.lisp` (nine tags), `float.lisp` (floating windows moved
and resized by a Super drag), `layer-inset.lisp` (tiling that insets by
layer exclusive zones), and `zoomer.lisp` (a floating canvas with planes, where
Mod+drag moves, resizes or pans, Mod+scroll zooms around the cursor, Mod+Tab and
Mod+1..9 switch planes, and Mod+f fits a window to the view; publish
`:zoomer-settings` to change its step sizes). `headset.lisp` declares two
virtual outputs and turns every other connector off. `special.lisp` adds scratchpad
workspaces over the default tiling: Mod+Shift+grave parks the focused window,
Mod+grave shows or hides it centered at three quarters of the work area, and a
window rule property `:special "name"` parks a window on open. Parked windows
are published as `:wm-exclude`, which the default `wm` leaves out of tiling.
