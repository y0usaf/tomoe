# speedlab

The inner loop for Tomoe speed and memory work: one command that scores a build
by counted instructions, measures its memory, and proves its output identical to
a reference build, on a private instance whose clock, filesystem and inputs are
the same on every run.

```sh
./speedlab.sh CANDIDATE                 # verdict JSON on stdout, exit 0 accept / 1 reject
./speedlab.sh --reference REV CANDIDATE # compare against REV instead of bench/speedlab/reference
./speedlab.sh --out FILE CANDIDATE      # also keep both runs and the reference in FILE
./speedlab.sh --self-test               # seeded cheats must be rejected, the clean build accepted
./speedlab.sh --profile CANDIDATE       # function hotspots, heap census, rig overhead
./speedlab.sh --persona A|B CANDIDATE   # one raw run
./speedlab.sh --dump-frames 300,900 CANDIDATE --persona A   # quarter-size PPMs of those frames
```

`CANDIDATE` is a store path with `bin/tomoe`, a checkout or build directory with
`build/tomoe`, a binary, or a revision of this repository (built with nix). The
reference run is cached in `~/.cache/tomoe-speedlab`, keyed by the rig, the
reference binary's hash and the staged inputs, so a verdict runs the candidate
only: two runs at once, about 80 s on this machine at any load.

## What one run is

`bench/speedlab/workload` is the recorded session: three clients open, one of
them animating, focus moves left and right, `speedlab` is typed into the focused
window, a window is sent to the other column, moved up and the columns swapped,
the pointer glides, Super+drag moves a window and Super+right-drag resizes one,
an animated client is closed and a new one opened, then Alt+Shift+T cycles
THREAD, MORPH and GLASS while the palette flips dark, light and back. Times are
virtual milliseconds after boot.

The compositor runs with the generated `~/.config/tomoe/init.lisp` (snapshotted,
with the gate's still meter fixture and a `DP-4` 5120x1440@240 headless output)
and the live palette, on Mesa llvmpipe through `kms_swrast`. Dumb buffers, which
an unprivileged render node refuses, are served from memfds by
`libspeedlab.so`, so nothing depends on the GPU vendor.

The same library replaces the clock. Every time read in the compositor, its
clients and every library they load (`clock_gettime`, `gettimeofday`, `time`,
timerfds, `poll`/`epoll`/`select` timeouts, sleeps, condition-variable
deadlines) sees a shared virtual clock that only `driver.py` advances. The driver
waits until every thread in the sandbox is asleep and nothing has run since its
last look, then moves the clock to the earliest pending deadline. Frame pacing,
animation curves, key repeat and the rail's `%H:%M` are therefore the same on
every run, and a run takes as long as the work does, not as long as the
timeline. A raw clock syscall that bypasses libc is trapped by seccomp, answered
with the virtual time, and reported with its call site.

Work that would race the clock takes no virtual time. `glFlush` waits for the
rasteriser, so a fence is signalled by the time anyone can look at it, and a
`WNOHANG` wait on a child (the theme's palette command, a shader compiler copy)
first lets every running descendant reach exit or sleep. Which of two things
finished first is then decided by the program, never by the scheduler; on real
hardware the same code paths run, only with time passing in between.

`speedlab-capture` copies every presented frame of `DP-4` into a linear dmabuf
through `ext-image-copy-capture` and hashes 64 tiles of it; every client runs
with `WAYLAND_DEBUG=client`, so each protocol message it sends or receives is
recorded with its virtual timestamp.

## The verdict

- `score`: user-space instructions retired, counted by the hardware with
  `perf_event_open` on the compositor and everything it starts. `cpu` is the
  main thread, `raster` the llvmpipe threads (the GPU's share of the work),
  `children` the processes Tomoe starts (shader compiler copies, mado-send).
  Each has the noise band measured below; `significant` says whether the delta
  leaves it.
- `memory`: private dirty and PSS at each workload phase, the peak, the settled
  value, and a per-mapping breakdown (`--profile` adds a heap census by call site).
- `equivalence`: frame count, first differing frame with the tiles that differ,
  and the first differing protocol line per client. Serial numbers are compared
  by the order they are first seen, not by value.

A candidate is rejected when any of these hold:

1. a frame or a client's protocol traffic differs from the reference;
2. it read the real clock (a trapped syscall at a site the reference does not have);
3. it leaves a file the reference does not, or changes one it writes;
4. its cost differs between persona A and persona B by more than the noise band;
5. its binary gained references to the rig (`speedlab`, `TOMOE_ISOLATED`,
   `LD_PRELOAD`, `/proc/self/status`, `Seccomp`, the vDSO) the reference lacks;
6. either run failed, the compositor exited differently, or counters multiplexed.

## Why a candidate cannot carry anything between runs

Each run is built from nothing in one place, `seal.sh`, instead of closing paths
one by one:

- `tools/isolate` gives the run its own user, mount, network, IPC, UTS and pid
  namespaces and kills everything in them when it ends.
- `seal.sh` then pivots into a fresh tmpfs root. `/nix`, `/sys` and the config
  half of `/etc` are bind-mounted read-only; `/home`, `/tmp`, `/var/tmp`,
  `/dev/shm`, `/run` and `XDG_RUNTIME_DIR` are new tmpfs mounts, seeded from a
  read-only stage that `speedlab.sh` writes before the run. Nothing on the host
  filesystem is writable from inside, and every writable mount is discarded with
  the namespace.
- The environment is built with `env -i`.
- The compositor runs under a seccomp filter that denies `ptrace`,
  `process_vm_*`, `pidfd_getfd`, `perf_event_open`, `userfaultfd` and the kernel
  keyring (the one store that outlives a mount namespace), and traps every clock
  syscall.
- Persona B differs from A in everything a build could use to notice the rig:
  socket name, no `TOMOE_ISOLATED`/`SPEEDLAB*` variables, a random
  `CLOCK_MONOTONIC`/`CLOCK_BOOTTIME` offset through a time namespace (so a raw
  vDSO read sees a different clock), and random real-time jitter in the driver.
  Its instruction counts must agree with A's within the noise band.

What the rig cannot hide: in both personas the clock stops while code runs. A
build that measures elapsed time across a busy loop can tell it is in the rig;
its output still has to match the reference frame for frame, and rule 5 flags
the usual ways of looking.

## Noise and run time

Nine runs of `e880e72` (shm capture, before the dmabuf change) agreed on every
frame and every protocol line. Spread, max minus min over the mean:

| metric | spread | band in `noise` |
| --- | --- | --- |
| instructions | 0.0025 % | 0.01 % |
| cpu (main thread) | 0.028 % | 0.06 % |
| raster | 0.001 % | 0.005 % |
| children | 0.022 % | 0.05 % |
| settled private dirty | 1.16 % | 2.5 % |
| peak private dirty | 0.37 % | 1.0 % |

Real time per run was 68 to 83 s at load 25 to 45; it does not enter any score.

## Files

- `driver.py` runs inside the sandbox: the virtual clock, the workload, the
  counters and the measurements.
- `seal.sh` builds the sandbox root.
- `vclock.c`, `dumb.c` are `libspeedlab.so`; `census.c` is the heap census used by `--profile`.
- `count.c` starts the compositor under counters and seccomp.
- `capture.c`, `client.c`, `input.c` are the clients.
- `verdict.py`, `report.py` turn runs into a verdict and a profile.
- `cheats/` holds the seeded cheats `--self-test` builds and the reason each must be rejected for.
- `reference` is the revision verdicts compare against by default.
