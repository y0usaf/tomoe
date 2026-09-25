# Rust parity inventory

Baseline: Rust/Smithay tree at git revision `6de3ba6` (that commit is only a
README edit; its parent tree is the feature baseline). Current tree is a Common Lisp
policy runtime, wlroots 0.20 C backend, and early pure-Lisp server. Paths prefixed
`6de3ba6:` are baseline evidence; compare behavior, not syntax.

Status: **Implemented** means behavior exists; **Partial** means only a subset
works; **Missing** means no corresponding behavior was found; **Unverified**
means hardware/client evidence is absent. Docs index features; source/tests decide
when a documented feature is a stub (`README.md`).

The current x86_64 build and full `nix flake check` pass. The core integration
suite reports 1041 assertions, including
headless shm-screencopy pixels, native layer ownership/zones/client updates,
three real X11 managed/unmanaged cycles preserving identity and layout, and
grab ownership through real target unmap/remap/destruction and reducer failure,
and recovery of failed external changes without replaying input or commands,
plus physical fractional-scale pixel boundaries, negative output origins, XDG
geometry offsets, camera projection, inverse hits, camera ownership rollback,
mixed-output virtual pointer delivery and cursor pixels, stationary pointer
retargeting, real seat grab transitions with two devices holding one button,
output reconfiguration across Xwayland idle shutdown and restart, prospective
output dependency settlement, cycle rejection before native writes, and
consecutive command/reload commits without stale output confirmation replay,
prospective layer geometry/workareas, fractional panel pixel/input boundaries,
recovery of consumers that read only workareas, first-commit candidate scene
pixels, rejected output commit rollback without client configures, scene-only
publication without intermediate layer sizes, and binding allocation recovery
(`tests/integration.lisp`, `tests/coordinates.lisp`, `tests/input-mapping.lisp`,
`tests/pointer-grab.lisp`, `tests/xwayland-idle.lisp`, `tests/output-settlement.lisp`,
`tests/output-revisions.lisp`, `tests/layer-settlement.lisp`,
`tests/workarea-recovery.lisp`, `tests/presentation.lisp`,
`tests/scene-publication.lisp`, `tests/executions.lisp`,
`tests/execution-helper.py`, `tests/execution-scheduler.lisp`,
`tests/processes.lisp`, `tests/process-helper.py`,
`tests/process-scheduler.lisp`, `tests/activation.lisp`,
`tests/activation-helper.py`, `tests/keyboard.lisp`, `tests/held-keys.lisp`,
`tests/held-events.lisp`, `tests/keyboard-seat.lisp`,
`tests/logical-keyboard.lisp`, `README.md`). The
confirmed x86_64 run records 1041 passed and 0 failed. The revision fixture
also passes 14 assertions in isolation, the layer fixture
passes 41, the presentation fixture passes 21, and scene publication passes 29.
The process supervisor fixture adds 29 packaged assertions, four process-helper
groups, and twelve direct scheduler scenarios covering shell/argv spawning,
source-relative cwd and environment canonicalization, restart/reload policy,
session-owned background groups, failure suppression, global declaration and
128-process reservation limits, and shutdown cleanup. A pure Lisp backend smoke
also retained service/session-child identities across reload, cancelled the
service on unmount, and stopped the session child on shutdown.
Activation and imperative spawning add 36 packaged assertions, including real
valid/stale serials, validity after later enters, pre-map urgency and expiry,
single use, rejected reducer/native publication, request commands, env overrides,
shipped focus ownership, and session cleanup. The native token helper exercises
pool bounds, revoke, ten-second expiry, and destruction with live tokens. A pure
Lisp imperative smoke also verified argv/cwd/env, reload/unmount lifetimes, and
shutdown cleanup.
JSON IPC adds 60 codec assertions, ten isolated transport groups, and a packaged
compositor fixture covering unsigned IDs, CLI/environment discovery, subscription
filters, method/announcement ownership, private rule calls, duplicate replies,
reducer/native rollback, real window lifetimes, WM snapshots, and native keyboard
focus/activity. Committed window geometry adds 60 dependency/settlement/recovery
checks, a real paused-client test of pending configures, native hits, fractional
scale and atomic size/state publication, and pure-backend buffer geometry checks.
Xdg lifetime checks cover same-ID detach/remap, retained private rule state and
timers, zero detached geometry, IPC open/close boundaries, hidden policy changes,
unbuffered close, and focus changes during the remap handshake. The delayed
activation regression fails against the preceding package. Shipped drag capture
is restricted to its original buffer generation, including failed detach/remap
recovery; the preceding package fails all five stale-drag assertions. The desktop
reducer suite passes 53 checks. A separate full-runtime pure-Lisp JSON smoke
retained the same window ID, scope state and placement across detach/remap and
closed the role while bufferless.
The full `nix build -L` and `nix flake check -L` pass with native ABI 23 on
x86_64-linux, including deferred output admission and backend requests, owned
output lifecycle and rollback, real nested resize, bounded event scheduling with
private callback fences, and pure-backend loading without the native module.
The retained shell foundation's standalone layout
suite passes 165 checks (`tests/ui-layout.lisp`). `tests/shell-ui.py` exercises
the complete compositor process with real text/vector pixels, fractional scale,
exclusive zones affecting client geometry, private clicks, cached textures,
candidate output buffers, rejection, reload, and zero surface/texture/callback
counts after unmount. It also checks canceled edges across two pointer devices,
balanced client releases on shell takeover, device removal, and fresh remount.
Image/icon coverage adds real PNG/JPEG/SVG pixels, alpha, tint, theme lookup,
missing-asset fallbacks, source-relative isolation, cache reuse and reload,
and rejected output commits with candidate asset cleanup. The first successful
commit after retry contains the new asset pixels. `tests/ui-assets-native.py`
checks decoder formats, ownership, clipped references, aborted candidates,
output loss, and bounded asset storage. Pure-backend materialization remains
independent of the native loader (`tests/pure-geometry.lisp`).
Owned file watches add direct inotify and overflow/recovery tests, private
scheduler lifetimes, real compositor content and shell pixels, native rollback,
reload/remount descriptor cleanup, and actual pure-backend delivery.
Direct native coverage in `tests/ui-native.py` verifies cache ordering, aborted
candidates, handler continuity across redraws and movement, callback retirement,
and output loss. Shell-takeover, cross-device cancellation, and queued-click
redraw regressions fail against their preceding builds. Another 38 runtime
checks replace a real output with identical name and geometry, verifying shell
restoration, callback retirement, failed-generation recovery, and idle cache
reuse (`tests/shell-output-lifetime.lisp`); the pre-fix runtime fails restoration.
The X11 queue-drain
regression now proves admission through Wayland's checked callback with an
unreadable XCB fd; its unpatched control fails. This closes a confirmed XWM
admission stall, while the exact cause of the earlier intermittent first-map
timeouts cannot be established retrospectively.
The complete Rust parity goal remains open.
A separate pure-Lisp backend smoke verifies the same JSON endpoints,
owned methods, typed replies, quit and socket cleanup. Tagged JSON values also
obey the runtime's copied-data budgets, which can reject nested/large values
earlier than the codec's own JSON depth/node limits.
Oversized reducer params receive an error reply, and owned payload validation
reserves the wire envelope's byte, node, and depth overhead before publication.
ABI 9 fails the scene mount/unmount configure checks and stops on the injected
binding allocation failure. The previous ABI 8 package
fails two presentation assertions: old first-frame pixels and an extra layer
configure after rejection. Earlier full runs intermittently timed out at the first
X11 map with no runtime error, including a run after scene staging was generalized;
the latest full run completed without that timeout. A confirmed XCB queue-drain
defect causing this admission symptom is now fixed and tested below. Hardware
and other unexercised protocol paths remain separate.
The owned keyboard-policy fixture adds 79 assertions: real client keymap/repeat
delivery through two test keyboards, held Shift and active-group base-level
bindings, options/variant handling, hotplug, ownership restoration, and rejection
after one device has prepared or a later binding allocation fails, a complete
33-key client focus-enter list, and release of the extra key on device removal.
Held bindings add 32 packaged assertions for independent device leases,
modifier/layout changes, consumed client edges, source/binding cancellation,
reducer/native failures, release IPC, invalid declarations, and teardown.
Twelve standalone scenarios cover queued events across ownership changes,
source generations, rejected reloads, cross-owner held-state settlement,
36-key device teardown, and held modifiers beyond the former 32-key cache limit during
keymap replacement (`tests/held-events.lisp`). The pure-Lisp
backend rejects unsupported settings without changing its accepted generation
or bindings; its IPC smoke also verifies press/release commands and legacy
three-argument binding syntax without physical device metadata.
The ordinary keyboard-seat fixture adds 39 assertions for first-down/last-up
client edges across devices, duplicate/unmatched input, combined held-key enter
lists, active-device removal with a surviving modifier snapshot, consumed/client
overlap, binding introduction during a hold, and focused clients recreating a
keyboard resource with 34 keys held across two devices, including code 767.
The logical-keyboard fixture adds 47 assertions for shared Shift, Caps Lock,
layout groups and LEDs, explicit backend masks and raw keys, duplicate input,
hotplug/last-device replacement, equal/changed maps, and atomic rejection of
physical or logical keymap staging. The preceding package reproduced the new
cross-device state and late-resource failures; both fixed fixtures pass in the
full check (`tests/keyboard-seat.lisp`, `tests/logical-keyboard.lisp`).
Owned rules add 70 native assertions for matching, property merging, refinement,
independent state/resources, binding leases, failed reload/native publication,
metadata changes, unmap/remap, and unmount. A standalone runtime suite covers
copy isolation, nested private-event delivery without replay, command boundaries,
bounded expansion, and dead-window pruning during failed policy evaluation.
The Lua-pattern suite passes 4,572 checks, including 4,440 differential cases
against the pinned Lua 5.4 interpreter (`tests/rules-native.lisp`,
`tests/window-rules.lisp`, `tests/lua-patterns.lisp`).
Independent stacking/visibility adds 132 native assertions with overlapping
clients and a top-layer panel. These cover ownership restoration, focus without
raising, hidden keyboard focus, movement and geometry retention, rule-resource
lifetimes, fullscreen acknowledgments, candidate/rollback pixels during output
configuration, and unmap/remap cleanup (`tests/window-stacking.lisp`).

