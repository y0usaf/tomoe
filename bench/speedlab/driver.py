import base64
import ctypes
import json
import mmap
import os
import fcntl
import hashlib
import random
import select
import signal
import struct
import subprocess
import sys
import threading
import termios
import time

STAGE = "/run/speedlab/stage"
CLOCK_PATH = "/run/speedlab/clock"
MAGIC = 0x6B6C6376
HEAD, SLOT, SLOTS = 64, 128, 256
SLOT_FORMAT = "<iiqIiQQQQQ64s"
SECOND = 10**9
T0 = 200_000 * SECOND
EPOCH = 1_795_400_000 * SECOND
BOOT = 3 * SECOND // 2
SETTLE = SECOND // 2
STUCK_SECONDS = 60
TYPE_MS = 40

TOOLS = os.environ["SPEEDLAB_TOOLS"]
PERF = os.environ["SPEEDLAB_PERF"]
MESA = os.environ["SPEEDLAB_MESA"]
PERSONA = os.environ["SPEEDLAB_PERSONA"]
HOME = os.environ["SPEEDLAB_HOME"]
ACCOUNT = os.environ["SPEEDLAB_ACCOUNT"]
UID = os.environ["SPEEDLAB_UID"]
CLOCK_LIB = f"{TOOLS}/lib/libspeedlab.so"
RUNTIME = f"/run/user/{UID}"

KEYS = {
    "alt": 56, "shift": 42, "super": 125, "ctrl": 29, "tab": 15, "space": 57, "enter": 28,
    "bracketleft": 26, "bracketright": 27, "minus": 12, "equal": 13,
}
for row, base in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for index, letter in enumerate(row):
        KEYS[letter] = base + index
BUTTONS = {"left": 272, "right": 273, "middle": 274}


class Failure(Exception):
    pass


