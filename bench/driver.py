import base64
import json
import os
import re
import statistics
import time

RUNTIME = "/run/bench"
UNIT = "bench-comp"
ENVIRONMENT = {
    "HOME": "/root",
    "PATH": "/run/current-system/sw/bin",
    "LIBGL_ALWAYS_SOFTWARE": "1",
    "MESA_LOADER_DRIVER_OVERRIDE": "zink",
    "GALLIUM_DRIVER": "llvmpipe",
    "LP_NUM_THREADS": "4",
    "XDG_CACHE_HOME": "/run/bench/cache",
    "XDG_DATA_DIRS": "/run/current-system/sw/share",
    "XDG_CONFIG_HOME": "/run/bench/config",
    "NO_AT_BRIDGE": "1",
}


def environment(name, **extra):
    values = dict(ENVIRONMENT)
    values["XDG_RUNTIME_DIR"] = f"{RUNTIME}/rt-{name}"
    values.update({key: str(value) for key, value in extra.items()})
    return values


def shell_environment(values):
    return "".join(f"{key}={value} " for key, value in values.items())


def unit_environment(values):
    return "".join(f"--setenv={key}={value} " for key, value in values.items())


class Compositor:
    name = ""
    version_command = ""
    presentation = "wp_presentation"
    focus_binding = ""
    drag_binding = ""
    drag_setup = ""

    def __init__(self, machine, paths):
        self.machine = machine
        self.paths = paths
        self.runtime = f"{RUNTIME}/rt-{self.name}"
        self.config = f"{RUNTIME}/config-{self.name}"
        self.display = None
        self.pid = None

    def run(self, command, check=True, timeout=900):
        return self.machine.execute(command, check_return=check, timeout=timeout)[1]

    def configure(self):
        self.run(f"mkdir -p {self.runtime} {self.config} && chmod 700 {self.runtime} {self.config}")
        for path, content in self.files().items():
            self.write(path, content)

    def write(self, path, content):
        encoded = base64.b64encode(content.encode()).decode()
        self.run(f"mkdir -p {os.path.dirname(path)} && "
                 f"printf %s {encoded} | base64 -d > {path}")

    def files(self):
        raise NotImplementedError

    def command(self):
        raise NotImplementedError

    def extra_environment(self):
        return {}

    def start(self):
        self.stop()
        self.configure()
        values = environment(self.name, **self.extra_environment())
        self.run(
            f"systemd-run --unit={UNIT} --property=Type=exec --remain-after-exit "
            + unit_environment(values)
            + " ".join(self.command())
        )
        self.pid = self.wait_pid()
        self.wait_display()
        return self.pid

    def wait_pid(self):
        for _ in range(1200):
            state = self.run(f"systemctl show -P SubState {UNIT}", check=False).strip()
            if state in ("running", "exited", "failed"):
                if state != "running":
                    raise AssertionError(
                        f"{self.name} stopped while starting:\n" + self.log()
                    )
                pid = self.run(f"systemctl show -P MainPID {UNIT}").strip()
                if pid and pid != "0":
                    return int(pid)
            time.sleep(0.1)
        raise AssertionError(f"{self.name} did not spawn:\n" + self.log())

    def sockets(self):
        listing = self.run(f"ls {self.runtime} 2>/dev/null", check=False)
        return [line for line in listing.split()
                if line.startswith("wayland-") and ".lock" not in line]

    def wait_display(self, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.sockets()
            if found:
                self.display = sorted(found)[0]
                self.wait_first_frame()
                return
            if self.run(f"systemctl show -P SubState {UNIT}", check=False).strip() != "running":
                raise AssertionError(f"{self.name} exited before creating a socket:\n" + self.log())
            time.sleep(0.2)
        raise AssertionError(f"{self.name} created no wayland socket in {timeout} s:\n" + self.log())

    def wait_first_frame(self, timeout=120):
        return

    def log(self):
        return self.run(f"journalctl -u {UNIT} --no-pager -o cat | tail -40", check=False)

    def stop(self):
        self.run(f"systemctl stop {UNIT}", check=False)
        self.run(f"systemctl reset-failed {UNIT}", check=False)
        self.run(f"rm -rf {self.runtime} {self.config}", check=False)
        self.display = None

    def client_environment(self):
        return {"XDG_RUNTIME_DIR": self.runtime, "WAYLAND_DISPLAY": self.display}

    def windows(self):
        raise NotImplementedError()

    def prepare_drag(self):
        return self.drag_setup

    def memory(self):
        return {}

    def version(self):
        values = environment(self.name, **self.extra_environment())
        output = self.run(f"{shell_environment(values)}{self.version_command}", check=False)
        return output.strip().splitlines()[0] if output.strip() else ""

    def bench(self, arguments, timeout=600):
        envs = shell_environment(dict(ENVIRONMENT) | self.client_environment())
        return self.run(f"{envs}{self.paths['client']} {arguments}", timeout=timeout)


class Tomoe(Compositor):
    name = "tomoe"
    version_command = "tomoe --version"
    focus_binding = "super+Tab -> focus-next-window (init.lisp)"
    drag_binding = "super+LMB -> move window (builtin drag extension)"
    drag_setup = "none; the builtin drag extension moves a tiled window"

    def files(self):
        return {
            f"{self.config}/init.lisp":
                '(in-package #:tomoe-user)\n'
                '\n'
                '(define-extension "bench-bindings" (:reads () :state nil) (snapshot state event)\n'
                '  (declare (ignore snapshot state event))\n'
                '  (values nil (list (bind-key \'(:mod) "Tab" "focus-next-window") nil)))\n'
        }

    def command(self):
        command = ["tomoe", "--socket", "bench", "--backend", "drm"]
        if self.paths.get("drm_device"):
            command += ["--drm_device", self.paths["drm_device"]]
        return command + ["--config", f"{self.config}/init.lisp", "--no-watch"]

    def extra_environment(self):
        return {"LIBSEAT_BACKEND": "builtin", "TOMOE_TIMINGS": "1"}

    def sockets(self):
        listing = self.run(f"ls {self.runtime} 2>/dev/null", check=False)
        return [line for line in listing.split() if line == "bench"]

    def wait_first_frame(self):
        envs = shell_environment(dict(ENVIRONMENT) | {"XDG_RUNTIME_DIR": self.runtime})
        for _ in range(600):
            if self.run(f"{envs}tomoe --socket bench inspect", check=False).strip():
                break
            time.sleep(0.1)
        self.run(f"{envs}tomoe --socket bench frames", check=False)

    def windows(self):
        reply = self.run(
            f"{shell_environment(dict(ENVIRONMENT) | {'XDG_RUNTIME_DIR': self.runtime})}"
            "tomoe --socket bench msg windows",
            check=False,
        )
        rows = []
        for entry in json.loads(reply or "[]"):
            box = entry.get("geometry") or {}
            rows.append({
                "app_id": entry.get("app_id", ""),
                "x": box.get("x"), "y": box.get("y"),
                "w": box.get("w"), "h": box.get("h"),
                "focused": bool(entry.get("focused")),
            })
        return rows

    def output(self):
        envs = shell_environment(dict(ENVIRONMENT) | {"XDG_RUNTIME_DIR": self.runtime})
        reply = self.run(f"{envs}tomoe --socket bench msg outputs", check=False)
        inspect = self.run(f"{envs}tomoe --socket bench inspect", check=False)
        refresh = re.search(r":REFRESH-MHZ (\d+)", inspect)
        for entry in json.loads(reply or "[]"):
            box = entry.get("geometry") or {}
            if box.get("w"):
                return {"name": entry.get("name"), "width": box["w"], "height": box["h"],
                        "refresh": int(refresh.group(1)) if refresh else None}
        return {}

    def memory(self):
        envs = shell_environment(dict(ENVIRONMENT) | {"XDG_RUNTIME_DIR": self.runtime})
        reply = self.run(f"{envs}tomoe --socket bench memory", check=False).strip()
        values = {}
        for line in reply.splitlines():
            match = re.match(r":([A-Z-]+) (-?\d+)", line.strip())
            if match:
                values[match.group(1).lower()] = int(match.group(2))
        return values


class Sway(Compositor):
    name = "sway"
    version_command = "sway --version"
    focus_binding = "Mod4+Tab -> focus next (bindsym)"
    drag_binding = "Mod4+LMB -> floating move (floating_modifier)"
    drag_setup = "swaymsg floating enable on the bench window"

    def files(self):
        return {
            f"{self.config}/sway.conf":
                "floating_modifier Mod4\n"
                "bindsym Mod4+Tab focus next\n"
                "default_border pixel 1\n"
                "focus_follows_mouse no\n"
                "xwayland disable\n"
        }

    def command(self):
        return ["sway", "--config", f"{self.config}/sway.conf", "--unsupported-gpu"]

    def extra_environment(self):
        values = {
            "WLR_BACKENDS": "drm,libinput",
            "WLR_RENDERER": "pixman",
            "WLR_NO_HARDWARE_CURSORS": "1",
            "LIBSEAT_BACKEND": "builtin",
        }
        if self.paths.get("drm_device"):
            values["WLR_DRM_DEVICES"] = self.paths["drm_device"]
        return values

    def ipc(self):
        listing = self.run(f"ls {self.runtime}/sway-ipc.* 2>/dev/null", check=False).split()
        return listing[0] if listing else ""

    def swaymsg(self, message):
        return self.run(f"swaymsg -s {self.ipc()} {message}", check=False)

    def wait_first_frame(self):
        for _ in range(600):
            if self.ipc():
                return
            time.sleep(0.1)

    def windows(self):
        tree = json.loads(self.run(f"swaymsg -s {self.ipc()} -t get_tree -r", check=False) or "{}")
        rows = []

        def walk(node):
            if node.get("app_id", "").startswith("bench-"):
                rect = node.get("rect", {})
                rows.append({
                    "app_id": node.get("app_id"),
                    "x": rect.get("x"), "y": rect.get("y"),
                    "w": rect.get("width"), "h": rect.get("height"),
                    "focused": bool(node.get("focused")),
                })
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                walk(child)

        walk(tree)
        return rows

    def output(self):
        try:
            entries = json.loads(self.run(f"swaymsg -s {self.ipc()} -t get_outputs -r", check=False) or "[]")
        except ValueError:
            return {}
        for entry in entries:
            mode = entry.get("current_mode") or {}
            if mode.get("width"):
                return {"name": entry.get("name"), "width": mode["width"],
                        "height": mode["height"], "refresh": mode.get("refresh")}
        return {}

    def prepare_drag(self):
        self.swaymsg("'[app_id=^bench-] floating enable'")
        self.swaymsg("'[app_id=^bench-] move position 300 300'")
        return self.drag_setup


class Niri(Compositor):
    name = "niri"
    version_command = "niri --version"
    focus_binding = "Mod+Tab -> focus-window-down (config.kdl)"
    drag_binding = "none: niri 26.04 has no interactive window move on a pointer bind"
    drag_setup = "none"

    def files(self):
        return {
            f"{self.config}/config.kdl":
                "input {\n"
                "    keyboard {\n"
                "        xkb {\n"
                "            layout \"us\"\n"
                "        }\n"
                "    }\n"
                "}\n"
                "binds {\n"
                "    Mod+Tab { focus-window-down; }\n"
                "}\n"
        }

    def command(self):
        return ["niri", "--config", f"{self.config}/config.kdl"]

    def extra_environment(self):
        return {"LIBSEAT_BACKEND": "builtin"}

    def niri(self, arguments):
        values = environment(self.name)
        return self.run(f"{shell_environment(values)}niri msg {arguments}", check=False)

    def wait_first_frame(self):
        for index in range(300):
            reply = self.niri("--json outputs").strip()
            if reply.startswith("[") or reply.startswith("{"):
                return
            if index % 10 == 0 and "no allocator available for device" in self.log():
                raise AssertionError(
                    "niri refuses software rendering here; its own log says "
                    "software EGL renderers are skipped")
            time.sleep(0.1)

    def windows(self):
        reply = self.niri("--json windows")
        rows = []
        try:
            entries = json.loads(reply or "[]")
        except ValueError:
            return rows
        for entry in entries:
            layout = entry.get("layout") or {}
            position = layout.get("pos_in_scrolling_layout") or [0, 0]
            size = layout.get("window_size") or [0, 0]
            rows.append({
                "app_id": entry.get("app_id", ""),
                "x": position[0], "y": position[1],
                "w": size[0], "h": size[1],
                "focused": bool(entry.get("is_focused")),
            })
        return rows

    def output(self):
        try:
            entries = json.loads(self.niri("--json outputs") or "{}")
        except ValueError:
            return {}
        for name, entry in entries.items():
            mode = entry.get("current_mode") or {}
            if mode.get("width"):
                return {"name": name, "width": mode["width"], "height": mode["height"],
                        "refresh": mode.get("refresh_rate")}
        return {}

    def memory(self):
        return {}


class Hyprland(Compositor):
    name = "hyprland"
    version_command = "Hyprland --version"
    focus_binding = "SUPER+Tab -> cyclenext (hyprland.conf)"
    drag_binding = "SUPER+LMB -> movewindow (bindm)"
    drag_setup = "hyprctl dispatch togglefloating on the bench window"

    def files(self):
        return {
            f"{self.config}/hypr/hyprland.conf":
                "monitor=Virtual-1,preferred,0x0,1\n"
                "bind = SUPER, Tab, cyclenext\n"
                "bindm = SUPER, mouse:272, movewindow\n"
                "input {\n"
                "    follow_mouse = 0\n"
                "    kb_layout = us\n"
                "}\n"
                "general {\n"
                "    border_size = 1\n"
                "    gaps_in = 0\n"
                "    gaps_out = 0\n"
                "}\n"
                "misc {\n"
                "    disable_hyprland_logo = true\n"
                "    disable_splash_override = true\n"
                "    force_default_wallpaper = 0\n"
                "}\n"
                "debug {\n"
                "    disable_logs = false\n"
                "}\n"
        }

    def command(self):
        return ["Hyprland", "--i-am-really-stupid", "--config",
                f"{self.config}/hypr/hyprland.conf"]

    def extra_environment(self):
        values = {
            "LIBSEAT_BACKEND": "builtin",
            "HYPRLAND_NO_SD_NOTIFY": "1",
        }
        if self.paths.get("drm_device"):
            values["AQ_DRM_DEVICES"] = self.paths["drm_device"]
        return values

    def signature(self):
        listing = self.run(f"ls {self.runtime}/hypr 2>/dev/null", check=False).split()
        return listing[0] if listing else ""

    def hyprctl(self, arguments):
        signature = self.signature()
        if not signature:
            return ""
        values = environment(self.name, HYPRLAND_INSTANCE_SIGNATURE=signature,
                             XDG_RUNTIME_DIR=self.runtime)
        return self.run(f"{shell_environment(values)}hyprctl {arguments}", check=False)

    def wait_first_frame(self):
        for _ in range(1200):
            if self.hyprctl("-j monitors").strip().startswith("["):
                return
            time.sleep(0.1)

    def windows(self):
        try:
            entries = json.loads(self.hyprctl("-j clients") or "[]")
        except ValueError:
            return []
        rows = []
        for entry in entries:
            if not str(entry.get("class", "")).startswith("bench-"):
                continue
            position = entry.get("at") or [0, 0]
            size = entry.get("size") or [0, 0]
            rows.append({
                "app_id": entry.get("class"),
                "x": position[0], "y": position[1],
                "w": size[0], "h": size[1],
                "focused": bool(entry.get("focusHistoryID") == 0),
            })
        return rows

    def output(self):
        try:
            entries = json.loads(self.hyprctl("-j monitors") or "[]")
        except ValueError:
            return {}
        for entry in entries:
            if entry.get("width"):
                return {"name": entry.get("name"), "width": entry["width"],
                        "height": entry["height"], "refresh": entry.get("refreshRate")}
        return {}

    def prepare_drag(self):
        self.hyprctl("dispatch togglefloating")
        time.sleep(0.4)
        return self.drag_setup

    def memory(self):
        return {}


CLASSES = {"tomoe": Tomoe, "sway": Sway, "niri": Niri, "hyprland": Hyprland}


def create(machine, paths, name):
    return CLASSES[name](machine, paths)


REPEATS = 3
IDLE_REPEATS = 2
SAMPLES = 200
FRAME_SECONDS = 8
IDLE_SECONDS = 60
WINDOW_COUNTS = (1, 10, 50)
IDLE_UNIT = "bench-idle"
READY = "/run/bench/ready"
WAIT = "/run/bench/go"


def ticks_per_second(machine):
    return int(machine.succeed("getconf CLK_TCK").strip())


def cpu_ticks(machine, pid):
    fields = machine.succeed(f"cat /proc/{pid}/stat").split()
    return int(fields[13]) + int(fields[14])


def context_switches(machine, pid):
    status = machine.succeed(f"cat /proc/{pid}/status")
    return tuple(int(value) for value in
                 re.findall(r"^(?:non)voluntary_ctxt_switches:\s+(\d+)", status, re.MULTILINE))


def rollup(machine, pid):
    values = {}
    for line in machine.succeed(f"cat /proc/{pid}/smaps_rollup").splitlines():
        key, _, rest = line.partition(":")
        if rest and rest.split():
            values[key.strip()] = int(rest.split()[0])
    return values


def megabytes(memory):
    converted = {}
    for key, value in memory.items():
        if key.endswith("_kb"):
            converted[f"{key[:-3]}_mb"] = round(value / 1024, 1)
        else:
            converted[key] = value
    return converted


def smaps(machine, pid, samples=3, gap=0.3):
    rows = []
    for index in range(samples):
        if index:
            time.sleep(gap)
        rows.append(rollup(machine, pid))
    result = {}
    for key, name in (("Rss", "rss"), ("Pss", "pss"), ("Pss_Anon", "pss_anon"),
                      ("Pss_File", "pss_file"), ("Private_Dirty", "private_dirty")):
        values = [row.get(key, 0) for row in rows]
        result[f"{name}_mb"] = round(statistics.median(values) / 1024, 1)
        result[f"{name}_noise_pct"] = round((max(values) - min(values)) / statistics.median(values)
                                            * 100, 1) if statistics.median(values) else 0.0
    result["samples"] = samples
    return result


def parse(text):
    anchor = text.rfind('"phase"')
    if anchor < 0:
        return {}
    start = text.rfind("{", 0, anchor)
    end = text.find("}", anchor)
    while start >= 0 and end >= 0:
        try:
            return json.loads(text[start:end + 1])
        except ValueError:
            end = text.find("}", end + 1)
    return {}


def counter(text, key):
    for line in reversed(text.splitlines()):
        fields = line.split()
        if len(fields) == 2 and fields[0] == key:
            return int(fields[1])
    return None


def endpoint(runs, name):
    values = [run.get(name) or {} for run in runs]
    present = [value for value in values if value.get("n")]
    if not present:
        return {"status": "unsupported", "samples": 0}
    p50s = [value["p50"] for value in present]
    median = statistics.median(p50s)
    return {
        "status": "ok",
        "samples": sum(value["n"] for value in present),
        "p50": round(median, 3),
        "p99": round(statistics.median(value["p99"] for value in present), 3),
        "max": round(max(value["max"] for value in present), 3),
        "noise_pct": round((max(p50s) - min(p50s)) / median * 100, 1) if median else 0.0,
        "runs_p50": [round(value, 3) for value in p50s],
    }


def merge_runs(runs):
    merged = {
        "runs": len(runs),
        "lost": sum(run.get("lost", 0) for run in runs),
        "presented_abandoned": any(run.get("presented_abandoned") for run in runs),
        "redraw_abandoned": any(run.get("redraw_abandoned") for run in runs),
        "event": endpoint(runs, "event_us"),
        "presented": endpoint(runs, "presented_us"),
        "redraw": endpoint(runs, "redraw_us"),
    }
    if merged["presented_abandoned"]:
        merged["presented"]["status"] = "unreliable"
    if merged["redraw_abandoned"]:
        merged["redraw"]["status"] = "unreliable"
    return merged


def placement(rows):
    return sorted((row.get("app_id"), row.get("x"), row.get("y"), row.get("w"), row.get("h"))
                  for row in rows if str(row.get("app_id", "")).startswith("bench-"))


def async_client(machine, runner, arguments, unit):
    machine.execute(f"systemctl stop {unit}", check_return=False)
    machine.execute(f"systemctl reset-failed {unit}", check_return=False)
    machine.execute(f"rm -f {READY}", check_return=False)
    values = dict(ENVIRONMENT) | runner.client_environment()
    envs = "".join(f"--setenv={key}={value} " for key, value in values.items())
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit {envs}"
        f"{runner.paths['client']} {arguments} --ready {READY}"
    )
    for _ in range(600):
        if machine.execute(f"test -s {READY}", check_return=False)[0] == 0:
            break
        log = machine.execute(f"journalctl -u {unit} -o cat | tail -6", check_return=False)[1]
        if "globals presentation" in log:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the bench client never reported ready:\n"
                             + machine.succeed(f"journalctl -u {unit} -o cat | tail -20"))
    time.sleep(1)