## Policy and public API

- **Partial — extension model.** `define-extension`, copied bounded snapshots,
  effect ownership, dependency rounds, and one-shot commands exist in
  `src/api.lisp` and `src/runtime.lisp`. The public surface is
  placement/focus/bind/output/layer/fullscreen/maximize/grab/camera/timers,
  owned async executions, managed run-once/services, retained `ui`/`shell-surface`
  declarations, and spawn/launch/close/quit/reload.
  The Rust surface is much larger:
  `6de3ba6:docs/lua-api.md`, `6de3ba6:crates/tomoe/src/lua.rs`.
- **Implemented — basic effects.** Placement, independent window raising and
  visibility, focus with optional raising, layer stacking/
  exclusive-zone/keyboard/visibility, fullscreen/maximize, key binding, and a
  single move/resize grab map to `src/api.lisp`,
  `src/runtime.lisp`, and `native/backend.h`.
- **Implemented — owned keyboard policy.**
  `configure-keyboard` owns the complete seat XKB policy, including `:rules`,
  `:model`, `:layout`, `:variant`, `:options`, `:repeat-rate`, and
  `:repeat-delay`; the resolved value is available through the `:keyboard`
  context key. Empty strings select XKB defaults for the first four fields.
  `:options nil` uses `XKB_DEFAULT_OPTIONS`, while `:options ""` explicitly
  disables default options. The latest owner replaces the whole policy, and
  omission restores the preceding owner or the session default. Strings are
  bounded to 1024 characters with no NUL; repeat rate is 0..2147483647 and
  repeat delay is 1..2147483647. The native backend prepares all current
  physical keyboards and the logical seat keyboard before publication and
  applies the accepted profile to future keyboards (`src/api.lisp`, `src/runtime.lisp`, `src/native.lisp`,
  `native/input.c`). An invalid XKB map rejects the complete native
  transaction and preserves the previous keymap and repeat settings. This is
  intentionally stronger transaction behavior than the Rust baseline, which
  retained the old map but still updated repeat information after a map
  failure (`6de3ba6:crates/tomoe/src/state.rs`). The pure-Lisp backend has no
  physical keyboard configuration. The native fixture passes 79 assertions;
  physical hardware input remains unverified. Equal keymaps preserve locks and
  active groups across repeat updates; a different keymap reconstructs state
  from physically held keys.
  Baseline: `6de3ba6:docs/lua-api.md`,
  `6de3ba6:crates/tomoe/src/lua.rs`.
- **Partial — held key bindings.** `bind-key`
  accepts modifier keywords, an XKB keysym, a required keyword press command,
  and an optional keyword release command (`src/api.lisp`, `README.md`). Native
  press/release events carry `:state` (`:pressed` or `:released`) plus numeric
  physical `:device` and `:keycode` fields. Opaque numeric `:source-id` and
  `:binding-id` fields let the runtime reject queued events from removed or
  replaced declarations; they are validation metadata, not stable policy IDs.
  Equal bindings in one source generation retain a held lease across unrelated
  transactions. Removal/re-addition, command changes, and source replacement
  cancel the old release callback even when the new declaration is identical;
  failed reload preserves the old lease. Consumed key-up is always swallowed,
  canceled or otherwise, and repeated presses while held are ignored. Leases
  are per device, improving on the Rust global raw-keycode latch. The control
  `command OWNER NAME` operation can invoke a release command when no press
  command has that name; it marks the synthetic event `:released` and supplies
  no device or keycode. A stateful reducer can read `:bindings` and clear a
  transient hold when its particular resolved declaration loses ownership;
  dependency settlement performs this before the accepted native publication.
  The held-events fixture covers a later owner taking a shortcut, cancellation
  without a replayed release, restoration after unmount, and a fresh press/release.
  With multiple shortcuts, state must be reconciled per declaration (or reset
  conservatively when any owned declaration changes); checking only whether the
  owner still appears in `:bindings` can retain state for a shortcut that was
  replaced. Reducer state survives reload, so held-state reducers should clear
  transient per-device/keycode state on `:mount` (see `README.md`). Both Lisp and
  Rust cancel active holds on successful source replacement. The remaining
  deliberate differences are per-device leases instead of Rust's global raw-keycode
  latch, suppression of repeated presses while a lease is held, and swallowing
  key-up for action-only bindings where Rust forwards an unlatched key-up. Baseline:
  `6de3ba6:docs/lua-api.md:17`, `6de3ba6:crates/tomoe/src/input.rs:83-120,448-564`,
  `6de3ba6:crates/tomoe/src/state.rs:280-287,545-623`, and
  `6de3ba6:crates/tomoe/src/lock.rs:145-189`. The native held-key fixture passes
  32 assertions and the queued-event fixture passes twelve scenarios. Rust's
  binding descriptions, `Mod`/combination-string aliases, session lock, physical
  hardware input, and the remaining advanced input paths are still outside this
  verified slice.
- **Partial — window model/control.** `:windows` carries id, title, app id,
  mapped dimensions, and acknowledged fullscreen/maximize; `:layout` carries
  placement/visibility/state; `:focus` accepts `nil` to clear focus. These cover
  much of the baseline window behavior through data and effects
  (`README.md`, `src/runtime.lisp`). Owned `raise-window`, `show-window`, and
  `hide-window` separate ordering and visibility from geometry. `focus :raise
  nil` changes only keyboard ownership. Hiding retains identity, geometry,
  focus, and rule resources. Xdg buffer detach retains admitted registry/rule
  scopes; native X11 unmap and xdg role destruction retire them.
  `:stacking` resolves visible managed windows bottom-to-top and drives candidate
  pixels, publication and hits. Fullscreen shares the ordinary window stack,
  below top/overlay layers, matching Rust. Moving an existing placement does not
  raise it. Real-client tests exercise ownership/reload restoration, hidden
  keyboard focus, rule lifetimes, fullscreen acknowledgments, and output commit
  failure (`tests/window-stacking.lisp`). Remaining gaps include presentation
  properties and Rust's initial admitted-but-unplaced open snapshot.
  Committed client geometry now has an independent `:window-geometry` dependency;
  candidate policy location, visibility and scale resolve in the same transaction.
  Pending configures leave committed size and acknowledged state unchanged.
  Rust has no public ID before first-buffer admission. After admission, buffer
  detach retains its registry entry, policy ownership and Space location; a
  previously placed window remains `mapped:true` with a 0×0 rectangle until
  reattachment or destruction. Lisp now retains the same admitted xdg registry
  and rule scopes through buffer detach, with a separate `:buffered` fact.
  Actual native input and active grabs require a buffer; policy focus and
  placement can remain owned and change before remap. Shipped drag effects use
  a buffer-generation guard so accepted effects cannot regain capture after
  rejected detach/remap transactions; rule state and timers still survive.
  Rust's "unmapped" snapshot instead includes
  policy-hidden registry windows. Baseline:
  `6de3ba6:crates/tomoe/src/{lua.rs:1941-1968,state.rs:789-838,1193-1320,1323-1345}`,
  `6de3ba6:crates/tomoe/src/{handlers.rs:186-218,394-406,770-798,space.rs:223-318}`.
  Rust pins Smithay `4ca26929`; its window liveness follows protocol-object
  lifetime, and null-buffer commits clear the content bounds. Reattachment on
  the same admitted toplevel retains identity, though Rust's automatic initial
  configure path does not cover that remap. Rust uses external xwayland-satellite
  xdg roles; its source alone does not establish native X11 unmap/override-redirect
  identity rules (`6de3ba6:crates/tomoe/src/xwayland.rs:64-106,273-412`).
- **Partial — events and request consumption.** Map, buffer, unmap, metadata, geometry, key,
  button, grab, and client fullscreen/maximize request data reach reducers
  (`src/runtime.lisp`, `README.md`). Missing callback-style
  consumption and general pointer axis/enter/leave, taskbar,
  move/resize, close, and minimize request behavior. Baseline hooks/dispatch:
  `6de3ba6:docs/lua-api.md`,
  `6de3ba6:crates/tomoe/src/handlers.rs`.
- **Implemented — owned rules.** `window-rule` ANDs app-ID/title Lua patterns
  with an optional predicate; no matchers means every known window. `rules-for`
  exposes copied arbitrary property alists, merged in declaration order with
  later equal keys winning, including NIL. Matcher/callback fields are excluded;
  interpretation remains policy-owned. Matching applications run after ordinary
  reducers and own per-window state, effects, bindings, timers, executions and
  services. Match loss, registry withdrawal, declaration removal and owner unmount retire
  their scope. Successful reload reapplies existing windows with fresh instance
  state and generation; predicate/application/native failure preserves accepted
  ownership. Dead-window pruning does not invoke failed callbacks. Nested
  declarations participate in bounded settlement; private events survive a
  parent's declaration refresh within the same candidate generation.
  The UTF-8 byte matcher retains Lua captures, backreferences, balanced/frontier
  patterns and quantifiers, with 4,440 Lua 5.4 oracle comparisons. Native tests
  verify properties, refinement, independent timers/state, binding dispatch and
  held-release cancellation, metadata/remap cleanup, and callback/backend
  rollback (`src/patterns.lisp`, `src/rules.lisp`, `tests/lua-patterns.lisp`,
  `tests/window-rules.lisp`, `tests/rules-native.lisp`). Broader Rust window
  properties and shipped WM interpretation remain separate gaps. Baseline:
  `6de3ba6:crates/tomoe/src/lua.rs:989-1080,1971-2044,3035-3081`,
  `6de3ba6:crates/tomoe/src/state.rs:579-664,1323-1382`; rule tests:
  `6de3ba6:crates/tomoe/src/lua.rs:3784-3860`.

