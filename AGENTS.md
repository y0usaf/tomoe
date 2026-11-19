# Working here

## Landing a change

Everything lands on `main` through the `tomoe-land` queue in `tools/land.sh`,
which checks the exact commit it pushes:

```sh
tools/land.sh submit my-branch   # rebase onto origin/main, run the smoke check, push main
tools/land.sh wait ID            # block until that ticket has a result
tools/land.sh submit 0108501d    # a commit, when there is no branch name
```

The commit is rebased onto `origin/main`, `checks.<system>.smoke` runs on the
result, and only that commit is pushed, to `origin` and `forgejo`. If the rebase
conflicts or the smoke check fails, nothing is pushed. `submit` needs a queue,
which `tomoe-land` runs on this machine; `tools/land.sh daemon` starts one.

The smoke check is one VM, a few minutes: it builds the compositor, starts it,
draws windows, takes keyboard and pointer input, and survives a policy reload
(`integration`, `pointer-protocols` and `livepatch` in `tests/default.nix`).

Before a commit is pinned in finix, `tools/gate.sh REV` runs it with the config finix
generates against that exact build: a private headless instance at 5120x1440@240,
with test windows, dark then light, every rail theme, rapid focus switches and
moves while windows close and open mid-animation, a window of its own reversed
between the columns with Alt+[ and Alt+] faster than one move lasts while the row
it moves along is read between keystrokes, which must show it crossing back and
forth with no more than one read standing still, an Alt+G region screenshot with
a red arrow and a label drawn on it, frozen with the selector's own key and
undone twice so that only a freeze that put the marks in the picture can still
show them, whose copied image must carry the marks just as the screen showed
them, going back to selecting after a mark which must leave the window as it was
before it, a reload, and a broken config,
which must be refused without disturbing the running one. It fails on any
extension failure, duplicate timer, lost focus, unplaced window, the compositor
exiting or a fatal log line. It also runs a client that paints from its frame
callback and requires it to hold the refresh: 99% of the refreshes over a second and
a half, with both the median and the 99th interval inside one refresh. A config that
asks for a shader hold is timed from an input
to the hold an idle machine is given, with the config's meter readings held
still: the configured output must have drawn while the input was fresh, must
draw nothing over the next two seconds, and must resume on the next input
without skipping the picture it held. Once, from that rest, it wakes the machine
three times with a key, three times with a pointer motion and three times with a
client's new buffer, and fails if the frame carrying a pointer motion or a key
only started later than one and a half refreshes, if their median first frame
took longer than two refreshes, or if the worst of those six took longer than
three; a client's buffer is held to three and a half at the median, because the
frame that shows it also redraws what a paused desktop left cold. Then it streams
pointer motion, drags a window and runs a client that repaints on every frame
callback, a second and a half each, and fails unless the output kept presenting:
within one and a quarter refreshes at the median and three at the 99th for nine
tenths of the refreshes in the window, six tenths for the drag and for the
wallpaper after a key, and two and a half and four for two fifths for the client
that repaints every frame. Then it holds every frame the headless output commits on
a fence, one of the dependencies its render wait waits for, that signals five refreshes later (`TOMOE_TEST_FENCE` names a file holding its
milliseconds; the output records each held frame in that name plus `.log`), with a
client painting from its frame callback, and fails unless the client animated on its
own before the hold and no presentation feedback or frame callback reached it, or
reported a time, before the fence of the frame it describes signalled. Every run prints
the numbers, so a change is read against them rather than against a pass. Those
numbers only fail the run with `GATE_SPEED=1`, because they measure the machine
as much as the commit: a landing is decided by the behaviour checks above —
the start, the windows, input, the reload, the refused config, the pixels, the
idle frames and the resume — and a rate is a speed claim. Pin only a revision it passed, and deploy the pin
commit the gate ran (or gate that commit itself): `tomoe-land` does both.