def unit_alive(machine, unit):
    state = machine.execute(f"systemctl is-active {unit}", check_return=False)[1].strip()
    return state in ("active", "activating", "reloading")


def async_stop(machine, unit):
    machine.execute(f"systemctl stop {unit}", check_return=False)
    machine.execute(f"systemctl reset-failed {unit}", check_return=False)


def gated_latency(machine, runner, scenario, geometry, windows):
    runs = []
    setup = None
    moved = None
    visible = None
    focused = None
    for _ in range(REPEATS):
        machine.execute(f"rm -f {WAIT}", check_return=False)
        async_client(machine, runner, f"--windows {windows} --phase {scenario} "
                     f"--count {SAMPLES} --output {geometry['width']}x{geometry['height']} "
                     f"--wait {WAIT}", IDLE_UNIT)
        before = placement(runner.windows())
        if scenario == "drag":
            setup = runner.prepare_drag()
        time.sleep(0.4)
        rows = runner.windows()
        visible = len(before)
        focused = [row["app_id"] for row in rows if row.get("focused")]
        machine.succeed(f"touch {WAIT}")
        started = time.monotonic()
        time.sleep(1.5)
        if scenario == "drag":
            moved = placement(runner.windows()) != before
        while unit_alive(machine, IDLE_UNIT):
            if time.monotonic() - started > 240:
                raise AssertionError(f"the {scenario} phase did not finish")
            time.sleep(0.1)
        log = machine.succeed(f"journalctl -u {IDLE_UNIT} -o cat")
        async_stop(machine, IDLE_UNIT)
        parsed = parse(log)
        if not parsed:
            raise AssertionError(f"{runner.name} {scenario} produced no result:\n{log[-2000:]}")
        runs.append(parsed)
        time.sleep(0.3)
    merged = merge_runs(runs)
    merged["windows"] = windows
    merged["visible_windows"] = visible
    merged["focused"] = focused
    if scenario == "drag":
        merged["setup"] = setup
        merged["windows_moved_by_drag"] = bool(moved)
    return merged