## Runtime and lifecycle

- **Implemented — transactional Lisp runtime.** Candidate mount/reload/unmount
  state is copied; callbacks are time-bounded; results, duplicate effects, and
  dependency cycles are checked; native scene writes happen after settlement:
  `src/runtime.lisp`. Mode/scale/position effects resolve prospective `:outputs`
  facts during settlement, before placement quantization or native commit.
  Matching native output acknowledgments do not replay reducers, and revision
  ordering prevents a queued older snapshot from replacing a newer commit.
- **Implemented — bounded ordinary native event scheduling.** ABI 23 exposes
  queue count and the FIFO prefix through the latest window/layer observation.
  `src/main.lisp` yields after 64 ordinary events or four ms between complete
  transactions, and uses a nonblocking backend poll while events remain. The
  observation prefix must finish before private work, including between callbacks
  in one timer/watch/exec/process pass and before IPC/control/source reloads.
  Membership is rechecked after reconciliation; a retired managed job from an
  earlier registry snapshot cannot start. Pure backend queries provide the same
  ordering contract. The budget is soft: observation fences may extend it and
  individual transactions are not preempted. Output commits retaining a newer
  request leave the revision watermark behind so interleaved private transactions
  refresh that request's actual identity before matching failures.
  `tests/native-event-fairness.py` proves active read-only IPC, a 5 ms owned timer,
  and async completion during a real 256-request commit chain; shutdown interrupts
  a 512-request chain and cancels its resources. ABI 22 fails the active IPC
  assertion. `tests/native-event-scheduler.lisp` passes 67 checks against the real
  native FIFO with synthetic lifetime payloads: lossless yield, generated events,
  stop/quit without prefetch, all six observation classes, tail-unmap retirement,
  and a first private timer withdrawing a later timer's window scope. Disabling
  the private callback fence reproduces that stale timer delivery.
- **Partial — stateful reload/adoption.** Source loading, stamps, watch
  sweeps, copied state, mount/unmount, existing-client adoption, process
  reaping, and cleanup exist in `src/runtime.lisp`,
  `src/main.lisp`. Mounted extension data and source matching provide the
  baseline persistence behavior across reload/remount; mounted extension/resource
  ownership is the lifecycle boundary. Core state/adoption is covered by current
  tests; existing resource teardown is only partially verified. The concrete
  reconciliation behavior and remaining resource gaps are tracked below.
  Baseline lifecycle: `6de3ba6:docs/lua-api.md`, `6de3ba6:crates/tomoe/src/lua.rs`.
- **Implemented — process supervision.** `run-once` and `service` provide named
  declarations for session launches and owner-scoped supervised processes
  (`src/api.lisp`, `src/processes.lisp`). Commands accept shell strings or
  direct argv; relative cwd is resolved from the declaring source, and sorted
  environment overrides replace inherited values without mutating the host.
  Accepted transactions reserve ownership before native publication and defer
  spawning. `run-once` defaults to `:once-per-session`, with
  `:once-per-config-version` for a new session child per successful source reload. A
  started one-shot remains session-owned across unmount/reload, including
  background members in its private group. Once-per-session history is keyed by
  source, owner name, and process name; removal/remount or a command change does
  not replay a successful launch. Services default to `:on-exit`
  restart and `:keep-if-unchanged` reload; `:never`, `:on-failure`, and
  `:on-exit` are supported restart policies, with a one-second restart floor.
  Active scheduling rotates declarations with a 64-job limit and an
  eight-millisecond budget checked between jobs.
  Failed service starts stay suppressed until a source or declaration changes;
  failed once starts leave their stamp unused and retry at the next accepted
  transaction. Omission and owner unmount cancel services. An unchanged running
  service survives reload under `:keep-if-unchanged`; replacements wait for the
  old lease, and failed reload preserves it. History is bounded to 4096 data
  stamps, with 64 declarations globally and 128 live/reserved processes including
  retiring services and session one-shots. Launch allocation, encoding, inherited
  environment limits, and OS errors are post-commit spawn failures, not scene
  rollback. The native process ABI is 1 alongside the exec ABI 1;
  both are separate from native wlroots ABI 23. Imperative `spawn` accepts the
  same shell/argv/cwd/env options; legacy `launch` keeps its argv interface. Both
  reserve session leases before publication, including outstanding commands
  through a nested reload, then use the pidfd helper and native activation tokens
  after acceptance. Environment overrides win independently over either token
  variable. Spawn failure revokes the generated token. Session process groups
  survive unmount and are stopped on shutdown; this cleanup deliberately extends
  Rust's detached-child shutdown behavior.
  The Lisp boundary allows 128 argv entries, 64 environment overrides, and
  65536 combined command/argv/cwd/override characters. Native vectors are
  bounded to 1 MiB and 4096 environment strings, with at most 256 `PATH`
  segments.
  Baseline: `6de3ba6:docs/lua-api.md`, `6de3ba6:crates/tomoe/src/process.rs`.
- **Implemented — owned timers.** Declarative `once` and `interval` effects
  deliver private `:timer` events through the reducer transaction, with the
  same timeout and command boundary. Equal declarations retain their schedule
  and one-shot completion across ordinary transactions. Successful source reload
  resets that source's timers; failed reload keeps them; unmount discards even
  already-due deliveries. Preparation allocates a bounded registry before native
  publication, and accepted commit arms it. Monotonic scheduling coalesces late
  ticks and bounds each pass (`src/timers.lisp`). The native-backed scheduler
  probe verifies due-event cancellation/reload, private delivery during external
  reconciliation, fair bounded passes, completion-relative recurrence through
  callback errors, zero initial delay, and resource-budget rejection
  (`tests/timer-scheduler.lisp`). An isolated pure Lisp backend smoke also
  verifies a single firing and an empty registry after unmount.
  The packaged API fixture adds 26 assertions, including native allocation
  failure before timer adoption, retry, source reload, cancellation, command
  delivery, duplicate rejection, and one-shot error consumption (`tests/timers.lisp`).
  This supplies the behavior of
  Rust `shell.once`/`shell.interval` through named effects rather than separate
  closures. Retained UI callbacks now have source and keyed-handler ownership through the
  shell foundation below.
- **Implemented — owned file watches.** `watch-file` declares a named,
  source-relative file observation with a private `:watch` content event.
  `support/watches.c` watches the canonical parent directory, so closing a
  written file and moving an editor's replacement into its path both work.
  Existing content is not replayed on registration. Text is untrimmed strict
  UTF-8; read/encoding/size failures deliver empty content with an explicit
  status. Each bounded native batch coalesces matching events to current
  content. Parent loss and restoration are observable, and lost parents retry
  every 250 ms. Queue overflow requests a content resynchronization.
  `src/watches.lisp` retains equal declarations in the same source generation,
  prepares new resources before native publication, and closes rejected
  candidates without consuming accepted watchers' queues. Successful reload
  discards the old source's queues while preserving reducer state; unmount,
  omission, rule retirement, and shutdown close all corresponding resources.
  Membership checks prevent a callback from entering a source retired earlier
  in the scheduler pass. Callback failure consumes the event and keeps watching.
  File watches operate on both backends independently of source auto-reload.
  The active/candidate budgets are 256/512 resources, with earlier rejection
  possible at kernel limits; content is bounded to 65536 bytes per declaration
  and scheduler passes to 64 resources or 8 ms, checked between handlers.
  The helper uses independent nonblocking inotify queues and a separate ABI 1.
  Baseline: `6de3ba6:crates/moonshell-runtime/src/{api,watch}.rs` and
  `6de3ba6:crates/moonshell/src/watcher.rs`. Rust's standalone shell wires these
  callbacks, but the fused compositor drops them with a warning
  (`6de3ba6:crates/tomoe/src/state.rs`). The standalone watcher retains directory
  registrations after callbacks are cleared, delivers each queued event's
  latest content without an application debounce, and clears all callbacks
  before reload. Lisp's bounded coalescing, owner-specific retirement, parent
  recovery, and failed-generation preservation are deliberate ownership changes.
  `tests/watch-helper.py` checks real inotify filtering, atomic saves, parent
  recovery, FIFO rejection, exact content limits, a queue larger than one poll's
  budget, and descriptor cleanup. `tests/watch-scheduler.lisp` covers private
  transactional delivery and resource generations; `tests/watches.py` checks
  real compositor content, dependent state, shell pixels, native rejection,
  reload/remount, and descriptor counts. `tests/watch-pure.lisp` exercises the
  independent pure Lisp backend path.
- **Implemented — owned async execution.** `exec-async` is a named owned effect
  with 30000 ms and 65536 combined stdout/stderr bytes as its defaults. Command
  strings are bounded to 65536 characters without NULs; timeout is bounded to
  1..2147483647 ms and output capture to 1..65536 bytes. Accepted transactions
  publish bounded ownership first and defer spawning until service; rejected
  transactions start no command. Equal declarations retain their running lease
  or a delivered tombstone across ordinary transactions. A successful source
  reload starts a fresh generation; a failed reload keeps the current lease.
  Omission, unmount, and replacement cancel the old lease before a replacement
  runs, retrying a failed group stop while retaining the pidfd and ownership.
  Completion is private to the declaring reducer and carries
  `:type`, `:name`, `:status`, `:code`, `:stdout`, `:stderr`, and `:error`; terminal
  statuses include `:exited`, `:signaled`, `:timeout`, `:output-limit`,
  `:io-error`, and `:spawn-error`. Completion handlers may return one-shot
  commands through the `:exec` command gate; the completion is consumed before
  handler evaluation and handler errors are recorded without replay.
  `support/executions.c` uses a Linux pidfd, a private process group, separate
  nonblocking pipes, and reaps only the direct child. Group cancellation covers
  members that remain in that group; descendants that detach, create another
  process group, or daemonize are outside this lease. Starting a command can
  have external side effects that rollback or unmount cannot undo. The helper is
  ABI 1 and requires Linux 6.9+ process-group pidfd signals; it is separate from
  the native wlroots ABI 23.
  Current isolated evidence is 38 API assertions, five helper checks, and ten
  direct scheduler scenarios, plus a pure-Lisp smoke that observed one private
  `:exec` completion (exit 7, `"pure"` stdout) and empty execution registries
  after unmount (`tests/executions.lisp`, `tests/execution-helper.py`,
  `tests/execution-scheduler.lisp`). These execution checks are included in the
  latest full run, which recorded 1041 passed and 0 failed.