`tools/contain.sh TOMOE CONFIG CLIENTS` runs that binary with that config on the same
kind of isolated instance and breaks things above the core on purpose: surfaces
past the canvas, a reducer that never returns or allocates without bound, one
that fails mid-animation or keeps event data, configs that loop or allocate, a
service and the config's own mado-send that segfault, a client that stops
reading, a pointer holding 41 buttons, and headset mode in and out while
surfaces churn. Each must leave the session answering, drawing on every output,
taking typed input, and a line in its log file naming what failed. `CLIENTS` is
the `bin` of `.#legacyPackages.x86_64-linux.client`.

A speed or memory claim starts with `bench/speedlab.sh CANDIDATE` (bench/speedlab/README.md):
the generated config, a recorded workload and a virtual clock on a sealed private
instance, scored by counted instructions and memory, rejected unless every frame
and every client protocol message matches `bench/speedlab/reference`. It runs in
about 85 s (counted instructions do not depend on the load); `--self-test` proves the seeded cheats are caught
and `--profile` maps where the instructions and the memory go.

Everything slower is for a speed claim, not for landing:

```sh
nix flake check            # every aggregate VM, ~30 min
nix build -L .#legacyPackages.x86_64-linux.suites.timing     # one suite, its own VM
nix build -L .#legacyPackages.x86_64-linux.suites.memory
nix build -L .#legacyPackages.x86_64-linux.suites.<name>     # any suite in tests/
```

Timing gates need a quiet machine, so run them alone.

## The VM lock

`$XDG_RUNTIME_DIR/agent/tomoe-vm.lock` is a shared/exclusive barrier on the one test
VM. Every build, eval or check on this machine takes a lease on it, Tomoe or not:
`flock -s` for builds and VMs that are not measuring anything, `flock -x` only for a
timing measurement or a benchmark.

Take a lease with `tools/lease.sh`, never with a descriptor of your own. `. tools/lease.sh`
gives you `lease_take LOCK -s|-x [SECONDS]` and `lease_release`; the lease is held in a
child of its own, so it goes away with the shell that took it. Open-coding it as
`exec 9>"$LOCK"; flock -s 9` leaks the lease twice over: every child of that shell keeps a
copy of descriptor 9, so a bot, a helper's shell or a nix build that daemonizes holds the
VM long after the work has ended, and an omp bash tool runs its commands in the
assistant's own long-lived process, where the descriptor outlives the script that opened
it. `tests/lease.sh` is the regression test for exactly this.

`flock` lets shared holders starve a waiting exclusive gate, so the lock also has a
queue marker: before asking for `flock -x`, create
`$XDG_RUNTIME_DIR/agent/tomoe-vm.pending/<your-agent-name>` and remove it as soon as
the lock is granted (and on any exit); before each new `flock -s`, wait until that
directory is empty — for at most `LAND_PENDING_WAIT` (10 minutes), because a steady
stream of exclusive gates must not starve shared work forever. `tools/land.sh` does all of
this for you.

## Never touch the live desktop

The session you are working in is someone's desktop. Run a private Tomoe, a test
client, Xwayland, xeyes or anything that injects input only through
`tools/isolate DIR COMMAND` (the gate does): it hides the live runtime directory,
X11 sockets and network, removes `/dev/uinput`, `/dev/uhid` and `/dev/input`, makes
your real home read-only and kills everything when the command ends. It refuses
to start while `DISPLAY`, `WAYLAND_DISPLAY`, `WAYLAND_SOCKET` or `TOMOE_SOCKET` is
set, so start it as `env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u
TOMOE_SOCKET tools/isolate ...`, and while a fake kernel input device exists.
Never use kernel fake input devices, `DISPLAY=:0` or the live sockets; Tomoe's
own `tomoe-pointer-script` and `tomoe event` inject input into one instance.

## The core and a running session

`make core` builds `build/tomoe-core.so` from the same sources, and
`tomoe command swap-core file=PATH` queues it: the driver checks the image in
that call (layout through `tomoe_core_abi`, function set, data sizes, every entry
patchable) and commits at the next frame boundary, where no call of the running
image is on the stack. The commit copies the writable data into the new image,
normalises every table pointer the heap holds (`wl_resource_instance_of` compares
implementations, `surface->role` decides the role, `screen->impl` the backend),
and patches the running image's function entries to the new image's bodies
through trampolines within reach of that image. Both images are built with
`-fpatchable-function-entry=5,0` and `tools/entries` widens the five one-byte
nops into one five-byte nop after each link, so an entry is one eight-byte store
while other threads run. The driver's own functions (`main.c`, listed by `nm` into
`build/main-functions.h`) and the loader's (`livecore_*`) are never patched.