def render(machine, runner, windows, geometry, hz, repeats=2):
    size = "200x140" if windows >= 50 else "480x320"
    runs = []
    for _ in range(repeats):
        async_client(machine, runner, f"--windows {windows} --size {size} --phase frames "
                     f"--seconds {FRAME_SECONDS} --output {geometry['width']}x{geometry['height']}",
                     IDLE_UNIT)
        before = cpu_ticks(machine, runner.pid)
        started = time.monotonic()
        while unit_alive(machine, IDLE_UNIT):
            if time.monotonic() - started > FRAME_SECONDS + 120:
                raise AssertionError("the frames phase did not finish")
            time.sleep(0.2)
        after = cpu_ticks(machine, runner.pid)
        log = machine.succeed(f"journalctl -u {IDLE_UNIT} -o cat")
        async_stop(machine, IDLE_UNIT)
        frames = counter(log, "frames")
        duration = counter(log, "frames_ns")
        lost = counter(log, "lost")
        if not frames or not duration:
            raise AssertionError(f"the frames phase reported no frames:\n{log[-2000:]}")
        cpu_ms = (after - before) * 1000.0 / hz
        runs.append({
            "frames": frames,
            "seconds": round(duration / 1e9, 3),
            "fps": round(frames / (duration / 1e9), 2),
            "cpu_ms_per_frame": round(cpu_ms / frames, 3),
            "cpu_pct_of_one_core": round(cpu_ms / (duration / 1e9) / 10, 1),
            "presented_lost": lost,
        })
        time.sleep(0.5)
    cpu = [run["cpu_ms_per_frame"] for run in runs]
    fps = [run["fps"] for run in runs]
    return {
        "windows": windows,
        "frames": runs[-1]["frames"],
        "fps": round(statistics.median(fps), 2),
        "fps_noise_pct": round((max(fps) - min(fps)) / statistics.median(fps) * 100, 1),
        "cpu_ms_per_frame": round(statistics.median(cpu), 3),
        "cpu_ms_per_frame_noise_pct": round((max(cpu) - min(cpu)) / statistics.median(cpu) * 100, 1),
        "cpu_pct_of_one_core": runs[-1]["cpu_pct_of_one_core"],
        "runs": runs,
    }