- **Implemented/intentional — startup isolation.** Current CLI selects
  auto/nested/headless/DRM and exports display variables to itself/children
  (`src/main.lisp`). Rust’s optional systemd/D-Bus environment import and
  session assets (`6de3ba6:README.md`, `6de3ba6:resources/tomoe-session.target`)
  are an integration choice, not a requirement to write global session state.

## Coordinates, camera, and geometry

- **Implemented — physical policy coordinates.** Placement uses bounded integer
  physical world pixels; output positions and layer facts use physical screen
  pixels. Client sizes cross one snapped-scale conversion boundary and `:layout`
  reports achievable physical sizes (`src/api.lisp`, `src/runtime.lisp`,
  `native/space.c`). `OUTPUTS.md` records the superseded logical prototype.
- **Partial — physical/world precision.** Physical anchors survive fractional
  scale independently of logical scene positions. Custom rendering and native
  hit testing share final physical destination rectangles, with xdg geometry
  and descendant offsets converted once. Mixed-output pointer routing and cursor
  composition pass checks with three outputs at scales 2,1,1. Relative motion
  converts using the starting output's scale, as in Rust. Transformed outputs
  and hardware behavior still need dedicated evidence.
  Single-output negative origins and scales 180/120 and 123/120 pass captured
  pixel and inverse-hit checks; the latter catches binary-float half rounding.
  Baseline: `6de3ba6:crates/tomoe/src/{space,coords}.rs`.
- **Partial — canvas camera/hit testing.** Owned `set-view`, `:view` snapshots,
  and read-only `hit-test` control use one camera with inverse mapping.
  Windows follow it while layers remain screen-fixed (`native/space.c`,
  `native/input.c`). Conflicting view owners, unmount restoration, failed
  reducer rollback, screen-fixed layers, and native inverse hits pass the suite.
  Per-output physical usable rectangles are exposed as `:workareas` and used by
  the shipped tiler. A policy pointer snapshot remains absent.
  Baseline: `6de3ba6:docs/lua-api.md`,
  `6de3ba6:crates/tomoe/src/space.rs`.

## Render and compositing

- **Implemented — base client composition.** wlroots renderer/allocator,
  scene, xdg/layer trees, cursor routing/rendering, scheduled full-frame output
  commits, seat, Xwayland, xdg toplevels/popups, managed X11, shm, subsurfaces,
  fractional-scale, viewporter, and xdg-output exist in
  `native/backend.c`, `native/window.c`, `native/output.c`,
  and `README.md`. The Rust `RenderElement` type is an implementation
  detail; its absence is not itself a parity gap.
- **Partial — presentation behavior.** Rust adds physical camera rescaling,
  border/shadow/radius shaders, clipping, blur, and render-time animation on top
  of the scene (`6de3ba6:crates/tomoe/src/render/mod.rs`). Current
  physical composition covers client pixels/camera/cursor, but policy has no
  equivalent user-visible window border/radius/blur/tearing properties or global
  shadow/blur settings. Baseline API/settings:
  `6de3ba6:docs/lua-api.md`; state:
  `6de3ba6:crates/tomoe/src/state.rs`.
- **Partial — fused shell/UI.** `ui` and `shell-surface` declare bounded retained
  element trees and per-output shell surfaces. `src/ui.lisp` measures and lays
  out logical lengths in physical pixels, produces clipped draw/hit plans,
  resolves intrinsic sizes and anchors, and subtracts sequential exclusive zones
  from prospective workareas. `native/ui.c` shapes Unicode text with Pango/Cairo,
  rasterizes rectangles/borders and progress widgets, and retains actual textures
  and callback tokens. Unchanged plans reuse their textures; redraws preserve
  tokens while owner, source, surface, output, click key, and command remain
  the same. Accepted removal or replacement of a handler retires its token;
  text, style, and geometry changes do not cancel already queued clicks.
  Visible click keys must be unique within each surface, and generated keys
  use full-tree preorder so clipping cannot rename a later receiver.
  External output invalidations force publication even when replacement facts
  and policy plans are equal; native destruction has already freed the old
  textures. A same-name output return therefore restores the shell with fresh
  callback tokens (`tests/shell-output-lifetime.lisp`).
  Shell layers use the same order for drawing
  and hits, above the client scene and below the cursor. `native/input.c` routes
  left-button commands privately by owner, source generation, and current
  callback token; blank shell regions also consume pointer buttons.
  Preparation joins the native presentation transaction, so rejected rendering,
  output commits, and source replacements preserve accepted resources and state
  (`src/runtime.lisp`, `src/native.lisp`). Evidence is the 165 layout checks in
  `tests/ui-layout.lisp` and the full-process fixture `tests/shell-ui.py`; direct
  native transaction/lifetime coverage is in `tests/ui-native.py`.
  The shell service facade and its widgets remain
  missing. Shell keyboard ownership is unsupported and rejected explicitly;
  `builtins/desktop.lisp` still supplies policy rather than a complete desktop UI.
  Baseline: `6de3ba6:docs/lua-api.md`,
  `6de3ba6:crates/tomoe/src/shell.rs`, and
  `6de3ba6:crates/moonshell-{runtime,render,surface,services}`.
- **Implemented — retained shell images/icons.** `native/ui-assets.c` decodes
  PNG/JPEG images and SVG icons from captured regular-file contents. The baseline
  contract is in
  `6de3ba6:crates/moonshell-render/src/{layout,draw,assets}.rs` and
  `6de3ba6:crates/moonshell-runtime/src/element.rs`: images use file-pixel
  intrinsic dimensions and stretch to their final rectangle; explicit logical
  dimensions scale with the output. Icons default to size 16, accept an SVG
  path or theme name and optional tint, and fall back to name text when loading
  fails. Icons draw a centered square sized to the smaller allocated dimension;
  SVG axes stretch independently, and tint replaces RGB while multiplying alpha.
  An explicit icon path excludes theme fallback. Theme lookup searches
  `XDG_DATA_DIRS` under hicolor, Adwaita, then breeze. Lisp resolves explicit
  relative paths beside the declaring source, whereas Rust uses the process
  working directory. Both cache successful decodes and failed lookups across
  redraws. Lisp additionally isolates caches by owner and source generation:
  successful source reload refreshes files and failures, rejected generations
  preserve accepted assets, and removing the final surface releases its assets.
  Clipped and zero-size nodes retain references until their surface retires.
  Scratch references span preparation and publication and are discarded on
  both success and failure; native surface references keep accepted assets alive.
  Paints use captured data with no frame-time filesystem access. SVG decoding
  has no base URI and cannot resolve external file or URL references.
  Encoded input is bounded to 16 MiB for images and 1 MiB for SVG; raster
  dimensions/paint targets to 16384 per axis and 64 MiB; the shared pool to
  256 assets and 128 MiB across accepted and candidate resources. Pool bytes
  count decoded rasters and encoded SVGs, excluding SVG library overhead.
  The real compositor gallery in `tests/shell-ui.py` proves pixels at scales
  1 and 1.5, source isolation, positive/negative cache lifetimes, failed reload,
  native commit rollback, new pixels in candidate output buffers, and zero
  retained assets after unmount. `tests/ui-assets-native.py` checks decoding,
  ownership rejection, aborts, output loss, and limits; `tests/ui-layout.lisp`
  covers intrinsic sizing, scaling, fallback text, clipping, and retained IDs.
- **Missing — animation behavior.** Render-only springs/easing, retargeting,
  open fades, and target-geometry invariants are baseline requirements
  (`6de3ba6:docs/lua-api.md`,
  `6de3ba6:crates/tomoe/src/animation.rs`); current code has no render clock or
  transient presentation transform.

## Outputs and backends

- **Partial — mode/scale/position.** Staged preferred/max/exact mode, refresh
  matching, scale, fixed/automatic layout, hotplug, and rollback exist in
  `native/output.c` and `src/native.lisp`. Native output previews expose candidate
  physical boxes and scales to policy dependencies without committing them.
  Layer geometry and per-output physical workareas now use one planner for
  preview and live arrangement. Policy output changes now render the settled
  candidate scene before backend commit, then publish that geometry once.
  Multiple-GPU atomicity and post-publication
  allocation recovery remain incomplete; X11's EWMH workarea reports the protocol canvas
  bounds rather than these per-output reservations.
  Unavailable advertised modes now fall back to preferred, then first advertised,
  matching Rust's `pick_mode` and its initial/live callers in
  `6de3ba6:crates/tomoe/src/backend/tty.rs:615-676,732-741,1108-1122`.
  Retained and fresh requests both use this fallback after a connector returns
  with a changed mode table. Progressive-mode selection on hardware remains
  unverified.
- **Implemented — deferred output admission and local failure recovery.**
  Startup configuration settles before queued connector events are drained.
  New outputs expose connector facts without enabling, rendering, creating a
  Wayland output global, or entering the layout. Their initial mode is retained
  for admission and owner removal, including a first advertised mode when none
  is marked preferred. Pending previews include the resolved topology, so the
  first enable commit contains the dependent shell pixels. Explicitly disabled
  ports remain inactive throughout startup and hotplug. Actual backend admission
  failures retain the requested declaration, hold arriving ports inactive, and
  expose `:output-errors` with connector names and messages. Holds belong to the
  connector incarnation, advertised capabilities and declaration; unrelated
  owners and equal policy reloads remain usable, and changing/removing the
  declaration or reconnecting allows recovery. `tests/output-hotplug.py` uses
  real backend additions/removals, registry globals, captured pixels and partial
  commit failure. It also verifies that owned arrival processes run once per
  connector lifetime through failed-generation recovery. Hardware behavior
  remains a separate verification gap.
