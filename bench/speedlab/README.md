# speedlab

The inner loop for Tomoe speed and memory work: one command that scores a build
by counted instructions, measures its memory, and proves its output identical to
a reference build, on a private instance whose clock, filesystem and inputs are
the same on every run.

```sh
bench/speedlab.sh CANDIDATE                    # verdict JSON on stdout, exit 0 accept / 1 reject
bench/speedlab.sh --reference REV CANDIDATE    # compare against REV instead of bench/speedlab/reference
bench/speedlab.sh --compare BASE CANDIDATE     # the same, for a climb: is CANDIDATE a win over BASE?
bench/speedlab.sh --out FILE CANDIDATE         # also keep both runs and the reference in FILE
bench/speedlab.sh --self-test                  # seeded cheats must be rejected, the clean build accepted
bench/speedlab.sh --profile CANDIDATE          # function hotspots, heap census, rig overhead
bench/speedlab.sh --persona A|B CANDIDATE      # one raw run
bench/speedlab.sh --dump-frames 300,900 CANDIDATE --persona A   # quarter-size PPMs of those frames
```

`CANDIDATE` is a store path with `bin/tomoe`, a checkout or build directory with
`build/tomoe`, a binary, or a revision of this repository (built with nix). The
reference run is cached in `~/.cache/tomoe-speedlab`, keyed by the rig, the
reference binary's hash, the staged inputs and the number of runs, so a verdict
reruns the candidate only: two runs at once, about 85 s on this machine at load
25, and the base's three side by side the first time a base is judged.

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
deadlines) sees a shared virtual clock that only `driver.c` advances. The driver
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
hardware the same code paths run, only with time passing in between. In the
same way, whenever the compositor is about to read its event loop or poll a
socket, it asks the driver to let every other task in the sandbox reach sleep
first, so a burst of client messages always arrives as one batch.

`speedlab-capture` copies every presented frame of `DP-4` into a linear dmabuf
through `ext-image-copy-capture` and hashes 64 tiles of it; every client runs
with `WAYLAND_DEBUG=client`, so each protocol message it sends or receives is
recorded with its virtual timestamp.

## The verdict

The verdict names its `base` (the reference, or `BASE` under `--compare`) and
its `candidate`, and `outcome` sums it up: `win` when at least one of cpu,
raster, children, settled or peak private dirty fell by more than its noise band
and none rose by more than its band, `loss` the other way round, `mixed` when
both happened, `neutral` when nothing left its band, and `reject` when any of
the rules below fired. A climb accepts a `win`, makes it the next `BASE`, and
keeps going. The base is run three times and the verdict scores against their
mean, so one base measurement cannot decide a climb; those runs are cached by the
base binary's hash, so they cost nothing the second time.