def idle(machine, runner, geometry, hz):
    runs = []
    for _ in range(IDLE_REPEATS):
        async_client(machine, runner, f"--windows 1 --phase idle --seconds {IDLE_SECONDS + 10} "
                     f"--output {geometry['width']}x{geometry['height']}", IDLE_UNIT)
        time.sleep(2)
        cpu_before = cpu_ticks(machine, runner.pid)
        switches_before = context_switches(machine, runner.pid)
        started = time.monotonic()
        time.sleep(IDLE_SECONDS)
        elapsed = time.monotonic() - started
        cpu_after = cpu_ticks(machine, runner.pid)
        switches_after = context_switches(machine, runner.pid)
        memory = smaps(machine, runner.pid)
        breakdown = runner.memory()
        async_stop(machine, IDLE_UNIT)
        cpu_ms = (cpu_after - cpu_before) * 1000.0 / hz
        runs.append({
            "seconds": round(elapsed, 1),
            "cpu_ms": round(cpu_ms, 1),
            "cpu_ms_per_s": round(cpu_ms / elapsed, 2),
            "wakeups_per_s": round((switches_after[0] - switches_before[0]) / elapsed, 2),
            "involuntary_switches_per_s": round(
                (switches_after[1] - switches_before[1]) / elapsed, 2),
            "memory": memory,
            "breakdown": breakdown,
        })
        time.sleep(1)
    cpu = [run["cpu_ms_per_s"] for run in runs]
    wakeups = [run["wakeups_per_s"] for run in runs]
    return {
        "seconds": runs[-1]["seconds"],
        "cpu_ms_per_s": round(statistics.median(cpu), 2),
        "cpu_ms_per_s_noise_pct": round((max(cpu) - min(cpu)) / statistics.median(cpu) * 100, 1)
        if statistics.median(cpu) else 0.0,
        "wakeups_per_s": round(statistics.median(wakeups), 2),
        "wakeups_per_s_noise_pct": round((max(wakeups) - min(wakeups))
                                         / statistics.median(wakeups) * 100, 1)
        if statistics.median(wakeups) else 0.0,
        "runs": runs,
        "memory": runs[-1]["memory"],
        "breakdown": runs[-1]["breakdown"],
    }