- **Implemented — deferred backend mode/scale/transform requests.**
  `output_request` retains copied fields and advances a per-connector request
  identity without committing or publishing geometry. Multiple pending requests
  coalesce, and `tomoe_outputs_begin` resolves them underneath existing owned
  declarations. Request identity lives in `:connectors`, keeping `:outputs`
  consumers idle when an owner masks the requested change. The existing
  presentation path renders candidate shell/client pixels and resolves client
  sizes before committing. Successful requests refresh only supplied fields in
  the underlying backend settings; owner removal restores that baseline.
  Reducer and backend failures keep accepted geometry and baseline, expose
  `:output-errors`, and consume the rejected request through a current-state hold.
  A new request or declaration allows recovery. `tests/output-requests.py`
  verifies synchronous callback isolation, coalescing, fractional scale,
  transform geometry/pixels, reducer rejection, partial backend rollback,
  unrelated owners and reloads, and mode-only/scale-only baseline restoration.
  Requests delivered by real commit callbacks preserve the successfully accepted
  request's baseline even if a newer request is rejected. Mode-only callbacks
  during an owner's publication retain the accepted baseline scale, rather than
  copying the owner's provisional live fields. Both regressions fail against
  their preceding ABI 22 packages. Per-request snapshots are prepared before
  backend commit and transferred after success without a late allocation.
  A callback request raised during a failed transaction receives a fresh output
  notification after accepted native state has been restored. Lisp defers to
  ordinary dependency settlement instead of committing the new request with
  retained effects. Operational failure records remain bounded by connected
  ports and survive until publication, so another connector's new request does
  not revive an unchanged rejected request. Process cases cover failed CLI owner
  mounts, private IPC state/reply rejection, failed request commits and independent
  connectors. Submitted rollback and successor buffers are captured separately:
  the previous package loses the notification, and a native-only fix still
  submits the successor with stale shell pixels; the combined fix passes.
  `tests/output-requests-nested.py` exercises real Wayland parent window resizes,
  dependent child shell pixels, reducer rejection and recovery.
  The preceding ABI 21 implementation synchronously commits each request,
  including both intermediate modes in a paired request.
- **Partial — owned display policy fields; hardware operation unverified.**
  `configure-output` owns `:disabled`, `:mirror`, and `:vrr` alongside mode,
  scale and position. Rust's contract is in `6de3ba6:docs/lua-api.md:340-348`
  and `6de3ba6:crates/tomoe/src/backend/tty.rs:938-991,1032-1180`.
  Disabled native outputs retain connector identity but leave active snapshots,
  workareas and Wayland globals. `:connectors` includes disabled ports, so their
  declarations remain installed and can be reversed. Mirrors copy the physical
  origin of an active non-mirror target, overriding explicit position and using
  no automatic packing width. Invalid targets pack after ordinary outputs.
  Unlike Rust's order-dependent mirror pass, mirror chains always fall back;
  resolving them never depends on output insertion order. Unsupported adaptive
  sync requests stay off; supported backend rejection rolls back the transaction.
  Source removal restores the predecessor's complete declaration or the native
  initial settings. `tests/output-policy.py` covers real output globals and
  surface membership, mirror pixels/input and chains in both connector orders,
  prospective consumers, all-disabled recovery, owner reload/unmount and
  partial commit rollback. Rejected transitions preserve global identities and
  produce no surface enter/leave churn; the actual rollback buffer contains
  accepted shell and client pixels. Accepted disablement sends surface leaves
  before making output resources inert. Backend-bound disabled states omit
  fields wlroots forbids while inactive, while policy and rollback retain their
  complete settings. `tests/output-options-native.py` covers native
  validation, adaptive-sync rejection and initial-state restoration, disabled
  frame handling, abort, output loss and recreation. Newly connected outputs
  remain inactive until the full policy transaction admits them.
  Hardware adaptive-sync operation remains unverified.
  Rotation is not an exposed field in Rust's `DisplaySettings`; current Lisp
  also has no owned transform API.
- **Partial/unverified — backend coverage.** Current native uses wlroots
  autocreate with auto/nested/headless/DRM selection
  (`native/backend.c`, `src/main.lisp`). Hardware DRM, physical
  input, and allocation recovery remain unverified (`README.md`). Rust
  has dedicated GLES/winit and DRM/GBM/libseat/libinput multi-GPU paths with
  direct scanout/gamma/damage/tearing/hotplug/VRR:
  `6de3ba6:crates/tomoe/src/backend/{mod,winit,tty}.rs`.

## Input and protocol surface

- **Partial — keyboard/pointer basics.** Current C handles motion/buttons/axis
  forwarding, cursor requests, default and owned xkb keymaps, repeat 25/600
  defaults, key bindings, and one policy grab (`native/input.c`); runtime gets key/button/grab
  events (`src/runtime.lisp`). Virtual-pointer devices share this path, including
  optional output mappings, while nested absolute devices resolve output names.
  Real Wayland tests cover pointer delivery, client cursor membership/pixels,
  multi-device button balancing, grab suppression, and focus/cursor restoration
  without additional motion. Keyboard tests cover two devices, real keymap/repeat
  client messages, active layout groups, and base-level modifier matching.
  Ordinary client keycodes now form a union across devices: one first down and
  last up, combined focus-enter lists, no duplicate/unmatched edges, and stable
  press routing through binding changes. Removing a device retains surviving
  key ownership. One stable logical seat keyboard owns modifier, lock and layout
  group state; Shift on one device affects bindings on another, and lock/group
  state survives removal and replacement of the last device. Consumed binding
  holds remain separate per device. A key-driven XKB state owns global physical
  edges, and a client projection merges explicit source modifier contributions.
  Pinned wlroots patches retain the full 768-code cache for late keyboard
  resources and expose every explicit backend modifier snapshot, including
  unchanged masks that its ordinary modifier signal suppresses. Test keyboards
  verify logical LED synchronization after backend processing.
  Physical input remains unverified, and axis/
  enter/leave are not policy hooks.
- **Partial — input configuration/advanced input.** The owned XKB map/repeat
  policy is implemented as described above. Missing libinput touchpad/mouse/device
  tuning, touch, tablets, relative
  pointer, constraints, or full DnD. Baseline settings:
  `6de3ba6:docs/lua-api.md`; handlers:
  `6de3ba6:crates/tomoe/src/handlers.rs`; implementation:
  `6de3ba6:crates/tomoe/src/input.rs`.
- **Partial — Wayland globals.** Current globals cover compositor, shm,
  subcompositor, data-device manager, viewporter, fractional-scale, xdg-output,
  xdg-shell, layer-shell, xdg-activation, legacy wlr-screencopy, and Xwayland
  (`native/backend.c`). Pure Lisp advertises only its M1 conservative
  set (`backend/server.lisp`).
- **Partial — application activation.** Native xdg-activation translates live
  targets to one-shot `:request` data (`:activate` or `:urgent`) without changing
  window facts or native focus. The shipped WM reveals the destination workspace
  and accepts activation through the regular transaction; replacement/unmounted
  policy can ignore it.
  Rejected reducers/publication consume the request without replay. Native tokens
  are single-use with a ten-second deadline, and pre-map requests preserve their
  request class and original deadline until adoption or target destruction.
  Serial freshness is decided at token creation; later enters do not revoke an
  accepted token. Limits are 64 server-issued tokens, 128 tracked token records,
  and 64 pending surfaces; failed tracking drops the request, and exhausted
  issuance reports a post-commit spawn failure.
  Imperative spawn supplies both activation environment variables. Pure Lisp
  protocol support and Rust's invalid-serial compatibility setting remain absent.
  Baseline: `6de3ba6:crates/tomoe/src/handlers.rs`,
  `6de3ba6:crates/tomoe/src/state.rs`, `6de3ba6:crates/tomoe/src/process.rs`.
- **Missing — protocol families.** Add/exercise primary selection and both data-control
  protocols; xdg/KDE decorations; foreign-toplevel
  list/taskbar; pointer constraints/relative pointer; presentation/tearing;
  explicit sync/dmabuf; gamma; image-copy capture; session lock; idle
  notify/inhibit; touch/DnD. Baseline setup:
  `6de3ba6:crates/tomoe/src/state.rs`; delegates:
  `6de3ba6:crates/tomoe/src/handlers.rs`.
- **Partial — X11.** Managed Xwayland toplevels/metadata/configure and
  override-redirect isolation exist (`native/window.c`). Three real transitions
  in each direction preserve the same identity, re-adopt layout, and exit cleanly
  (`tests/x11-client.c`, `tests/integration.lisp`). Compare Rust’s
  on-demand satellite lifecycle (`6de3ba6:crates/tomoe/src/xwayland.rs`),
  activation, input, and unmanaged behavior before closing this gate.
  The intermittent first-map timeout was not reproduced in 18 isolated startup
  attempts (10 plain, 5 instrumented, 3 at the readiness boundary). These used
  the preceding ABI 7 package with unchanged production X11 code. wlroots and
  Xwayland source inspection rules out clients mapping before the XWM installs
  its root event mask.
  ABI 18 full checks reproduced the timeout before the shell fixtures, while
  reruns passed. The pinned wlroots `x11_event_handler` registered checked
  dispatch but drained XCB events only for a readable-fd notification.
  `patches/wlroots-xwm-queued-events.patch` also drains on checked dispatch
  (mask zero). Synchronous replies can leave events in XCB's internal queue
  after the fd has drained; those events now progress without new X traffic.
  `tests/x11-queued-events.py` and its preload probe create that queue state
  with a real synchronous reply, verify fd unreadability, observe the actual
  checked callback delivering events for the client's XID, and require admission
  and cleanup. The preceding unpatched ABI 18 package deterministically stalls
  with zero checked events; the patched package passes. The link to each earlier
  uninstrumented timeout cannot be established retrospectively; this specific
  admission failure is reproduced and fixed.