Everything that can fail happens in the call that asks for the swap: the
`dlopen`, the symbol pairing, the writable protections on the data and code
pages, and every patch record and trampoline are built and checked there, and a
failure refuses the swap with the running core and its clients untouched. The
commit that runs at the frame boundary only writes bytes that are already known
to fit. `--test-swap-fail=FILE` injects the next attempt's failure - FILE holds
`protect` or `alloc` and is removed - and the livecore suite uses it to check
that a refused swap leaves the old core drawing and taking input.

A static object whose *address* another object remembers has to be shared, not
copied: `native/state.c` is retained state the driver owns by name, each image
points its statics at it when loaded through `tomoe_core_attach`, and both then
address the same object. That covers the rect pool (`region.c`) and the Lisp
runtime with its symbol table (`lib/lisp/heap.c`, which `state.c` hands `tomoe_state`
through `lisp_attach`). Everything else is a copy per image,
which is why the commit has to normalise the table pointers above and why
`selection.c` compares a `wl_interface` by name rather than by address; a new
static whose address is compared across images will break a swap.

`nix build .#legacyPackages.x86_64-linux.suites.livecore`, and the smoke check,
swap a core in and out of a session with three drawing clients and assert it from
both ends. The clients themselves report what they received: the focused window
has to log a key event and a pointer motion after each swap, after a refused swap
and after the second swap, while the session's own binding has to fire for the
same key press. On the session's side the windows stay mapped, keep committing
frames, a client that connects after a swap maps, a client that dies after one or
two swaps leaves the session drawing and answering, a second swap returns to the
first core, and a core built for another layout is refused with the session still
running.

All of that input comes from one `tomoe-pointer-script` for the whole run, driven
by `wait`/`signal` files. Starting a second one flaps the seat's capabilities
(each virtual keyboard device comes and goes), and a client that then asks for a
new keyboard is handed one that never gets its keymap, so its keys stop arriving:
a pre-existing seat bug the test does not trip over because the device it types
with is the same for the whole run.

## The keyboard

`lib/xkb/` is Tomoe's own XKB: rules, keymap compiler, state machine, keysyms
and the keymap text clients get, with libxkbcommon's API under a `txkb_` prefix.
`tests/xkb-trace.c` drives it and libxkbcommon 1.13 through the same trace for
every model, layout, variant and option in xkeyboard-config (`tests/xkb-cases.awk`)
and the whole keysym table: each keymap's keysyms per level, repeat flags and names,
every key tapped under every set of held Shift, Control, Alt, AltGr and Super with
Caps and Num Lock, and 3000 seeded presses and releases of modifier, latch, lock
and ordinary keys whose holds overlap and end in any order. A second pair replays
the same events on libxkbcommon's state and reads them back, as a client does,
through the keymap text each side sends. `xkb-equivalence` fails on any difference
and prints the first diverging lines; it is one of the compositor VMs' packages,
so the smoke check rebuilds it whenever those files change.

## Releasing

`nix run .#release -- rc|soak|promote` (README, Releases). A candidate is a commit
on `main`; it is soaked with its own `gate.sh` for 30 minutes and promoted on the
same commit, which also moves `tomoe-stable` in finix to it and the finix revision
it soaked with. Try the whole path with `--dry-run` and a short `RELEASE_SOAK`.

## Rules

- Gate the exact commit you push. Never push a commit that was not the one checked.
- `origin` and `forgejo` both get `main`; delete your branch once it lands
  (`tools/land.sh --push` does that).
- If something breaks on `main`, revert it rather than leaving it red.
- Update `AGENTS.md` and the README when behaviour changes; this file is short on
  purpose.