def windows_memory(machine, runner, geometry, windows):
    async_client(machine, runner, f"--windows {windows} --size 200x140 --phase idle --seconds 120 "
                 f"--output {geometry['width']}x{geometry['height']}", IDLE_UNIT)
    time.sleep(3)
    memory = smaps(machine, runner.pid)
    rows = runner.windows()
    breakdown = runner.memory()
    async_stop(machine, IDLE_UNIT)
    time.sleep(0.5)
    return {
        "windows": windows,
        "visible_windows": len([row for row in rows
                                if str(row.get("app_id", "")).startswith("bench-")]),
        "memory": memory,
        "breakdown": breakdown,
    }


def run_compositor(machine, paths, name, hz):
    runner = create(machine, paths, name)
    result = {"name": name, "started": False, "latency": {}, "render": {}, "idle": {},
              "memory": {}, "failures": []}
    began = time.monotonic()
    runner.start()
    result["started"] = True
    result["startup_s"] = round(time.monotonic() - began, 2)
    result["pid"] = runner.pid
    result["version"] = runner.version()
    geometry = runner.output()
    result["output"] = geometry
    result["bindings"] = {"focus": runner.focus_binding, "drag": runner.drag_binding}
    if not geometry.get("width"):
        raise AssertionError(f"{name} reported no output geometry")
    runner.bench(f"--windows 1 --phase move --count 20 "
                 f"--output {geometry['width']}x{geometry['height']}")
    for scenario, windows in (("move", 1), ("key", 2), ("drag", 1)):
        try:
            result["latency"][scenario] = gated_latency(machine, runner, scenario, geometry,
                                                        windows)
        except Exception as error:
            result["failures"].append({"phase": f"latency {scenario}", "error": str(error)})
            result["latency"][scenario] = {"status": "failed"}
        time.sleep(0.5)
    for windows in WINDOW_COUNTS:
        try:
            result["render"][str(windows)] = render(machine, runner, windows, geometry, hz)
        except Exception as error:
            result["failures"].append({"phase": f"render {windows}", "error": str(error)})
            result["render"][str(windows)] = {"status": "failed"}
    try:
        result["memory"]["windows_50"] = windows_memory(machine, runner, geometry, 50)
    except Exception as error:
        result["failures"].append({"phase": "memory 50", "error": str(error)})
        result["memory"]["windows_50"] = {"status": "failed"}
    try:
        result["idle"] = idle(machine, runner, geometry, hz)
    except Exception as error:
        result["failures"].append({"phase": "idle", "error": str(error)})
        result["idle"] = {"status": "failed"}
    if result["idle"].get("status") != "failed":
        result["memory"]["windows_1"] = {"windows": 1, "memory": result["idle"]["memory"],
                                         "breakdown": result["idle"]["breakdown"]}
    return runner, result