## Capture, lock, and portal

- **Partial — capture/screenshot.** Legacy wlr-screencopy and shm pixel capture
  are implemented and headless pixel stacking/live-layer changes pass the latest
  integration check (`native/backend.c`, `tests/integration.lisp`).
  Software cursor selection now matches Rust's separate capture composition
  (`6de3ba6:crates/tomoe/src/capture.rs:85-108,316-320,360-364`).
  `native/space.c` prepares a cursor-free companion only for pending consumers,
  using the displayed buffer's scene, size and transform. The patched wlroots
  provider selects it by exact primary-buffer identity and releases its lock on
  every copy result; display cursor state is never hidden for a capture.
  Captures remain pending through output transactions, including partial backend
  failure and rollback, and resume from the accepted scene after publication.
  Region requests clip to physical output bounds after scaling and report
  capture-local damage. Companion buffers retire after the commit attempt;
  rejected candidates and cancelled clients retain no capture storage.
  `tests/screencopy.py` exercises real shared-memory clients, custom cursor pixels,
  cropped/damaged frames, fractional scale and negative output origins, mixed
  pending cursor choices, rollback and cancellation. Hardware DMA-BUF readback
  remains unverified. Other remaining behavior is the screenshot UI, modern ext-image-copy-capture path,
  and portal source selection. Rust has paced wlr-screencopy/ext-image-copy-
  capture, screenshot UI, and Lua actions/hooks:
  `6de3ba6:crates/tomoe/src/{capture,screenshot}.rs`,
  `6de3ba6:crates/tomoe/src/ui/screenshot_ui.rs`,
  `6de3ba6:docs/lua-api.md`.
- **Missing — lock/idle.** No lock state machine, locked backdrop/frame
  confirmation, idle notify, or inhibitor. Baseline:
  `6de3ba6:crates/tomoe/src/lock.rs`, `6de3ba6:crates/tomoe/src/state.rs`,
  `6de3ba6:crates/tomoe/src/handlers.rs`.
- **Missing — portal.** No portal crate/assets/PipeWire source selection.
  Baseline: `6de3ba6:crates/xdg-desktop-portal-tomoe/src/{main,outputs,screencast,pipewire_stream,toplevel_stream,toplevels}.rs`
  and `6de3ba6:resources/{tomoe.portal,tomoe-portals.conf}`.

## Control, IPC, policies, and services

- **Partial — private control.** Current mode-0600 `.ctl` socket supports
  framed safe Lisp `inspect/hit-test/reload/mount/unmount/command/event/quit`
  (`src/control.lisp`, `README.md`), but it is not Rust’s JSON
  wire contract.
- **Partial — JSON IPC.** Newline UTF-8 requests/replies, wire 2 built-in
  version/windows/outputs/view/subscribe/quit, exact filtered subscriptions,
  core lifecycle/focus/output/keyboard-activity events, and `tomoe msg` now exist.
  The mode-0600 socket is exported through `TOMOE_SOCKET`; persistent clients
  use nonblocking bounded queues, fair service quotas, and shutdown draining.
  `serve-state`/`serve-method` own endpoints, private `:ipc` reducer events
  produce post-commit replies/broadcasts, and `announce` owns continuous event
  snapshots. Later owners replace earlier ones; unmount/rule withdrawal restores
  the prior endpoint/value. Successful source replacement announces again;
  failed settlement/native preparation emits no proposed values or commands.
  Window focus reflects the real seat, including exclusive layers and unmanaged
  X11 focus. Geometry uses committed client size at the current placement/scale;
  xdg fullscreen/maximized flags reflect acknowledged state. One surface commit
  publishes size and state together, and size-only changes invalidate only the
  geometry dependency (`tests/window-geometry.py`, `tests/window-geometry.lisp`).
  Tests cover framing, Unicode, integer IDs, subscription replacement,
  slow peers, half-close, live-socket preservation, CLI discovery, ownership,
  rollback, WM and real window/rule lifetimes (`src/json.lisp`, `src/ipc.lisp`,
  `src/ipc-transport.lisp`, `tests/json.lisp`, `tests/ipc-transport.py`, `tests/ipc.py`).
  Remaining gaps: deferred screencast selection/portal hooks (currently fallback)
  and locked-state suppression of keyboard activity. Xdg registry identity and
  owned rule resources survive buffer detach/remap; native X11 still withdraws
  on unmap. Deliberate stricter limits bound copied JSON
  strings/nodes/nesting/numeric lexemes and reject duplicate nested request keys;
  slow peers cannot block policy. Baseline:
  `6de3ba6:crates/tomoe/src/ipc.rs`, `6de3ba6:crates/tomoe-ipc/src/lib.rs`,
  `6de3ba6:docs/lua-api.md`.
- **Partial — shipped WM.** The ordinary `wm` extension implements nine
  workspaces, eight-pixel dwindle splits, insertion-order switching/moving/cycling,
  rule admission, fullscreen output selection, client-fullscreen settings,
  explicit client state requests, activation reveal/focus, and reload retention
  (`builtins/desktop.lisp`, `6de3ba6:resources/wm.lua`). `drag` and `commands`
  remain independent owners. Owned `:wm-state` publishes workspace counts and
  JSON `wm_state` is served/announced through ordinary extension effects. The
  shell compositor service facade is still missing. Lisp gracefully hides windows while outputless and restores
  the active workspace on return. Xdg buffer detach now retains workspace
  assignments and admission state until role destruction.
  `tests/wm-native.lisp` covers real client placement, visibility, rule timing,
  state requests, activation, reload, and ownership withdrawal.
  One policy timing gap remains: a new fullscreen toggle masked by a later
  `fullscreen nil` owner can retain private fullscreen intent until the next
  non-toggle update, although the resolved native flag is already false.
  Previously active fullscreen intent is retired immediately by drag start.
- **Implemented — session notification producer and owned consumer.**
  `support/notifications.c` hosts the session D-Bus daemon independently of
  extension generations. `src/notifications.lisp` supplies copied `:services`
  facts through `service-state`; ordinary dependency settlement updates the
  shipped `notification-popups` shell consumer. It implements the four daemon
  methods, body capability, urgency, replacement ordering, expiry, and close
  signals. Existing bus owners remain untouched and setup failure is nonfatal.
  Unmount releases popup resources while daemon state survives; replacement
  consumers read retained facts. Failed consumers retain accepted effects and
  owe an update against current producer facts. Replacement expiry cancellation
  and fresh-ID collision avoidance deliberately fix defects in the baseline.
  `tests/notifications.py` uses a private session bus and real compositor to
  check daemon methods/signals, normal/urgent and multiline popup pixels, old expiry
  cancellation, default expiry, equal-query deduplication, consumer failure
  recovery, unmount/remount and source reload, existing-owner coexistence,
  record/text limits, replacement ordering and ID collision avoidance.
  `tests/notifications-policy.lisp` checks ordinary copied snapshots and shell
  ownership/layout without opening a bus (56 checks, including long text,
  64 outputs and high scales). `tests/notifications-helper.py` verifies absent
  and stalled buses, encoded runtime-path fallback, and disconnect cleanup.
  A real pure-backend smoke also passed notification admission, observer
  unmount/remount and accepted/rejected reload, explicit close, and session
  name/socket cleanup. The popup bounds preview text and
  visible card counts against physical canvas and copied-data budgets; full
  service records remain available. This is a deliberate bounded-display
  difference from Rust's unrestricted intrinsic popup tree.
  Method signature errors use the standard D-Bus `InvalidArgs` error; bounded
  field validation uses the daemon's `InvalidArgs` namespace. Unlike Rust's
  empty success for unknown methods, sd-bus provides standard method errors
  and introspection.
  Baseline: `6de3ba6:crates/moonshell-services/src/notifications.rs`,
  `6de3ba6:resources/moonshell/notifications.lua`.
- **Implemented — session MPRIS observer and owned media label.**
  `support/mpris.c` observes real session-bus players without claiming a name
  or sending playback controls. `src/mpris.lisp` publishes copied `:mpris`
  service facts independently of mounted consumers. Selection prefers playing
  players, then activity and name; metadata includes artist arrays, artwork
  URLs, length, volume and sampled position in seconds. Status/track changes
  request position and `Seeked` updates it; there is no periodic position tick.
  `examples/media.lisp` supplies an optional ordinary `media-label` surface.
  Its fixed 240-by-28 logical canvas bounds allocation across 64 scale-8 outputs;
  character truncation preserves Unicode. Unmount releases surfaces while
  the producer retains facts; replacement consumers seed from current facts.
  Bus failure clears the snapshot without restarting the observer.
  Discovery and property reads are asynchronous. Unique owner, property and
  read revisions fence old replies; aliases share a player lifetime. Replies
  do not make old observations outrank later activity on another player. These fences,
  bounded pending calls and property invalidation handling deliberately fix
  baseline races and indefinite retention of unanswered requests.
  `tests/mpris.py` exercises real players, metadata types and numeric bounds,
  activity selection, invalidated properties, delayed reads and owner changes,
  independent consumers, accepted/rejected reload and consumer recovery,
  timer/IPC progress, and actual label pixels and teardown. `tests/mpris-helper.py`
  covers missing/stalled/disconnected buses, escaped runtime paths, request
  deadlines, text/artist bounds, read ordering, and descriptor cleanup.
  The discovery probe forces one denied owner lookup; the previous package
  permanently misses that player, while the corrected producer retries it.
  Both stale-read regressions also fail against the preceding package: older
  metadata and position replies undo newer reads despite unchanged signal
  revisions. `tests/mpris-policy.lisp` checks copied facts and owned label
  plans (36 checks), including the maximum output/scale allocation bound.
  A pure-backend real-bus smoke passed preexisting discovery, UTF-8 metadata,
  seek updates, consumer replacement, owner removal, notification coexistence,
  and quit/socket cleanup.
  The Rust facade's playback methods were warn-once no-ops, so the Lisp service
  exposes read-only facts. Rust's default desktop also did not mount media UI.
  Baseline: `6de3ba6:crates/moonshell-services/src/mpris.rs`,
  `6de3ba6:resources/moonshell/services.lua:122-132`,
  `6de3ba6:resources/moonshell/widgets/mpris.lua`.