class Clock:
    def __init__(self):
        fd = os.open(CLOCK_PATH, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        os.ftruncate(fd, HEAD + SLOT * SLOTS)
        self.mm = mmap.mmap(fd, HEAD + SLOT * SLOTS)
        os.close(fd)
        struct.pack_into("<IIqqI", self.mm, 0, 0, MAGIC, T0, EPOCH, 0)
        self.address = ctypes.addressof(ctypes.c_char.from_buffer(self.mm))
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.syscall.restype = ctypes.c_long

    @property
    def now(self):
        return struct.unpack_from("<q", self.mm, 8)[0]

    def advance(self, when, probe=False):
        struct.pack_into("<q", self.mm, 8, max(when, self.now))
        gen = (struct.unpack_from("<I", self.mm, 0)[0] + 1) & 0xFFFFFFFF
        if probe:
            struct.pack_into("<I", self.mm, 24, gen)
        struct.pack_into("<I", self.mm, 0, gen)
        self.libc.syscall(ctypes.c_long(202), ctypes.c_void_p(self.address), ctypes.c_long(1),
                          ctypes.c_long(0x7FFFFFFF), ctypes.c_long(0), ctypes.c_long(0), ctypes.c_long(0))

    def slots(self):
        for index in range(SLOTS):
            values = struct.unpack_from(SLOT_FORMAT, self.mm, HEAD + SLOT * index)
            if values[0]:
                yield index, values

    def release(self, index):
        struct.pack_into("<i", self.mm, HEAD + SLOT * index, 0)


class Reader(threading.Thread):
    def __init__(self, stream, sink):
        super().__init__(daemon=True)
        self.fd, self.sink = stream.fileno(), sink
        self.stream = stream
        self.lock = threading.Lock()
        self.partial = b""
        os.set_blocking(self.fd, False)
        self.start()

    def run(self):
        while True:
            select.select([self.fd], [], [])
            with self.lock:
                try:
                    data = os.read(self.fd, 1 << 20)
                except BlockingIOError:
                    continue
                if not data:
                    if self.partial:
                        self.sink.append(self.partial.decode("utf-8", "replace"))
                    return
                lines = (self.partial + data).split(b"\n")
                self.partial = lines.pop()
                self.sink.extend(line.decode("utf-8", "replace") for line in lines)

    def sync(self):
        buffered = ctypes.c_int(0)
        while self.is_alive():
            fcntl.ioctl(self.fd, termios.FIONREAD, buffered)
            if buffered.value == 0:
                break
            time.sleep(0.0001)
        with self.lock:
            pass


class Rig:
    def __init__(self, socket, jitter):
        self.clock = Clock()
        self.me = os.getpid()
        self.socket = socket
        self.jitter = jitter
        self.steps = 0
        self.names = {}
        self.clients = {}
        self.events = {}
        self.frames = []
        self.capture_lines = []
        self.phases = []
        self.samples = []
        self.violations = []
        self.input = None
        self.capture = None
        self.server = None
        self.counter = None
        self.started = time.monotonic()
        self.debug = []
        self.binary = None
        self.readers = []
        self.census_phases = {}
        self.recorder = None

    def scan(self):
        busy, snapshot = False, {}
        for name in os.listdir("/proc"):
            if not name.isdigit() or int(name) == self.me:
                continue
            try:
                tasks = os.listdir(f"/proc/{name}/task")
            except OSError:
                continue
            for tid in tasks:
                try:
                    with open(f"/proc/{name}/task/{tid}/stat", "rb") as f:
                        stat = f.read()
                    with open(f"/proc/{name}/task/{tid}/schedstat", "rb") as f:
                        slices = f.read().split()[2]
                except (OSError, IndexError):
                    busy = True
                    continue
                close = stat.rindex(b")")
                state = stat[close + 2:close + 3]
                if state in (b"R", b"D", b"W"):
                    busy = True
                if tid == name and name not in self.names:
                    self.names[name] = stat[stat.index(b"(") + 1:close].decode("utf-8", "replace")
                snapshot[tid] = (state, slices)
        return busy, snapshot

    def quiesce(self):
        limit = time.monotonic() + STUCK_SECONDS
        previous = None
        while True:
            busy, snapshot = self.scan()
            if not busy and snapshot == previous:
                return
            previous = None if busy else snapshot
            if time.monotonic() > limit:
                raise Failure("the session never settled: something kept running for "
                              f"{STUCK_SECONDS} s of real time at virtual {self.vtime():.3f} s: "
                              + self.describe(snapshot))
            if busy:
                time.sleep(0.0002)

    def describe(self, snapshot):
        self.debug = []
        for verb in ("inspect", "animation", "frames", "inspect"):
            try:
                self.debug.append(subprocess.run([self.binary, "--socket", self.socket, verb], capture_output=True,
                                                 text=True, timeout=10, env=self.client_env()).stdout)
            except Exception as error:
                self.debug.append(str(error))
            time.sleep(0.3)
        fds = []
        for fd in sorted(os.listdir(f"/proc/{self.server}/fd"), key=int) if self.server else []:
            try:
                target = os.readlink(f"/proc/{self.server}/fd/{fd}")
                with open(f"/proc/{self.server}/fdinfo/{fd}") as f:
                    info = " ".join(line.strip() for line in f if not line.startswith(("pos", "flags", "mnt_id", "ino")))
            except OSError:
                continue
            fds.append(f"{fd} -> {target} [{info}]")
        self.debug.append("\n".join(fds))
        table = []
        for name in sorted((n for n in os.listdir("/proc") if n.isdigit()), key=int):
            try:
                with open(f"/proc/{name}/status") as f:
                    fields = dict(line.rstrip("\n").split(":\t", 1) for line in f if ":\t" in line)
                with open(f"/proc/{name}/cmdline", "rb") as f:
                    command = f.read().replace(b"\0", b" ")[:160].decode("utf-8", "replace")
            except OSError:
                continue
            table.append(f"{name} ppid={fields.get('PPid')} {fields.get('State')} threads={fields.get('Threads')} {command}")
        self.debug.append("\n".join(table))
        time.sleep(0.2)
        _, later = self.scan()
        moving = []
        for tid, (state, slices) in sorted(later.items()):
            before = snapshot.get(tid)
            if state in (b"R", b"D", b"W") or not before or before[1] != slices:
                try:
                    with open(f"/proc/{tid}/comm") as f:
                        comm = f.read().strip()
                    with open(f"/proc/{tid}/wchan") as f:
                        wchan = f.read().strip()
                except OSError:
                    comm = wchan = "?"
                moving.append(f"{tid}:{comm}:{state.decode()}:{wchan}")
                if state == b"R" and self.server and os.path.exists(f"/proc/{self.server}/task/{tid}"):
                    self.clock.libc.syscall(ctypes.c_long(234), ctypes.c_long(self.server), ctypes.c_long(int(tid)),
                                            ctypes.c_long(signal.SIGABRT))
                    time.sleep(0.5)
                    break
        return ", ".join(moving[:12]) or "nothing visible"

    def alive(self, pid):
        try:
            with open(f"/proc/{pid}/stat", "rb") as f:
                stat = f.read()
        except OSError:
            return False
        return stat[stat.rindex(b")") + 2:stat.rindex(b")") + 3] != b"Z"

    def next_deadline(self):
        soonest = None
        for index, values in self.clock.slots():
            pid, next_due = values[0], values[2]
            if not self.alive(pid):
                self.collect(values)
                self.clock.release(index)
                continue
            if soonest is None or next_due < soonest:
                soonest = next_due
        return soonest if soonest is not None else 2**63 - 1

    def collect(self, values):
        pid, kind, _, _, number, count = values[:6]
        if count:
            where = values[10].rstrip(b"\0").decode("utf-8", "replace")
            self.violations.append({"pid": pid, "process": self.names.get(str(pid), "?"),
                                    "count": count, "syscall": number, "where": where})

    def step(self, when):
        if self.jitter and random.random() < 0.05:
            time.sleep(random.random() * 0.003)
        self.steps += 1
        self.clock.advance(when)

    def run_until(self, target):
        guard = 0
        while True:
            self.quiesce()
            due = self.next_deadline()
            if due > target:
                break
            guard = guard + 1 if due <= self.clock.now else 0
            if guard > 1000:
                raise Failure("a timer kept firing without virtual time moving")
            self.step(due)
        if self.clock.now < target:
            self.step(target)
        self.quiesce()

    def vtime(self):
        return (self.clock.now - T0) / SECOND

    def spawn(self, argv, env, events=None, stdin=False):
        process = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True)
        out, err = [], [] if events is None else events
        process.readers = [Reader(process.stdout, out), Reader(process.stderr, err)]
        process.out = out
        self.readers += process.readers
        return process

    def sync(self):
        for reader in self.readers:
            reader.sync()

    def client_env(self):
        return {"HOME": HOME, "XDG_RUNTIME_DIR": RUNTIME, "WAYLAND_DISPLAY": self.socket, "LD_PRELOAD": CLOCK_LIB,
                "WAYLAND_DEBUG": "client", "PATH": os.environ["PATH"], "LANG": "C.UTF-8"}

    def server_env(self):
        env = {
            "HOME": HOME, "USER": ACCOUNT, "LOGNAME": ACCOUNT, "SHELL": "/bin/sh",
            "PATH": os.environ["PATH"], "LANG": "C.UTF-8", "TZDIR": "/etc/zoneinfo",
            "XDG_RUNTIME_DIR": RUNTIME,
            "XDG_DATA_DIRS": f"/run/current-system/sw/share:/etc/profiles/per-user/{ACCOUNT}/share",
            "XCURSOR_THEME": read_stage("cursor-theme"), "XCURSOR_SIZE": read_stage("cursor-size"),
            "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe", "LP_NUM_THREADS": "16",
            "MESA_LOADER_DRIVER_OVERRIDE": "kms_swrast",
            "MESA_SHADER_CACHE_DISABLE": "true", "GBM_BACKENDS_PATH": f"{MESA}/lib/gbm",
            "LIBGL_DRIVERS_PATH": f"{MESA}/lib/dri",
            "__EGL_VENDOR_LIBRARY_FILENAMES": f"{MESA}/share/glvnd/egl_vendor.d/50_mesa.json",
            "LD_PRELOAD": CLOCK_LIB,
        }
        if PERSONA == "A":
            env.update({"TOMOE_ISOLATED": "/run/speedlab", "SPEEDLAB": "1", "SPEEDLAB_RUN": "measure"})
        return env

    def start(self, binary, profile):
        self.binary = binary
        self.profiling = profile
        argv = [f"{TOOLS}/bin/speedlab-count", "--log", f"{HOME}/.speedlab-server.log"]
        env = self.server_env()
        if profile:
            env.pop("LD_PRELOAD", None)
            argv += ["--open", "--", f"{PERF}/bin/perf", "record", "-q", "-e", "instructions:u", "-c", "10000000",
                     "-o", f"{HOME}/.speedlab-perf.data", "--", "env", "SPEEDLAB_CENSUS=1",
                     f"LD_PRELOAD={TOOLS}/lib/libspeedlab-census.so:{CLOCK_LIB}", binary, "--backend", "headless"]
        else:
            argv += ["--", binary, "--backend", "headless"]
        if PERSONA == "A":
            argv += ["--socket", self.socket]
        self.counter = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, close_fds=True)
        line = self.counter.stdout.readline().split()
        if not line or line[0] != "started":
            raise Failure("the counter could not start the compositor")
        self.server = int(line[1].split("=")[1])
        if profile:
            while True:
                self.quiesce()
                children = [p for p in os.listdir("/proc") if p.isdigit() and parent_of(p) == self.server]
                found = [p for p in children if command_of(p) == "tomoe"]
                if found:
                    self.recorder, self.server = self.server, int(found[0])
                    break
                self.step(self.clock.now + SECOND // 1000)
        socket_path = f"{RUNTIME}/{self.socket}"
        limit = self.clock.now + BOOT
        while not os.path.exists(socket_path):
            if self.clock.now >= limit or not self.alive(self.server):
                raise Failure(f"the compositor did not open {socket_path}")
            self.run_until(min(limit, self.clock.now + SECOND // 10))
        dumps = []
        if os.path.exists(f"{STAGE}/dump-frames"):
            os.makedirs(f"{HOME}/.speedlab-dumps", exist_ok=True)
            dumps = ["--dump-dir", f"{HOME}/.speedlab-dumps"]
            for index in read_stage("dump-frames").split():
                dumps += ["--dump", index]
        self.capture = self.spawn([f"{TOOLS}/bin/speedlab-capture", "--output", read_stage("output"), *dumps],
                                  self.client_env(), self.events.setdefault("capture", []))
        self.input = self.spawn([f"{TOOLS}/bin/speedlab-input", *read_stage("extent").split()],
                                self.client_env(), self.events.setdefault("input", []), stdin=True)
        self.run_until(T0 + BOOT)
        self.sync()
        if not any(line == "ready" for line in self.input.out):
            raise Failure("the input device never connected")
        if not any(line.startswith("constraints") for line in self.capture.out):
            raise Failure("the output capture never started")
        self.mark("boot")

    def send_input(self, *lines):
        for line in lines:
            before = len(self.input.out)
            self.input.stdin.write((line + "\n").encode())
            self.input.stdin.flush()
            limit = self.clock.now + 5 * SECOND
            while True:
                self.quiesce()
                self.sync()
                if len(self.input.out) > before:
                    break
                if self.input.poll() is not None:
                    raise Failure("the input device died: " + "\n".join(self.events["input"][-5:]))
                due = self.next_deadline()
                if due > limit:
                    raise Failure(f"the compositor never answered the input {line!r} within 5 virtual seconds")
                self.step(due)

    def counters(self):
        self.counter.stdin.write("read\n")
        self.counter.stdin.flush()
        fields = dict(field.split("=") for field in self.counter.stdout.readline().split()[1:])
        return int(fields["main"])

    def memory(self):
        self.clock.advance(self.clock.now, probe=True)
        self.quiesce()
        values = {}
        with open(f"/proc/{self.server}/smaps_rollup") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[0].endswith(":") and parts[1].isdigit():
                    values[parts[0][:-1]] = int(parts[1])
        with open(f"/proc/{self.server}/status") as f:
            for line in f:
                if line.startswith(("VmHWM", "VmRSS")):
                    values[line.split(":")[0]] = int(line.split()[1])
        for _, slot in self.clock.slots():
            if slot[0] == self.server:
                values.update({"malloc_arena_kB": slot[6] // 1024, "malloc_used_kB": (slot[7] + slot[9]) // 1024,
                               "malloc_free_kB": slot[8] // 1024, "malloc_mmap_kB": slot[9] // 1024})
        return values

    def mark(self, name):
        sample = self.memory()
        self.census_phases[str(struct.unpack_from("<I", self.clock.mm, 24)[0])] = name
        self.phases.append({"name": name, "t": round(self.vtime(), 6), "main": self.counters(),
                            "frames": self.frame_count(), "pss_kB": sample.get("Pss", 0),
                            "private_dirty_kB": sample.get("Private_Dirty", 0)})
        self.samples.append(sample)

    def frame_count(self):
        self.sync()
        return sum(1 for line in self.capture.out if line.startswith("frame "))

    def chord(self, combo):
        names = combo.split("+")
        codes = [KEYS[name] for name in names]
        self.send_input(*[f"key {code} down" for code in codes],
                        *[f"key {code} up" for code in reversed(codes)])

    def act(self, verb, args):
        if verb == "open":
            name, color = args[0], args[1]
            argv = [f"{TOOLS}/bin/speedlab-client", "--app-id", name, "--title", name, "--color", color]
            if len(args) > 2:
                argv += ["--animate", args[2]]
            self.clients[name] = self.spawn(argv, self.client_env(), self.events.setdefault(name, []), stdin=True)
        elif verb == "animate":
            self.tell(args[0], f"animate {args[1]}")
        elif verb == "quit":
            self.tell(args[0], "quit")
        elif verb == "key":
            self.chord(args[0])
        elif verb == "move":
            self.send_input(f"move {args[0]} {args[1]}")
        elif verb == "press":
            self.send_input(f"key {KEYS[args[0]]} down")
        elif verb == "release":
            self.send_input(f"key {KEYS[args[0]]} up")
        elif verb == "button":
            self.send_input(f"button {BUTTONS[args[0]]} {args[1]}")
        elif verb == "palette":
            with open(f"{HOME}/.cache/theme/gtk.css", "w") as f:
                f.write(read_stage(f"palette-{args[0]}"))
        elif verb == "mark":
            self.mark(args[0])
        elif verb != "wait":
            raise Failure(f"unknown workload verb {verb}")

    def tell(self, name, line):
        client = self.clients[name]
        if client.poll() is not None:
            raise Failure(f"client {name} exited early: " + "\n".join(self.events[name][-5:]))
        client.stdin.write((line + "\n").encode())
        client.stdin.flush()

    def glide(self, start, x0, y0, x1, y1, span):
        self.send_input(f"move {x0} {y0}")
        steps = max(1, span // 4)
        for index in range(1, steps + 1):
            self.run_until(start + index * 4 * 10**6)
            x = x0 + (x1 - x0) * index // steps
            y = y0 + (y1 - y0) * index // steps
            self.send_input(f"move {x} {y}")

    def workload(self, lines):
        origin = self.clock.now
        for line in lines:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            at, verb, args = int(line[0]), line[1], line[2:]
            self.run_until(origin + at * 10**6)
            if verb == "glide":
                x0, y0, x1, y1, span = map(int, args)
                self.glide(origin + at * 10**6, x0, y0, x1, y1, span)
            elif verb == "type":
                for index, letter in enumerate(" ".join(args)):
                    self.run_until(origin + (at + index * TYPE_MS) * 10**6)
                    self.chord("space" if letter == " " else letter)
            else:
                self.act(verb, args)
            self.quiesce()
            for name, client in self.clients.items():
                if client.poll() not in (None, 0):
                    raise Failure(f"client {name} failed: " + "\n".join(self.events[name][-5:]))
        self.run_until(self.clock.now + SETTLE)
        self.mark("settled")

    def breakdown(self):
        groups = {}
        current = None
        with open(f"/proc/{self.server}/smaps") as f:
            for line in f:
                parts = line.split()
                if parts and "-" in parts[0] and not parts[0].endswith(":"):
                    path = " ".join(parts[5:]) if len(parts) > 5 else ""
                    current = groups.setdefault(category(path, parts[1]), {"Rss": 0, "Pss": 0, "Private_Dirty": 0,
                                                                          "Private_Clean": 0, "Size": 0, "mappings": 0})
                    current["mappings"] += 1
                elif current is not None and len(parts) >= 2 and parts[0][:-1] in current:
                    current[parts[0][:-1]] += int(parts[1])
        return sorted(({"what": name, **{k + ("_kB" if k != "mappings" else ""): v for k, v in values.items()}}
                       for name, values in groups.items()), key=lambda item: -item["Pss_kB"])

    def profile(self):
        reports = {}
        data = f"{HOME}/.speedlab-perf.data"
        for name, sort in (("threads", "comm,dso"), ("functions", "comm,dso,sym")):
            done = subprocess.run([f"{PERF}/bin/perf", "report", "-i", data, "--stdio", "--no-children", "-F",
                                   "overhead,sample," + sort, "--sort", sort, "--percent-limit", "0.05"],
                                  capture_output=True, text=True, env={"PATH": os.environ["PATH"], "HOME": HOME})
            reports[name] = done.stdout[-600000:] + done.stderr[-2000:]
        census = {}
        for name in sorted(os.listdir(HOME)):
            if name.startswith(".speedlab-census-"):
                with open(f"{HOME}/{name}") as f:
                    census[name] = f.read()
        reports["census"] = census
        reports["census_phases"] = self.census_phases
        return reports

    def finish(self):
        settled = self.samples[-1]
        breakdown = self.breakdown()
        for name in list(self.clients):
            if self.clients[name].poll() is None:
                self.tell(name, "quit")
        self.run_until(self.clock.now + SECOND // 2)
        os.kill(self.server, signal.SIGTERM)
        limit = self.clock.now + 5 * SECOND
        while self.alive(self.server) and self.clock.now < limit:
            self.run_until(self.clock.now + SECOND // 20)
        self.counter.stdin.write("wait\n")
        self.counter.stdin.flush()
        status = int(self.counter.stdout.readline().split("=")[1])
        for index, values in self.clock.slots():
            self.collect(values)
        for name in os.listdir("/proc"):
            if name.isdigit() and int(name) not in (self.me, self.counter.pid):
                try:
                    os.kill(int(name), signal.SIGKILL)
                except OSError:
                    pass
        time.sleep(0.05)
        self.counter.stdin.write("final\n")
        self.counter.stdin.flush()
        fields = dict(field.split("=") for field in self.counter.stdout.readline().split()[1:])
        for process in [self.input, self.capture, *self.clients.values()]:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        for reader in self.readers:
            reader.join(timeout=5)
        for line in self.capture.out:
            if line.startswith("frame "):
                parts = line.split()
                self.frames.append([int(parts[1]), parts[2], parts[3], parts[4], parts[5:]])
        peak = max(self.samples, key=lambda s: s.get("Pss", 0))
        return {
            "exit_status": status,
            "instructions": int(fields["instructions"]),
            "cycles": int(fields["cycles"]),
            "main_instructions": int(fields["main"]),
            "process_instructions": int(fields["process"]),
            "scaled": fields["scaled"] == "1",
            "phases": self.phases,
            "memory": {"peak_pss_kB": peak.get("Pss", 0), "peak_private_dirty_kB": max(s.get("Private_Dirty", 0) for s in self.samples),
                       "hwm_kB": settled.get("VmHWM", 0), "settled": settled, "breakdown": breakdown},
            "frames": self.frames,
            "capture": [line for line in self.capture.out if not line.startswith("frame ")],
            "events": self.events,
            "violations": self.violations,
            "steps": self.steps,
            "virtual_seconds": round(self.vtime(), 6),
            "real_seconds": round(time.monotonic() - self.started, 3),
            "state": state_listing(self.socket),
        }


def category(path, perms):
    base = path.rsplit("/", 1)[-1]
    if not path:
        if "x" in perms:
            return "anonymous executable (JIT)"
        if perms.startswith("---"):
            return "anonymous reservation"
        return "anonymous (malloc mmap, textures, stacks)"
    if path == "[heap]":
        return "malloc heap (brk)"
    if path.startswith("[stack"):
        return "main stack"
    if path in ("[vvar]", "[vdso]", "[vsyscall]", "[vvar_vclock]"):
        return "kernel"
    if path.startswith("/memfd:"):
        return "memfd " + path[len("/memfd:"):].split(" (deleted)")[0]
    if path.startswith("/dev/dri/"):
        return "GPU buffer mappings"
    if "dmabuf" in path or path.startswith("anon_inode:"):
        return "dmabuf " + path
    if base.endswith((".ttf", ".otf", ".ttc", ".pcf", ".woff", ".woff2", ".pcf.gz")):
        return "font files"
    if base.endswith("cache-11") or "fontconfig" in path:
        return "fontconfig cache"
    if ".so" in base:
        return "library " + base.split(".so")[0] + ".so"
    if path.startswith("/nix/store/") and "/bin/" in path:
        return "executable " + base
    return "file " + path


def state_listing(socket):
    found = []
    for root in (HOME, RUNTIME, "/tmp", "/var/tmp", "/dev/shm"):
        for directory, dirs, files in os.walk(root):
            for name in files + [d for d in dirs if os.path.islink(os.path.join(directory, d))]:
                path = os.path.join(directory, name)
                if path.startswith(f"{HOME}/.speedlab"):
                    continue
                try:
                    if path.startswith(f"{HOME}/.local/state/tomoe/") and path.endswith(".log"):
                        digest = "log"
                    elif os.path.islink(path) or not os.path.isfile(path):
                        digest = "link:" + os.readlink(path) if os.path.islink(path) else "special"
                    else:
                        with open(path, "rb") as f:
                            digest = hashlib.sha256(f.read()).hexdigest()[:24]
                except OSError:
                    digest = "unreadable"
                found.append([path.replace(socket, "<socket>"), digest])
    return sorted(found)


def parent_of(pid):
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            stat = f.read()
        return int(stat[stat.rindex(b")") + 2:].split()[1])
    except (OSError, ValueError, IndexError):
        return 0


def command_of(pid):
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return ""


def read_stage(name):
    with open(f"{STAGE}/{name}") as f:
        return f.read().strip() if not name.startswith("palette") else f.read()


def main():
    binary = sys.argv[1]
    profile = sys.argv[2] if len(sys.argv) > 2 else None
    prctl = ctypes.CDLL(None).prctl
    prctl(4, 0, 0, 0, 0)
    socket = "speedlab-a" if PERSONA == "A" else "tomoe-0"
    random.seed(os.urandom(16))
    rig = Rig(socket, jitter=PERSONA == "B")
    result = {"persona": PERSONA}
    try:
        os.makedirs(f"{HOME}/.cache/theme", exist_ok=True)
        with open(f"{HOME}/.cache/theme/gtk.css", "w") as f:
            f.write(read_stage("palette-dark"))
        with open(f"{STAGE}/workload") as f:
            lines = f.readlines()
        rig.start(binary, profile)
        rig.workload(lines)
        result.update(rig.finish())
        if profile:
            result["profile"] = rig.profile()
        result["ok"] = True
    except Failure as error:
        result.update({"ok": False, "error": str(error)})
    except Exception as error:
        result.update({"ok": False, "error": f"{type(error).__name__}: {error}"})
    if rig.debug:
        result["debug"] = rig.debug
    if os.path.isdir(f"{HOME}/.speedlab-dumps"):
        result["dumps"] = {}
        for name in sorted(os.listdir(f"{HOME}/.speedlab-dumps")):
            with open(f"{HOME}/.speedlab-dumps/{name}", "rb") as f:
                result["dumps"][name] = base64.b64encode(f.read()).decode()
    result["log_tail"] = []
    for path in (f"{HOME}/.speedlab-server.log", f"{HOME}/.local/state/tomoe/{socket}.log"):
        try:
            with open(path, errors="replace") as f:
                result["log_tail"] += f.read().splitlines()[-60:]
        except OSError:
            pass
    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()


main()