def formatted(value, noise=None):
    if isinstance(value, dict):
        if value.get("status") != "ok":
            return "none"
        field = "p50"
        return f"{value[field]:.2f}"
    if isinstance(value, float):
        text = f"{value:.2f}" if value < 100 else f"{value:.0f}"
        return f"{text}±{noise:.1f}%" if noise is not None else text
    return str(value)


ROWS = (
    ("move presented p50 ms", ("latency", "move", "presented"), "p50"),
    ("move event p50 ms", ("latency", "move", "event"), "p50"),
    ("move redraw p50 ms", ("latency", "move", "redraw"), "p50"),
    ("move presented p99 ms", ("latency", "move", "presented"), "p99"),
    ("key presented p50 ms", ("latency", "key", "presented"), "p50"),
    ("key focus event p50 ms", ("latency", "key", "event"), "p50"),
    ("key redraw p50 ms", ("latency", "key", "redraw"), "p50"),
    ("drag presented p50 ms", ("latency", "drag", "presented"), "p50"),
    ("drag event p50 ms", ("latency", "drag", "event"), "p50"),
    ("cpu/frame 1 window ms", ("render", "1", "cpu_ms_per_frame"), None),
    ("cpu/frame 10 windows ms", ("render", "10", "cpu_ms_per_frame"), None),
    ("cpu/frame 50 windows ms", ("render", "50", "cpu_ms_per_frame"), None),
    ("fps 50 windows", ("render", "50", "fps"), None),
    ("idle cpu ms/s", ("idle", "cpu_ms_per_s"), None),
    ("idle wakeups/s", ("idle", "wakeups_per_s"), None),
    ("rss 1 window MB", ("memory", "windows_1", "memory", "rss_mb"), None),
    ("pss 1 window MB", ("memory", "windows_1", "memory", "pss_mb"), None),
    ("pss 50 windows MB", ("memory", "windows_50", "memory", "pss_mb"), None),
)