- **Implemented — session battery observer and owned indicator.**
  `support/battery.c` reads UPower's aggregate display device on the system bus
  and falls back to actual sysfs attributes. `src/battery.lisp` publishes copied
  `:battery` facts independently of consumer lifetime. Availability means a
  battery is present; absent defaults are `nil`, 100 percent and not charging.
  UPower percentages round and clamp to 0–100; only state 1 means charging.
  Missing or malformed sysfs capacity defaults to 100, and charging requires
  the trimmed, case-sensitive `Charging` value. The fallback chooses the first
  lexical device whose type is `Battery`, reads immediately, and polls every
  30 seconds while a battery exists. An initially empty fallback does not poll
  for future insertion, matching Rust. System-bus death closes that connection
  and continues the fallback without reconnecting.
  Owner and property/read revisions reject stale replies. Daemon replacement
  reseeds facts; invalidated properties trigger reads. Fallback polling rescans
  removed devices and clears unavailable facts instead of retaining a removed
  path indefinitely. These changes deliberately repair baseline lifetime gaps.
  Read errors preserve accepted facts and the verified owner; initial read
  failure activates sysfs while allowing later same-owner recovery. Separate
  source caches prevent partial UPower signals from inheriting sysfs facts.
  `examples/battery.lisp` supplies the optional `battery-indicator`: charging
  overrides the historical strict >80, >40 and >15 icon bands. The fixed
  112-by-28 logical canvas fits 64 scale-8 outputs; the default desktop does not
  mount it, matching Rust. Consumer removal releases only its surfaces.
  `tests/battery-helper.py` exercises real UPower activation, types, rounding, partial
  signals, invalidations, read/owner ordering, request expiry, missing/stalled
  buses, disconnect fallback and descriptor cleanup. The isolated clock probe
  exercises the real 30-second sysfs cadence, lexical selection, malformed and
  nonblocking attributes, directory replacement/removal and stop-on-absence.
  `tests/battery.py` exercises independent consumers, accepted/rejected reload,
  consumer failure recovery, timer/IPC progress, coexisting services, actual
  themed icon pixels and teardown. `tests/battery-policy.lisp` checks copied
  facts, thresholds, ownership and output/scale bounds (41 checks).
  Negative checks against earlier helpers reproduce initial-timeout failure,
  retired-owner timeout and older same-owner error rollback, sysfs `-0`
  misparsing, and mixed-source facts after fallback. The corrected helper
  passes those cases.
  The final packaged helper, 41 policy checks and real compositor/pixel fixture
  all pass in the full x86_64 Nix check alongside the existing service suites.
  A pure-backend real-bus smoke passed preexisting discovery, charge changes,
  independent consumers, accepted/rejected reload and consumer recovery,
  owner replacement, service coexistence and quit/socket cleanup.
  Baseline: `6de3ba6:crates/moonshell-services/src/battery.rs`,
  `6de3ba6:resources/moonshell/services.lua:87-98`,
  `6de3ba6:resources/moonshell/widgets/battery.lua`.
- **Implemented — session network observer and owned indicator.**
  `support/network.c` follows NetworkManager's root, primary active connection
  and `SpecificObject` access point on the system bus. `src/network.lisp`
  publishes copied `:network` facts independently of consumer lifetime, with
  disconnected defaults `nil`, `nil` and 0. State 50 or higher means connected,
  including local-only connectivity. The Rust Wi-Fi type prefix is preserved;
  wired connections have no SSID and strength 0, while hidden Wi-Fi SSIDs retain
  strength. The reported strength byte remains 0–255. SSIDs use Rust-compatible
  lossy UTF-8 decoding, including embedded NUL, and equal decoded facts do not
  advance the producer revision. Numeric Unicode scalar lists carry SSIDs
  through the native C-string boundary before the Lisp adapter makes strings.
  NetworkManager defines `SpecificObject` as an activation-time object; this
  preserves the actual Rust chain and does not add device-based roaming tracking.
  Owner, chain incarnation, per-property signal and read-order fences reject
  delayed replies across owner/path reuse. Invalidations withdraw unknown
  details immediately and request new reads; same-valued pointer signals fence
  reads without retiring live children. Read errors preserve accepted facts,
  while failed or malformed initial seeds use an independent sysfs cache and
  retain the verified owner for later signal/invalidation recovery. Fully
  rejected replies cannot displace fallback. Identical failed reads are not
  retried continuously. These changes repair baseline ordering/lifetime gaps.
  The fallback scans `/sys/class/net`, excludes exactly `lo`, and reports
  connected when any trimmed `operstate` equals `up`. It supplies no SSID and
  strength 0. Readable roots poll immediately and every 30 seconds even when
  initially empty; initially unreadable roots install no timer. Later root
  disappearance publishes disconnected while preserving the cadence. Owner
  loss starts fallback, and fatal bus loss closes the connection without
  reconnecting. Reads and scans are bounded and reject malformed, oversized or
  nonregular attributes, with sampled directory identity checked after reading.
  `examples/network.lisp` supplies the optional `network-indicator`, using the
  historical >=75, >=50 and >=25 Wi-Fi icon thresholds. A bounded label
  sanitizes control characters only for display. The fixed 240-by-28 logical
  canvas fits 64 scale-8 outputs; the default desktop does not mount it,
  matching Rust. Consumer removal releases only its owned surfaces.
  `tests/network-helper.py` covers real activation and D-Bus typing, raw strength,
  lossy UTF-8/NUL and revision equality, partial updates, overlapping reads,
  invalidations, owner/path reuse, spoofed/unselected signals, deadlines,
  fallback recovery, missing/stalled buses and descriptor cleanup. Its clock
  probe exercises the real 30-second sysfs cadence and directory lifecycle.
  `tests/network.py` covers independent consumers, accepted/rejected reload,
  failed consumer recovery, timer/IPC progress during held reads, coexisting
  services, exact themed icon pixels and teardown. `tests/network-policy.lisp`
  checks copied facts, icon thresholds, label bounds and ownership (48 checks).
  Earlier helpers reproduce activation/method-interface failures, lossy UTF-8
  corruption, stale-read authority and malformed initial-seed fallback failure.
  A held invalidation also reproduces delayed withdrawal of stale details.
  The final packaged helper, all 48 policy checks and the real compositor/pixel
  fixture pass in the complete x86_64 Nix check alongside prior service suites.
  A pure-backend real-bus smoke passes initial discovery, NUL and strength
  transport, state-only disconnect/reconnect with retained details, consumer
  replacement/reload, owner replacement, service coexistence and socket cleanup.
  Baseline: `6de3ba6:crates/moonshell-services/src/network.rs`,
  `6de3ba6:resources/moonshell/services.lua:100-110`,
  `6de3ba6:resources/moonshell/widgets/network.lua`.
- **Partial — session tray watcher and owned icon row.**
  `src/tray.lisp` integrates the session producer and publishes copied `:tray`
  facts, independently of consumer generation. The intended native contract in
  `support/tray.c` owns `org.kde.StatusNotifierWatcher`, acts as a host, and
  retains registration-ordered item metadata (`:service`, `:id`, `:title`,
  `:status`, `:icon-name`). An additive `:path` field distinguishes multiple
  objects registered by one application. Empty defaults are `(:items nil)`;
  there is no available flag or action facade. Missing buses and coexisting
  watchers leave empty facts without replacing, queueing or retrying ownership.
  Registration accepts the baseline name/path forms, resolves unique owners,
  and deduplicates exact service/path pairs. Owner transfer, removal and delayed
  replies require registration and per-property/read fences. The watcher adds
  typed discovery properties, introspection and correctly shaped signals to
  repair the Rust implementation's incomplete protocol responses.
  `examples/tray.lisp` supplies an optional bounded icon row; Rust had no shipped
  tray widget. The row preserves registration order and displays an overflow
  count while full service facts remain available. Consumer removal must
  release only its own surfaces/assets. Pixmaps, activation, scrolling and
  dbusmenu are absent from the Rust producer/facade and remain outside this
  service. The native helper, real-bus fixtures and final verification are in
  progress; this row is not yet an implementation-complete claim.
  Baseline: `6de3ba6:crates/moonshell-services/src/tray.rs`,
  `6de3ba6:resources/moonshell/services.lua:143-148`,
  `6de3ba6:crates/moonshell-runtime/src/services_bridge.rs:75-90`,
  `6de3ba6:crates/tomoe/src/main.rs:258-273`,
  `6de3ba6:crates/tomoe/src/state.rs:634-663,912-929`.
- **Missing — zoomer and screencast policies.** No infinite-camera `zoomer`
  or screencast policy. Baseline:
  `6de3ba6:resources/{zoomer,screencast}.lua`,
  `6de3ba6:crates/moonshell-services/src`.
  The fused entry point actually starts all five native producers
  (`6de3ba6:crates/tomoe/src/main.rs:219-268`); the old API document's placeholder
  claim is stale. Battery uses UPower with sysfs fallback, network uses
  NetworkManager with operstate fallback, MPRIS observes session-bus players,
  and notifications/tray host their session-bus services. Audio/sysinfo values
  are static defaults, and facade write methods are warn-once no-ops
  (`6de3ba6:resources/moonshell/services.lua:27-54,62-65,112-132`), so those
  nonexistent actions are not baseline behavior to reproduce.
  The Lisp `:audio` and `:sysinfo` service records now supply those same static
  volume/mute and CPU/memory defaults; they do not claim live hardware readings.
  Native producers belong to the compositor session; reload seeds replacement consumers from
  cached snapshots (`6de3ba6:crates/tomoe/src/state.rs:659-662`). Notifications,
  MPRIS, battery and network now follow this separation; tray integration is
  undergoing verification.

