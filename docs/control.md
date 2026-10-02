# Control and IPC

Drive a running Tomoe from a shell or a program: the `tomoe` subcommands, the
JSON IPC that bars and scripts speak, and the control protocol under both.

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
extension failures, and the last error. Its `:x-display` and `:session-bus` fields
are the `DISPLAY` and `DBUS_SESSION_BUS_ADDRESS` this instance exported. Mutating
commands print nothing on success and return a nonzero exit status on failure.
`command OWNER NAME` invokes an active binding through the same extension
dispatch as keyboard input.
It refuses a name bound only by `bind-button` or `bind-scroll`, whose events
carry pointer data it cannot supply; send those with `event`.

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

## JSON IPC

`tomoe msg METHOD [JSON]` speaks JSON wire version 2 over the persistent
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
keys are rejected on server requests, including nested params.

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

Requests are `(1 :inspect)`, `(1 :hit-test X Y)`, `(1 :frames)`, `(1 :memory)`, `(1 :reload)`,
`(1 :mount "path")`, `(1 :unmount "name")`, `(1 :command "owner" "command")`,
`(1 :event "PLIST")`, or `(1 :quit)`. `:event` carries one string holding a data property list whose
`:type` must be `:key`, `:button`, or `:grab`; the compositor reads it and
dispatches it like a real input event. `:memory`, or CLI `memory`, reports the
Lisp heap's `:dynamic-usage` and `:dynamic-space-size`, `:bytes-consed`,
`:bytes-consed-between-gcs`, the `:gc-count`, total, longest and CPU
microseconds of collection, the C heap's `:heap-used` and `:heap-held` bytes,
the renderer's own `:textures` and `:texture-bytes`, its dmabuf `:images`, the
live `:surfaces`, and each output's allocated `:buffers` and `:buffer-bytes`.
Replies are `(1 :ok result)` or
`(1 :error "message")`. Version 1 is exact; unknown versions fail explicitly.
Reader evaluation and dispatch syntax such as `#.` and circular object labels
are disabled, and the reader accepts exactly the data the printer emits,
including the cons dot an extension's own state may contain. This is a data
protocol, not an unauthenticated REPL.