def noise_for(path, entry):
    try:
        if path[0] == "latency":
            return entry["latency"][path[1]]["presented"]["noise_pct"]
        if path[0] == "render":
            return entry["render"][path[1]].get(f"{path[2]}_noise_pct")
        if path[0] == "idle":
            return entry["idle"].get(f"{path[1]}_noise_pct")
        if path[0] == "memory":
            memory = entry["memory"][path[1]]["memory"]
            key = path[-1].replace("_mb", "_noise_pct")
            return memory.get(key)
    except (KeyError, TypeError):
        return None
    return None


def table(scoreboard, names):
    label_width = max(len(row[0]) for row in ROWS) + 2
    widths = {name: max(len(name), 10) for name in names}
    lines = ["metric".ljust(label_width) + "".join(name.rjust(widths[name] + 2)
                                                   for name in names)]
    for label, path, field in ROWS:
        cells = []
        for name in names:
            entry = next((row for row in scoreboard if row["name"] == name), None)
            if entry is None:
                cells.append("failed")
                continue
            try:
                value = entry
                for key in path:
                    value = value[key]
                if isinstance(value, dict) and value.get("status") == "ok":
                    cells.append(f"{value[field]:.2f}")
                else:
                    cells.append(formatted(value, noise_for(path, entry)))
            except (KeyError, TypeError):
                cells.append("n/a")
        lines.append(label.ljust(label_width) + "".join(cell.rjust(widths[name] + 2)
                                                        for cell, name in zip(cells, names)))
    return lines