## Spatiotemporal composability

- **Implemented — policy data and admission proposals.** `publish-state` owns
  copied named values with later-owner precedence and omission/unmount restoration.
  `:admission t` reducers recalculate from transaction-start state and event after
  ordinary reducers and rules, replacing preliminary commands as well as effects,
  including private state and commands of surviving descendant rule scopes.
  `previous-context` exposes a copied stable basis for relative input actions.
  Refinement cannot permanently capture an early rule assignment, and cycles
  reject the complete proposal (`tests/published-state.lisp`). Callbacks remain
  trusted Lisp subject to the existing data/time budgets; this does not claim
  isolation from arbitrary global mutations in user code.

- **Implemented — grab lifetime reconciliation.** `resolved-grab` in
  `src/runtime.lisp` selects the latest contribution with a mapped target.
  Native unmap/destruction immediately releases capture; commit compares the
  actual native state with the eligible owner instead of trusting a Lisp cache.
  Real window/layer unmap, remap, destruction, conflicting owners, duplicate
  rejection, and reducer failure pass against both `:grab` and `:native-grab`
  (`tests/integration.lisp`). Physical input remains a separate unverified gate.
- **Implemented — failed external snapshot reconciliation.** `dispatch-event`
  preserves client/output facts and retains their invalidated context keys until
  a transaction commits. The finite pending set joins the next input or
  administrative transaction. Reducers use its current event and latest
  snapshot; there is no stale event queue or idle retry. Real map, metadata,
  repeated unmap/remap, source remount, sibling rollback, and command failures
  pass (`src/runtime.lisp`, `tests/integration.lisp`). Event-only transient
  actions, including acceptance of a failed client state request, are discarded;
  persistent adoption/removal must derive from snapshots. Shipped focus, float,
  workspaces, and deck recover adoption/removal while preserving the current
  input action (`tests/policy-recovery.lisp`).
- **Implemented — camera ownership.** The singleton view follows mount
  precedence, disappears on unmount, and rolls back with the candidate
  transaction. View changes rescale/redraw windows even if placement is
  unchanged. The native renderer and pointer hit test invert the same rounded
  rectangle, so a rounded-up edge does not become an unclickable strip
  (`native/space.c`, `tests/coordinates.lisp`).
- **Partial — native presentation transactions.** Policy transactions stage
  output boxes/scales, camera, window roots and stacking, and layer geometry
  without changing the live scene or sending client configures. Rendering reads
  that candidate when outputs change; backend success publishes its targets and
  layer overrides. Scene-only transactions publish the prepared scene directly,
  preserving current output facts without a backend configuration commit.
  Binding records and strings are allocated before publication; final grab
  ownership is installed against the settled geometry. Layer configures are
  deduplicated against the last sent logical size, including in-flight requests.
  A rejected backend commit restores previous output buffers with the unchanged
  live scene (`native/presentation.c`, `native/output.c`, `native/space.c`).
  `tests/commit-probe.c` observes successful configuration buffers directly;
  `tests/presentation.lisp` verifies candidate pixels and failure preservation.
  `tests/scene-publication.lisp` verifies settled scene-only configures and
  binding allocation failure/retry/unmount. The native ABI probe in
  `tests/empty-presentation.py` verifies camera publication and scratch cleanup
  when no outputs remain; its preceding ABI 10 build drops that publication.
  Unplaced mapped layers retain their facts and accept override publication or
  release while their scene roots remain hidden. `tests/layer-lifecycle.py`
  exercises real headless output loss and return, including removal of an
  override while no output exists, unchanged client configure counts, and
  suppression of exclusive keyboard focus while unplaced. Its preceding build
  drops the preview layer, rejects its candidate, and leaves its root hittable.
  Initial and remapping panels retain readiness after further bufferless
  requests while outputless. Output return configures the current request;
  unmap clears readiness before wlroots resets protocol state. The lifecycle
  probe verifies both cases with real clients; the preceding build stalls
  the deferred initial configure.
  Materialization reads newer committed
  output facts when their notifications are still queued, without changing
  output configuration or acknowledging consumer invalidations prematurely.
  Geometry still uses current client buffers while requests await acknowledgment.
  External lifetime events can lag native objects in the Lisp queue. Backend-initiated output
  changes, hardware/multiple-GPU rollback, and allocation failures after
  publication need further implementation and evidence. Detected publication
  failure stops the compositor; it is not a reversible transaction at that point.
- **Partial — SP-Temporal ceremony.** Current transactions preserve native facts
  while cancelling owner-scoped timers, file watches, services, and captured async executions
  on unmount; started session run-once leases remain until shutdown. Reload copies
  reducer state. The retained shell foundation now mounts real per-output
  textures and callbacks, reserves usable geometry, handles private clicks, and
  preserves the accepted generation through reducer/native rejection and failed
  reload. `tests/shell-ui.py` exercises rendering, input, cached reuse, successful
  reload and unmount with zero retained surface/texture/byte/callback/asset counts;
  `tests/ui-layout.lisp` covers the resource-free layout boundary, with additional
  native lifecycle checks in `tests/ui-native.py` and `tests/ui-assets-native.py`.
  `tests/watches.py` combines file observation, private state publication,
  dependent reducers, actual shell pixels, rollback, and reload with zero watch
  descriptors after unmount.
  The complete compound mount → exercise → unmount → reload/remount ceremony is
  still missing: shell service facades and the
  combined resource/queue/state-subscriber invariants below are not all present
  or proven together. Existing timer/service/async tests remain separate evidence.
  Authoritative baseline: `6de3ba6:crates/tomoe/src/sp_temporal.rs`.
  Its test queues a watch declaration, supplies no real service snapshots,
  manually fires timers, and fingerprints only display count (lines191-198,
  244-251,347-353). Completion here requires actual external service delivery
  and full topology/resource evidence, beyond that baseline test's coverage.
- Completion invariants: snapshot an empty/drained shell context and stable
  display fingerprint; mount adopts/renders all surfaces, reserves zones, and
  hit-tests clicks; timers/async exec/watches/state subscribers/keyboard/
  managed child services belong to the mounted extension/resources; session
  run-once children and external service producers have session lifetimes;
  unmount cancels owned queues/callbacks and leaks no globals; compositor
  display/window/service facts survive; reload/remount
  restores state and re-adopts the same topology; camera/animation/input/
  capture/output all share one coordinate path; any failure rolls back mounted
  resource ownership and leaves no native residue.

## Recommended completion gates

The coordinate gate requires a renderer migration, not a unit-name change.
`6de3ba6:crates/tomoe/src/{coords,space}.rs` keep physical window/output
positions separate from integer protocol rectangles. Configure sizes are
quantized as logical `round(physical / scale)` and achievable physical
`round(logical * scale)`; scale is snapped to 1/120. The camera uses
`screen = (world - offset) * zoom`, an integer offset, and zoom clamped to
1/16 through 16. Layers, outputs, and cursor stay fixed in screen space.

Both normal output frames and output-configuration buffers now use
`native/space.c` rather than scene-output rendering. wlroots scene trees retain
surface lifetime and stacking; protocol layout remains a separate boundary.
The custom path maintains output enter/leave, fractional-scale notifications,
frame callbacks, cursor rendering, and capture. Surface commits, including
desynchronized subsurfaces, invalidate the shared scene. Remaining validation
must cover windows spanning mixed-scale outputs, popup/subsurface trees,
output transforms, and hardware pointer/cursor behavior. The first policy
output-configuration buffer now renders the settled candidate scene, verified
at the actual backend commit. Scene-only transactions now share the publication
batch, and materialization adopts newer committed output facts before queued
notifications arrive. Broader external lifetime consistency, native allocation
recovery after publication, and hardware rollback remain composability gaps.

1. **Coordinate foundation:** physical world geometry, one canvas camera with output-aware mapping,
   inverse pointer mapping, usable areas, scale quantization; fractional-scale
   round-trip pixel/hit tests.
2. **Window behavior:** data snapshots/metadata, independent raise/properties,
   unmapped facts, hooks/rules, queued ops, request consumption, and ownership/
   reversion; map/unmap/reload/rule tests.
3. **Scene/render behavior:** camera transforms, clipping, border/shadow/blur,
   cursor/DnD, render-only animation, and tested damage/pixel stacking; nested/
   headless screenshots plus target-geometry invariants.
4. **Input/protocols:** physical keymap/device-settings verification, binding metadata/aliases, pointer hooks,
   constraints/relative pointer, DnD, selection/data-control, activation,
   decorations, foreign-toplevel, presentation/sync; protocol-client tests.
5. **Outputs/backends:** hardware mirroring/VRR,
   owned rotation, DRM/libseat/libinput,
   multi-GPU/direct scanout/gamma and rollback; real-hardware gate plus nested/
   headless coverage.
6. **Capture/security:** modern image-copy, screenshot UI, portal/PipeWire,
   session lock and idle; portal clients, locked-frame-per-output, and reload
   leakage tests. Legacy headless screencopy pixels are already verified.
7. **Control/process:** JSON IPC compatibility, subscriptions/endpoints,
   activation and session assets, and broader launch/process lifecycle coverage;
   CLI/stuck-client/restart/shutdown tests.
8. **Fused shell:** extend the retained shell foundation with shell services,
   then complete the compound ceremony above
   as a mount → exercise → unmount → reload/remount test with zero residue and
   identical topology.

Current evidence still leaves hardware DRM, physical keyboard/pointer/grab, native allocation recovery, hardware rotation, hardware mirroring/VRR, and several X11 physical input
paths unverified (`README.md`).