- `score`: user-space instructions retired, counted by the hardware with
  `perf_event_open` on the compositor and everything it starts. `cpu` is the
  main thread, `raster` the llvmpipe threads (the GPU's share of the work),
  `children` the processes Tomoe starts (shader compiler copies, mado-send).
  Each has the noise band measured below; `significant` says whether the delta
  leaves it, and `reference` is the mean of the base's three runs.
- `memory`: private dirty and PSS at each workload phase, the peak, the settled
  value, and a per-mapping breakdown (`--profile` adds a heap census by call site).
- `equivalence`: frame count, first differing frame with the tiles that differ,
  and the first differing protocol line per client. Serial numbers are compared
  by the order they are first seen, not by value.

A candidate is rejected when any of these hold:

1. a frame or a client's protocol traffic differs from the reference;
2. the base's three runs do not agree with each other, so there is nothing to compare to;
3. it read the real clock (a trapped syscall at a site the reference does not have);
4. it leaves a file the reference does not, or changes one it writes;
5. it sees the rig: its output differs between persona A and persona B, or any
   of its four costs differs between them by more than the noise band;
6. its binary gained references to the rig (`speedlab`, `TOMOE_ISOLATED`,
   `LD_PRELOAD`, `/proc/self/status`, `Seccomp`, the vDSO) the reference lacks;
7. either run failed, the compositor exited differently, or counters multiplexed.

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

## Self-test

`--self-test` builds the clean tree and each patch in `cheats/` with nix, runs
every build through a full verdict against the clean build, and fails unless the
clean build is accepted and each cheat is rejected with the reason `cheats/expect`
names for it:

| cheat | what it does | rejected because |
| --- | --- | --- |
| `pixel` | draws one magenta pixel at (4001, 701) | output frame 0 differs, in the one tile holding that pixel |
| `event` | sends `wl_keyboard.modifiers` before `enter` | protocol traffic differs at the first focus change |
| `clock` | reads `CLOCK_MONOTONIC` through a raw syscall | it read the real clock, at its call site |
| `cache` | writes `/tmp/.tomoe-frame-cache` and `~/.cache/tomoe/frames.bin` at exit and changes course when they exist at start | it leaves state the reference does not; the files never survive to the next run |
| `detect` | when `TOMOE_ISOLATED` is set (spelled so no marker matches) copies captured frames with `glBlitFramebuffer` instead of a textured pass | it sees the rig: persona A's cost or output differs from persona B's |

## Noise and run time

Repeated runs agreed on every frame (1801 at 5120x1440) and every protocol
line: eight of `e880e72` four at a time at load 25 to 50, and four of the
current reference `89ad344` two at a time. Spread is max minus min over the
mean; the band in `noise` is what a verdict calls significant and what persona A
and B may differ by.

| metric | e880e72 (8) | 89ad344 (4) | band |
| --- | --- | --- | --- |
| instructions (everything) | 0.0018 % | 0.0009 % | 0.005 % |
| cpu (compositor main thread, ~58 G) | 0.137 % | 0.063 % | 0.2 % |
| raster (llvmpipe threads, ~4.88 T) | 0.0012 % | 0.0001 % | 0.005 % |
| children (shader compiler copies, mado-send) | 0.023 % | 0.022 % | 0.05 % |
| settled private dirty (~276 MB) | 0.50 % | 1.39 % | 2.5 % |
| peak private dirty (~553 MB) | 0.21 % | 0.07 % | 3.0 % |

Peak private dirty is the one metric whose agreement does not survive a change
of session. Ten alternating runs of `9fd1d47` and `faecc51` at load 16 to 26
peaked at a 0.97 % and a 0.78 % standard deviation, a 1.91 % and a 1.67 %
spread, and each build's one cached run sat 1.2 % off that build's own five-run
mean, so inside the old 0.5 % band the same build scored both a win and a loss.
Its band is three times the worse deviation, 3.0 %, and the base is the mean of
three runs. CPU on the same runs deviated 0.010 to 0.018 %, well inside its
0.2 % band. The bands live in `noise` alone; `SPEEDLAB_BASE_RUNS` sets the run
count, which a verdict refuses to score below three.

A verdict with a cached reference runs the two personas side by side: 87 s
measured at load 25, 130 to 200 s with four other runs alongside; a base that is
not cached adds its three runs, side by side, before those. Real time never
enters a score.

## What the rig costs, and what it changes

- Capturing every frame into a dmabuf costs 0.5 G raster instructions a frame
  (900 G of the 4.88 T) and 0.7 G on the main thread; it is the same for every
  build. `--profile` measures it with a run that captures nothing.
- GPU work and child processes take no virtual time (see above), so a shader
  copy has always finished when Tomoe first looks, and fallback frames never
  appear. The shader cache starts empty on every run, so each theme's shaders
  are compiled cold.
- llvmpipe keeps textures in process memory, so memory figures include what
  would be VRAM on a GPU; the census attributes it by Tomoe call site.

## Profile of e880e72

`bench/speedlab.sh --profile e880e72` (`profile-e880e72.json` beside this file):
8.15 virtual seconds, 1801 frames.

Where the instructions go: raster 3.98 T without the capture (98 % of
everything), the main thread 57.4 G, shader compiler copies 31 G. On the main
thread, `libLLVM` (llvmpipe compiling shader variants at draw time) is 66 %,
`libgallium` 16 %, libc 8.6 %, Tomoe's own code 7.6 % (4.4 G). Inclusive,
`effect_shader` is at least 12.5 % of the main thread, `draw_scene` 9.6 %, the
Lisp policy (`call_with`, `run_let`, `loop_eval`) about 4.6 %, `ui_render` 4.1 %,
`effect_blur` 3.9 %, `decorate` 3.8 % (DWARF unwinding stops early on deep
stacks, so these are lower bounds). The hottest Tomoe functions by self cost are
the Lisp heap and copier: `heap_alloc_in` 270 M, `deep_copy` 240 M plus
`copy_walk` 90 M, `call_with` 230 M, then `pass_record` 200 M and
`region_combine` 140 M. libc on the main thread: `memcmp` 1.23 G, `memmove`
1.01 G, malloc and free 0.9 G, `qsort` 0.35 G.

Where the memory goes in the rig: 276 MB private dirty settled, 553 MB at the
peak (the pointer phase, while windows are dragged and resized). Settled: 185 MB
of live malloc, of which blur chain levels (`effects.c` `level_ensure`) 64 MB,
other textures (`texture_new`) 43 MB, Mesa's own 51 MB, and Tomoe's own heap
10 MB (4 MB from `tomoe_create`, 2.6 MB Lisp attach); the three 5120x1440 scanout
buffers 86 MB; LLVM's code 65 MB of shared clean pages. At the peak the blur
levels alone are 356 MB.

The running desktop (read-only `smaps` of the live Tomoe on the RTX 4090):
105 MB RSS, 64 MB PSS, 42 MB private dirty. The NVIDIA user-space driver is 62
MB of the RSS (`libnvidia-gpucomp` 28 MB shared clean, `/dev/nvidiactl` mappings
21 MB of which 15 MB private dirty, `libnvidia-eglcore` 10 MB, `libcuda` 2.7 MB,
NVML 1.3 MB); Tomoe's anonymous memory 16 MB and brk heap 8.6 MB.

### Targets, ranked by expected gain

1. Wallpaper-only ticks recomposite the whole output. The llama wallpaper is
   `:fps 0`, so it advances on every 240 Hz refresh, and a frame where only the
   wallpaper moved is composited from scratch like any other. Measured with the rig:
   the same config with the wallpaper at 30 fps drew 789 frames instead of 1801
   and cut raster from 3.98 T to about 1.34 T (-66 %, capture overhead removed)
   and main-thread instructions from 58.0 G to 51.1 G (-12 %): 2.6 G raster
   instructions per wallpaper-only frame, more than the 2.2 G average frame.
   Reusing the composited layers above
   the wallpaper when only the wallpaper changed (or drawing it only where it is
   uncovered) keeps the output identical and should recover most of that.
2. Blur chain levels. `level_ensure` holds 356 MB at the drag peak and 64 MB
   settled, and the peak private dirty (553 MB) is double the settled (276 MB).
   Chains are kept per key and their levels re-specified whenever the grown
   size changes (`effects.c`), so a drag or resize leaves a trail of full-size
   levels until they expire. Reusing levels by size
   class and releasing idle chains should take the peak down by most of 290 MB
   and the settled by up to 64 MB (VRAM on a GPU).
3. Shader variants compiled at draw time. LLVM is 38 G of the 57 G
   main-thread instructions, under `effect_shader` and the scene draw, plus 31 G
   in the compiler copies, mostly at theme switches. Fewer distinct programs and
   pipeline states per theme (one effect program with uniforms instead of one
   per effect) would cut both; up to 66 % of the main thread on a cold cache.
4. The Lisp policy's allocation and copying: `deep_copy` and `copy_walk` 330 M,
   `heap_alloc_in` 270 M, `cons` 100 M, plist lookups (`keyword_get`) 130 M, out
   of Tomoe's 4.4 G. Keeping extension state in its region instead of copying it
   per dispatch is worth roughly 0.6 G (1 % of the main thread, 14 % of Tomoe's
   own code).
5. Per-draw overhead in Mesa. `libgallium` is 9.2 G of the main thread (16 %),
   below `_mesa_draw_arrays`/`llvmpipe_draw_vbo` (14.6 % inclusive, which also
   holds the variant compiles of target 3), reached from Tomoe through
   `render_quad`/`pass_quad` (7 % inclusive): many small quads, each a draw
   with its own state. Batching quads that share a program into one draw cuts
   the per-draw setup; the measured ceiling is the 9.2 G. libc's `memcmp`
   (1.23 G) and `memmove` (1.01 G) are next, but their callers were not
   attributed: `render.c` compares scene runs with `memcmp` and `region.c` sorts
   rectangles with `qsort` (0.35 G), and LLVM uses both, so they need a call
   graph that unwinds further before they are a target.

Outside the rig's reach: on the desktop `libnvidia-ml` (1.3 MB RSS, which
`support/system.c` dlopens for the GPU meter) and `libcuda` (2.7 MB) are mapped
into the compositor; moving the GPU probe into a process of its own would take
out at least the first.

## Files

- `driver.c` runs inside the sandbox: the virtual clock, the workload, the
  counters and the measurements.
- `seal.sh` builds the sandbox root.
- `vclock.c`, `dumb.c` are `libspeedlab.so`; `census.c` is the heap census used by `--profile`.
- `count.c` starts the compositor under counters and seccomp.
- `capture.c`, `client.c`, `input.c` are the clients.
- `verdict.c`, `report.c` turn runs into a verdict and a profile; `palette.c`
  splits the live palette into the dark and light copies a run stages, and
  `json.c` is the reader and writer they share with `driver.c`.
- `cheats/` holds the seeded cheats `--self-test` builds and the reason each must be rejected for.
- `reference` is the revision verdicts compare against by default.