def main(machine, paths):
    machine.succeed("mkdir -p /run/bench/cache && chmod 755 /run/bench")
    cards = machine.succeed(
        "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done 2>/dev/null"
    ).split()
    paths["drm_device"] = f"/dev/dri/{cards[0]}" if cards else ""
    drm_outputs = machine.succeed("ls /sys/class/drm 2>/dev/null").split()
    hz = ticks_per_second(machine)
    names = [name.strip() for name in paths["compositors"].split(",") if name.strip()]
    scoreboard = []
    failures = {}
    for name in names:
        print(f"bench: measuring {name}", flush=True)
        runner = None
        try:
            runner, result = run_compositor(machine, paths, name, hz)
        except Exception as error:
            detail = str(error)
            if runner is not None:
                detail += "\n--- " + name + " log ---\n" + runner.log()
            failures[name] = detail
            print(f"bench: {name} failed: {error}", flush=True)
            if runner is not None:
                runner.stop()
            continue
        runner.stop()
        scoreboard.append(result)
        print(f"bench: {name} measured", flush=True)
        print("BENCH-ONE-BEGIN", flush=True)
        print(json.dumps({"benchmark": "tomoe-bench", "version": 1, "compositors": {name: result}},
                         indent=2, sort_keys=True), flush=True)
        print("BENCH-ONE-END", flush=True)
    document = {
        "benchmark": "tomoe-bench",
        "version": 1,
        "environment": {
            "vm_cores": paths["cores"],
            "vm_memory_mb": paths["memory"],
            "kernel": machine.succeed("uname -r").strip(),
            "drm_device": paths.get("drm_device", ""),
            "drm_devices": drm_outputs,
            "clock_ticks_per_second": hz,
            "renderer": "Mesa llvmpipe through zink, compositing to a vkms virtual KMS output",
            "input": "uinput evdev pointer and keyboard devices (real libinput path)",
            "repeat_runs": REPEATS,
            "samples_per_run": SAMPLES,
            "frame_seconds": FRAME_SECONDS,
            "idle_seconds": IDLE_SECONDS,
            "idle_repeats": IDLE_REPEATS,
        },
        "endpoints": {
            "event": "input injection to the compositor's input event reaching the client",
            "presented": "input injection with a client commit at the same moment, to the "
                         "wp_presentation timestamp of the frame that carried that commit",
            "redraw": "input injection to the client's frame callback for a repaint it commits "
                      "immediately after the injection, which is the frame-callback endpoint",
            "ranking": "compositors are compared on presented against presented only; event and "
                       "redraw are reported separately and never mixed into it",
        },
        "limitations": [
            "vkms is a software KMS device: every present is a simulated flip with no real "
            "vblank or scanout, so these are compositor pipeline numbers, not input-to-photon",
            "rendering is llvmpipe in a 4 core VM, so cpu-per-frame is CPU rasterisation cost "
            "and says nothing about GPU cost on real hardware",
            "power draw cannot be measured in a VM; idle cost is CPU time and context switches",
            "presented includes one client commit issued by the neutral client at injection "
            "time and excludes any real application's own work",
            "a compositor that reports no wp_presentation feedback for a phase is listed as "
            "none for that phase instead of falling back to another endpoint",
        ],
        "compositors": {entry["name"]: entry for entry in scoreboard},
        "failures": failures,
    }
    print("bench: table", flush=True)
    for line in table(scoreboard, names):
        print(line, flush=True)
    text = json.dumps(document, indent=2, sort_keys=True)
    encoded = base64.b64encode((text + "\n").encode()).decode()
    machine.succeed(f"printf %s {encoded} | base64 -d > /run/bench/scoreboard.json")
    print("BENCH-JSON-BEGIN", flush=True)
    print(text, flush=True)
    print("BENCH-JSON-END", flush=True)
    for name, error in failures.items():
        print(f"bench: {name} could not be measured: {error}", flush=True)
