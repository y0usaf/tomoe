# Session services

Tomoe runs the small daemons a desktop needs, a notification server, media
player tracking, battery, backlight, sounds, network and a tray host, and
hands their state to extensions.

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
events. The `(media-control ACTION)` command, with `:play-pause`, `:next` or
`:previous`, sends that MPRIS call to the selected player and does nothing
when there is none.

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
polling when no battery remains. An initially empty fallback does
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
lists allow 64 entries. Sysfs attributes are limited to 256 bytes and scans
to 256 directory entries per attempt, with one retry if the selected directory
changes during a sample. Malformed and nonfinite UPower values leave facts intact.

## Backlight

`(adjust-brightness PERCENT)` is a command that steps the first backlight under
`/sys/class/backlight`, in name order, by PERCENT of its maximum (negative
steps down), clamped to the device's range. It asks logind's
`Session.SetBrightness` on the system bus to write the value, so the
compositor needs no write access to sysfs. A missing backlight or a refused
call is reported as the last error. `TOMOE_BACKLIGHT_ROOT` selects an alternate
backlight class directory.

## Sounds

`(sound EVENT FILES &key gain)` owns the sound for one event. `:key` plays on
every key press the seat receives and `:button` on every pointer button press,
bound or not, including while the session is locked or a screenshot is being
taken. `:open` plays when a window is first admitted and `:close` when its
lifetime ends, the same moments as `window_open` and `window_close`. FILES is a
WAV path or a list of up to 16, played in turn. Each must be 16-bit PCM or
32-bit float, mono or stereo, at 48000 Hz; relative paths resolve beside the
declaring source. GAIN is in decibels, from -60 to 12. Neither the key nor the
button reaches policy.

```lisp
(define-extension "sounds" () (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (sound :key '("click-1.wav" "click-2.wav" "click-3.wav") :gain -18)
                (sound :button "select.wav" :gain -15)
                (sound :open "open.wav" :gain -6)
                (sound :close "close.wav" :gain -6))
          nil))
```

Files load when the declaration changes; an unreadable or unsupported file
fails the transaction with its reason and keeps the previous sounds. The
latest owner of an event wins, and omission restores the preceding owner.
While any sound is declared, the compositor holds one PipeWire playback stream
open at a 256-frame quantum, so the audio device stays awake; decoded samples
stay in memory and the input path only queues them, lock-free, for the stream's
realtime thread. The stream is set up and connected on its own thread, which
retries once a second while PipeWire is away; events while the stream is not
running play nothing and are not queued. Withdrawing the last sound disconnects
and joins that thread. `inspect` reports `:sounds` and `:native-sound`, whose
`:state` is `:off`, `:connecting`, `:streaming`, `:retrying` or `:failed`, with
the last `:error` and counts of `:played` and `:dropped` sounds.

## Network

Declare `:reads (:services)` and call `(service-state snapshot :network)` for
`(:connected BOOL :ssid STRING-OR-NIL :strength INTEGER)`. Disconnected defaults
are `nil`, `nil` and 0. The producer belongs to the compositor session, including
under `--bare`; consumer removal or replacement leaves current facts available.

The system-bus observer follows NetworkManager's primary active connection and
its access point. State 50 and above means connected, including local-only
connectivity. Wired connections have no SSID and strength 0. Hidden Wi-Fi SSIDs
also report `nil` but retain the access point's strength. Strength preserves the
reported byte, normally 0–100. SSID byte arrays use lossy UTF-8 decoding;
embedded NUL remains part of the Lisp string and crosses JSON IPC as an escape.

Connection and access-point changes withdraw downstream details immediately.
Owner, path-lifetime and property/read revisions fence delayed replies;
invalidated properties trigger fresh reads. Failed reads retain accepted facts,
while an unsuccessful initial root read activates the independent sysfs fallback.
Daemon replacement clears the old chain and seeds from its new owner.

The observed access point comes from the active connection's
[`SpecificObject`](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.Connection.Active.html).
NetworkManager defines this as the object used during activation;
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
The service/path pair distinguishes multiple items from the same application.
Status remains the item's
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
It reports itself as a host and protocol version 0.
Fatal bus loss withdraws all items and closes the producer; it does not reconnect
or retry acquiring the watcher name.

Mount `examples/tray.lisp` for an ordinary owned icon row. The service preserves
all registered items while the row bounds its display. Observation
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
