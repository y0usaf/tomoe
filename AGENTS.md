# Working here

## Landing a change

Tomoe has no test suite: no VM suites, golden files, unit tests or test clients,
and none are added. `nix flake check` builds the package and nothing else. A
change is checked when it is made, and the check is thrown away:

- a rewrite, port or cleanup meant to change nothing: build the previous commit
  and this one and compare what they do (the bytes, the frames, the protocol
  messages or the Lisp results), as SpeedLab does;
- code that parses what a client or a file hands it: a sanitizer or fuzz run;
- anything visible: a private instance through `tools/isolate`, then the desktop.

If something breaks, fix it or revert it.

Everything lands on `main` through `tomoe-land`, the queue finix runs on this
machine: `tomoe-land submit BRANCH` rebases it onto `origin/main`, builds `.#tomoe`,
pushes that exact commit to `origin` and `forgejo`, pins it in finix and queues a
switch. `tomoe-land wait ID` blocks until that ticket has a result.

A speed or memory claim starts with `bench/speedlab.sh CANDIDATE` (bench/speedlab/README.md):
the generated config, a recorded workload and a virtual clock on a sealed private
instance, scored by counted instructions and memory, rejected unless every frame
and every client protocol message matches `bench/speedlab/reference`. It runs in
about 85 s (counted instructions do not depend on the load); `--self-test` proves the seeded cheats are caught
and `--profile` maps where the instructions and the memory go. `nix run .#bench`
compares Tomoe with sway, niri and Hyprland on one vkms output; run it alone.

## The VM lock

`$XDG_RUNTIME_DIR/agent/tomoe-vm.lock` is a shared/exclusive barrier on the one benchmark
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
it.

`flock` lets shared holders starve a waiting exclusive holder, so the lock also has a
queue marker: before asking for `flock -x`, create
`$XDG_RUNTIME_DIR/agent/tomoe-vm.pending/<your-agent-name>` and remove it as soon as
the lock is granted (and on any exit); before each new `flock -s`, wait until that
directory is empty — for at most `LAND_PENDING_WAIT` (10 minutes), because a steady
stream of exclusive holders must not starve shared work forever. `tomoe-land` does all of
this for you.

## Never touch the live desktop

The session you are working in is someone's desktop. Run a private Tomoe, a
client, Xwayland, xeyes or anything that injects input only through
`tools/isolate DIR COMMAND`: it hides the live runtime directory,
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
`protect` or `alloc` and is removed - so a refused swap can be tried on a
private instance.

A static object whose *address* another object remembers has to be shared, not
copied: `native/state.c` is retained state the driver owns by name, each image
points its statics at it when loaded through `tomoe_core_attach`, and both then
address the same object. That covers the rect pool (`region.c`) and the Lisp
runtime with its symbol table (`lib/lisp/heap.c`, which `state.c` hands `tomoe_state`
through `lisp_attach`). Everything else is a copy per image,
which is why the commit has to normalise the table pointers above and why
`selection.c` compares a `wl_interface` by name rather than by address; a new
static whose address is compared across images will break a swap.

A virtual keyboard that comes and goes flaps the seat's capabilities, and a client
that then asks for a new keyboard is handed one that never gets its keymap, so its
keys stop arriving: a known seat bug. Inject input from one long-lived
`tomoe event` or virtual-keyboard client per run.

## The keyboard

`lib/xkb/` is Tomoe's own XKB: rules, keymap compiler, state machine, keysyms
and the keymap text clients get, with libxkbcommon's API under a `txkb_` prefix.
The keymap text declares keycodes from 8 to at least 255, as libxkbcommon's does,
because Xwayland adopts that range and X11 clients read the core mapping from
keycode 8. A change here is checked against libxkbcommon 1.13 on the same layouts
and key sequences before it lands.

## Rules

- Push the exact commit you built and checked.
- `origin` and `forgejo` both get `main`; delete your branch once it lands.
- If something breaks on `main`, revert it rather than leaving it broken.
- Update `AGENTS.md` and the README when behaviour changes; this file is short on
  purpose.
