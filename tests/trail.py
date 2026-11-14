#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

POLICY = """(in-package #:tomoe-user)

(define-extension "cursor-trail" (:reads (:outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (append
             (when output
               (list (configure-output (getf output :name)
                                       :mode (list 1280 720 {hz})
                                       :scale 1 :position '(0 0))))
             (list (shell-surface :cursor-trail (ui :stack)
                                  :anchors '(:top :right :bottom :left)
                                  :layer :overlay
                                  :click-through t
                                  :background '(:trail :cell {cell} :life {life}
                                                :steps {steps} :color #xb8e0ffff
                                                :fps {fps} :idle {idle}))))
            nil)))
"""

TOLERANCE = 6.0
MAXIMUM = 20.0
DRAGS = 6
COST_GROWTH = 1.5
COST_SLACK = 1.0
REFRESHES = (60, 240)
CAP_FPS = 60
CAP_GROWTH = 1.25
STEP_GROWTH = 1.5


class Failure(Exception):
    pass


def parse_frames(text):
    entries = {}
    for match in re.finditer(r':NAME "([^"]+)"([^\n]*)', text, re.IGNORECASE):
        fields = {key.lower(): value for key, value in
                  re.findall(r':([A-Za-z-]+)\s+([-\w]+)', match.group(2))}
        times = re.search(r':FRAME-TIMES \(([^)]*)\)', match.group(2), re.IGNORECASE)
        fields["frame-times"] = [int(value) for value in times.group(1).split()] if times else []
        recent = re.search(r':FRAME-US \(([^)]*)\)', match.group(2), re.IGNORECASE)
        fields["frame-us"] = [int(value) for value in recent.group(1).split()] if recent else []
        entries[match.group(1)] = fields
    return entries


class Compositor:
    def __init__(self, work, tomoe, trail=True, fps=0, idle=0.5, cell=9, life=0.35, steps=4, hz=60):
        self.tomoe = tomoe
        self.socket = "trail"
        self.runtime = os.path.join(work, "run")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.log_path = os.path.join(work, "tomoe.log")
        self.log = open(self.log_path, "w")
        policy = os.path.join(work, "policy.lisp")
        with open(policy, "w") as handle:
            handle.write(POLICY.format(cell=cell, life=life, steps=steps, fps=fps, idle=idle,
                                       hz=hz)
                         if trail else "(in-package #:tomoe-user)\n")
        self.environment = dict(os.environ)
        self.environment.update({"XDG_RUNTIME_DIR": self.runtime, "WAYLAND_DISPLAY": self.socket,
                                 "TOMOE_DEBUG": "1"})
        self.process = subprocess.Popen(
            [tomoe, "--socket", self.socket, "--backend", "headless", "--bare", "--no-watch",
             "--config", policy],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise Failure("tomoe exited while starting:\n" + self.text())
            if self.call("inspect")[0] == 0:
                return
            time.sleep(0.1)
        raise Failure("tomoe did not answer inspect:\n" + self.text())

    def call(self, *arguments):
        result = subprocess.run([self.tomoe, "--socket", self.socket, *arguments],
                                env=self.environment, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def text(self):
        if not self.log.closed:
            self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def frames(self):
        reply = self.call("frames")[1]
        parsed = parse_frames(reply)
        if not parsed:
            raise Failure("unreadable frames reply: " + reply[:400])
        return parsed

    def animation(self):
        reply = self.call("animation")[1]
        fields = {key.lower(): int(value) for key, value in
                  re.findall(r":([A-Za-z-]+)\s+(-?\d+)", reply)}
        times = re.search(r":TRAIL-TIMES \(([^)]*)\)", reply, re.IGNORECASE)
        fields["trail-times"] = [int(value) for value in times.group(1).split()] if times else []
        if "frames" not in fields or "period-ns" not in fields:
            raise Failure("unreadable animation reply: " + reply[:400])
        return fields

    def cost(self, before, after):
        rows = {}
        for name, fields in after.items():
            old = before.get(name)
            if not old:
                continue
            frames = int(fields.get("frame", 0)) - int(old.get("frame", 0))
            nanos = int(fields.get("frame-ns", 0)) - int(old.get("frame-ns", 0))
            if frames:
                rows[name] = {"frames": frames, "ms": round(nanos / frames / 1e6, 3)}
        return rows

    def drawn(self, before, after):
        return sum(row["frames"] for row in self.cost(before, after).values())

    def wakeups(self):
        with open(f"/proc/{self.process.pid}/status") as handle:
            status = handle.read()
        return sum(int(find) for find in re.findall(r"ctxt_switches:\s+(\d+)", status))

    def screenshot(self, path):
        result = subprocess.run(["grim", "-t", "ppm", path], env=self.environment,
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise Failure("grim failed: " + result.stderr.strip())

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    parts = data.split(b"\n", 3)
    width, height = (int(value) for value in parts[1].split())
    return width, height, parts[3]


def lit_pixels(path, threshold=20, step=2):
    width, height, pixels = read_ppm(path)
    points = []
    span = width * 3
    for y in range(0, height, step):
        row = pixels[y * span:(y + 1) * span]
        if max(row) <= threshold:
            continue
        for x in range(0, width, step):
            at = x * 3
            if row[at] > threshold or row[at + 1] > threshold or row[at + 2] > threshold:
                points.append((x, y))
    return points


def write_png(source, target):
    width, height, pixels = read_ppm(source)
    stride = width * 3
    raw = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    with open(target, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        handle.write(chunk(b"IEND", b""))


def segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    along = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return math.hypot(px - (ax + dx * along), py - (ay + dy * along))


def path_distance(point, path):
    return min(segment_distance(point[0], point[1], *a, *b) for a, b in zip(path, path[1:]))


def measure(points, path):
    on_path = [path_distance(point, path) for point in points]
    off_chord = [segment_distance(x, y, *path[0], *path[-1]) for x, y in points]
    return {
        "lit": len(points),
        "path_mean": round(sum(on_path) / len(on_path), 2),
        "path_max": round(max(on_path), 2),
        "chord_mean": round(sum(off_chord) / len(off_chord), 2),
        "off_chord": sum(1 for value in off_chord if value > 20),
    }


def encode_path(points):
    return ";".join(f"{x},{y}" for x, y in points)


def arc_points(cx, cy, radius, start, end, count):
    return [(round(cx + radius * math.cos(start + (end - start) * i / count)),
             round(cy + radius * math.sin(start + (end - start) * i / count)))
            for i in range(count + 1)]


def drag(compositor, path, steps, interval_ms, burst, hold_ms=400):
    arguments = [compositor.pointer, "--path", encode_path(path), "--steps", str(steps),
                 "--interval-ms", str(interval_ms), "--hold-ms", str(hold_ms),
                 "--delay-ms", "50", "--extent", "1280x720"]
    if burst:
        arguments.append("--burst")
    result = subprocess.run(arguments, env=compositor.environment, capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure("pointer-path failed: " + result.stderr.strip())


def drag_async(compositor, path, steps, interval_ms, burst, hold_ms=0):
    """Start a drag client and return once it has released the button, so the
    compositor's own frames during the pointer's :idle window can be read
    before the client's tail sleep ends."""
    arguments = [compositor.pointer, "--path", encode_path(path), "--steps", str(steps),
                 "--interval-ms", str(interval_ms), "--hold-ms", str(hold_ms),
                 "--delay-ms", "50", "--extent", "1280x720"]
    if burst:
        arguments.append("--burst")
    process = subprocess.Popen(arguments, env=compositor.environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    released = False
    for line in process.stdout:
        if line.strip() == "released":
            released = True
            break
    if not released:
        process.wait(timeout=30)
        raise Failure("pointer-path failed: " + (process.stderr.read() or "no release"))
    return process


def park(compositor, path):
    drag(compositor, [path[0], (path[0][0] + 1, path[0][1])], 1, 16, False, 100)
    time.sleep(1.2)


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def summary(values, areas=()):
    return {"frames": len(values),
            "median": round(percentile(values, 0.5), 3),
            "p99": round(percentile(values, 0.99), 3),
            "worst": round(max(values), 3) if values else 0.0,
            "pixels": round(percentile(areas, 0.5)) if areas else 0}


def drag_frames(compositor, path, steps, interval, burst):
    before = compositor.frames()
    drag(compositor, path, steps, interval, burst, 0)
    after = compositor.frames()
    times = [value / 1000.0 for fields in after.values() for value in fields["frame-us"]]
    drawn = sum(int(fields.get("frame", 0)) - int(before[name].get("frame", 0))
                for name, fields in after.items() if name in before)
    pixels = max((int(fields.get("drawn-pixels", 0)) for fields in after.values()), default=0)
    return times, (pixels if drawn else 0)


def drag_cost(work, tomoe, pointer, trail, stroke, verbose=False):
    tag = "cost-trail" if trail else "cost-plain"
    directory = os.path.join(work, tag)
    os.makedirs(directory, exist_ok=True)
    compositor = Compositor(directory, tomoe, trail)
    compositor.pointer = pointer
    compositor.work = directory
    frames, areas = [], []
    try:
        time.sleep(1.0)
        park(compositor, stroke)
        for _ in range(DRAGS):
            times, area = drag_frames(compositor, stroke, 6, 5, True)
            frames += times
            if area:
                areas.append(round(area))
            time.sleep(0.8)
    finally:
        compositor.stop()
        if verbose:
            print(directory + ":\n" + compositor.text()[-6000:], file=sys.stderr)
    return frames, areas


class Recorder:
    def __init__(self, compositor, work, tag, keep=False):
        self.compositor, self.work, self.tag = compositor, work, tag
        self.keep = keep
        self.lit = []
        self.best = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def loop(self):
        index = 0
        while not self.stop.is_set():
            frame = os.path.join(self.work, f"{self.tag}-{index:03d}.ppm")
            try:
                self.compositor.screenshot(frame)
            except Failure:
                break
            points = lit_pixels(frame)
            self.lit.append(len(points))
            if self.keep:
                if self.best is None or len(points) > self.best[1]:
                    if self.best is not None:
                        os.unlink(self.best[0])
                    self.best = (frame, len(points), points)
                else:
                    os.unlink(frame)
            else:
                os.unlink(frame)
            index += 1
            time.sleep(0.01)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join()


def trace(compositor, path, steps, interval_ms, burst, tag):
    with Recorder(compositor, compositor.work, tag, keep=True) as recorder:
        time.sleep(0.3)
        before = compositor.frames()
        drag(compositor, path, steps, interval_ms, burst)
        after = compositor.frames()
        time.sleep(0.2)
    if not recorder.best:
        return None, None, 0
    frame, _, points = recorder.best
    return frame, measure(points, path), len(recorder.lit)


def steps_of(times, period):
    """Summarize one run of trail steps against the display's own period."""
    steps = len(times)
    span = times[-1] - times[0] if steps > 1 else 0
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    refreshes = span / period if period else 0.0
    return {"steps": steps, "span-ns": span,
            "steps-per-refresh": round(steps / refreshes, 3) if refreshes else 0.0,
            "step-hz": round(steps / (span / 1e9), 1) if span else 0.0,
            "median-gap-ns": gaps[len(gaps) // 2] if gaps else 0,
            "max-gap-ns": gaps[-1] if gaps else 0}


def paced(work, tomoe, pointer, path, hz, fps=0):
    """Drag once on an output at HZ and read how the trail paced its steps.

    The trail steps during the drag and, while the pointer rests inside its
    :idle window, on the frames the compositor produces for it alone; the
    second run is what a refresh-rate gate or a :fps cap decides."""
    tag = f"refresh-{hz}" if not fps else f"refresh-{hz}-cap-{fps}"
    directory = os.path.join(work, tag)
    os.makedirs(directory, exist_ok=True)
    compositor = Compositor(directory, tomoe, True, fps=fps, hz=hz)
    compositor.pointer = pointer
    compositor.work = directory
    try:
        time.sleep(1.0)
        compositor.animation()
        process = drag_async(compositor, path, 2, 6, False)
        moving = compositor.animation()
        started = time.monotonic()
        time.sleep(0.3)
        resting = compositor.animation()
        elapsed = time.monotonic() - started
        process.wait(timeout=30)
    finally:
        compositor.stop()
    period = resting["period-ns"]
    return {"hz": hz, "fps": fps, "refresh-mhz": resting["refresh-mhz"], "period-ns": period,
            "moving": steps_of(moving["trail-times"], period),
            "resting": steps_of(resting["trail-times"], period),
            "resting-seconds": round(elapsed, 3),
            "resting-hz": round(len(resting["trail-times"]) / elapsed, 1) if elapsed else 0.0,
            "lead-min-ns": resting["lead-min-ns"]}


def judge_pace(entry, failures):
    period, hz, fps = entry["period-ns"], entry["hz"], entry["fps"]
    if entry["refresh-mhz"] != hz * 1000:
        failures.append(f"refresh {hz}: the output runs at {entry['refresh-mhz'] / 1000:.0f} Hz")
    if not period:
        failures.append(f"refresh {hz}: the compositor reported no refresh period")
        return
    moving, resting = entry["moving"], entry["resting"]
    if not fps and not moving["steps"]:
        failures.append(f"refresh {hz}: the trail drew no steps while the pointer moved")
    elif not fps and moving["steps-per-refresh"] < 0.9:
        failures.append(f"refresh {hz}: the trail took {moving['steps']} steps over"
                        f" {moving['span-ns'] / period:.1f} refreshes while the pointer moved,"
                        " so some refresh showed a stale trail")
    if not resting["steps"]:
        failures.append(f"refresh {hz}: the trail stopped stepping while the pointer rested"
                        " inside its :idle window")
        return
    if fps:
        if entry["resting-hz"] > CAP_FPS * CAP_GROWTH:
            failures.append(f"cap: a :fps {fps} trail stepped {entry['resting-hz']} times a"
                            f" second on a {hz} Hz output instead of capping at {CAP_FPS}")
        if resting["steps-per-refresh"] >= 0.5:
            failures.append(f"cap: a :fps {fps} trail still advanced on"
                            f" {resting['steps-per-refresh']} of every refresh")
        return
    if resting["steps-per-refresh"] < 0.9:
        failures.append(f"refresh {hz}: resting inside its :idle window, the trail took"
                        f" {resting['steps']} steps over {resting['span-ns'] / period:.1f}"
                        " refreshes, so some refresh showed a stale trail")
    if resting["median-gap-ns"] > period * STEP_GROWTH:
        failures.append(f"refresh {hz}: the trail stepped every"
                        f" {resting['median-gap-ns'] / 1e6:.2f} ms at the median, more than one"
                        f" refresh ({period / 1e6:.2f} ms)")
    if entry["lead-min-ns"] <= period // 2:
        failures.append(f"refresh {hz}: the trail was timed only"
                        f" {entry['lead-min-ns'] / 1e6:.2f} ms ahead of the frame start, not at"
                        " the moment the frame is shown")
    if hz > 60 and entry["resting-hz"] <= 60 * STEP_GROWTH:
        failures.append(f"refresh {hz}: the trail stepped {entry['resting-hz']} times a second"
                        " while the pointer rested, still gated at 60 fps instead of following"
                        " the display")


def main():
    parser = argparse.ArgumentParser(description="Cursor trail path capture, cost and refresh"
        " pacing check: the trail steps once per display refresh, costs the cells it lights,"
        " and draws nothing at rest.")
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--pointer", required=True)
    parser.add_argument("--trail", action="store_true",
                        help="draw the cursor trail; omit for a plain run")
    parser.add_argument("--work", default=None)
    parser.add_argument("--emit", default=None, help="directory for PNG artifacts")
    parser.add_argument("--report", default=None)
    parser.add_argument("--fps", type=int, default=0,
                        help="a cap on the trail's refresh-rate steps; 0 follows the display")
    parser.add_argument("--idle", type=float, default=0.5)
    parser.add_argument("--cell", type=float, default=9.0)
    parser.add_argument("--life", type=float, default=0.35)
    parser.add_argument("--steps", type=int, default=4)
    parser.add_argument("--verbose", action="store_true", help="print each compositor log")
    arguments = parser.parse_args()
    work = arguments.work or tempfile.mkdtemp(prefix="trail-")
    os.makedirs(work, exist_ok=True)
    report, failures = {}, []

    compositor = Compositor(work, arguments.tomoe, arguments.trail, arguments.fps,
                            arguments.idle, arguments.cell, arguments.life, arguments.steps)
    compositor.pointer = arguments.pointer
    compositor.work = work
    arc = arc_points(640, 440, 300, 3.2, 6.0, 48)
    zigzag = [(280, 520), (440, 200), (600, 520), (760, 200), (920, 520)]
    try:
        time.sleep(1.0)
        baseline = os.path.join(work, "idle.ppm")
        compositor.screenshot(baseline)
        report["idle_lit"] = len(lit_pixels(baseline))

        if not arguments.trail:
            park(compositor, arc)
            frame, _, _ = trace(compositor, arc, 2, 6, False, "plain")
            settled = os.path.join(work, "plain-settled.ppm")
            compositor.screenshot(settled)
            report["plain"] = {"digest": hashlib.sha256(open(settled, "rb").read()).hexdigest(),
                               "identical_to_idle": open(settled, "rb").read() ==
                               open(baseline, "rb").read()}
            if arguments.emit and frame:
                os.makedirs(arguments.emit, exist_ok=True)
                write_png(frame, os.path.join(arguments.emit, "plain.png"))
            print(json.dumps(report, indent=2))
            return 1 if failures else 0

        before, wakes = compositor.frames(), compositor.wakeups()
        time.sleep(2.0)
        after, wakes = compositor.frames(), compositor.wakeups() - wakes
        report["idle"] = {"seconds": 2.0, "drawn": compositor.drawn(before, after), "wakes": wakes}
        if report["idle"]["drawn"]:
            failures.append(f"idle: the overlay drew {report['idle']['drawn']} frames while the pointer was still")

        motions = [
            ("arc", arc, 2, 6, False),
            ("burst-arc", arc, 1, 5, True),
            ("burst-zigzag", zigzag, 6, 5, True),
        ]
        for tag, path, steps, interval, burst in motions:
            park(compositor, path)
            frame, stats, shots = trace(compositor, path, steps, interval, burst, tag)
            if not frame:
                report[tag] = {"shots": shots}
                failures.append(f"{tag}: the motion drew no trail")
                continue
            report[tag] = {"shots": shots, "stats": stats}
            if stats["path_mean"] > TOLERANCE or stats["path_max"] > MAXIMUM:
                failures.append(f"{tag}: the trail is {stats['path_mean']}px from the path it was "
                                f"dragged along (worst {stats['path_max']}px), not following it")
            if arguments.emit:
                os.makedirs(arguments.emit, exist_ok=True)
                write_png(frame, os.path.join(arguments.emit, tag + ".png"))

        park(compositor, zigzag)
        with Recorder(compositor, work, "fade") as recorder:
            time.sleep(0.2)
            drag(compositor, zigzag, 6, 8, False, 0)
        fades = recorder.lit
        report["fade"] = fades
        if not fades or max(fades) < 200:
            failures.append(f"fade: no trail was drawn to fade ({len(fades)} frames)")
        else:
            peak = fades.index(max(fades))
            if fades[-1]:
                failures.append(f"fade: the trail was still lit ({fades[-1]} px) after the pointer stopped")
            grown = [step for step, (was, now) in enumerate(zip(fades[peak:], fades[peak + 1:]))
                     if now > was + 50]
            if grown:
                failures.append(f"fade: the trail grew after the pointer stopped ({grown[:4]})")

        settled = os.path.join(work, "settled.ppm")
        compositor.screenshot(settled)
        report["settled"] = {"lit": len(lit_pixels(settled)),
                             "identical_to_idle": open(settled, "rb").read() ==
                             open(baseline, "rb").read()}
        if report["settled"]["lit"]:
            failures.append("settled: the trail left pixels behind after fading")
        before, wakes = compositor.frames(), compositor.wakeups()
        time.sleep(1.5)
        after, wakes = compositor.frames(), compositor.wakeups() - wakes
        report["rest"] = {"seconds": 1.5, "drawn": compositor.drawn(before, after), "wakes": wakes}
        if report["rest"]["drawn"]:
            failures.append(f"rest: the overlay drew {report['rest']['drawn']} frames with the pointer still")

        compositor.stop()
        for hz in REFRESHES:
            entry = paced(work, arguments.tomoe, arguments.pointer, arc, hz)
            report[f"refresh-{hz}"] = entry
            judge_pace(entry, failures)
        entry = paced(work, arguments.tomoe, arguments.pointer, arc, REFRESHES[-1], CAP_FPS)
        report["cap"] = entry
        judge_pace(entry, failures)
        uncapped = report.get(f"refresh-{REFRESHES[-1]}", {}).get("resting-hz", 0)
        if uncapped <= 2 * CAP_GROWTH * CAP_FPS:
            failures.append("refresh pacing: the uncapped trail did not step faster than the"
                            f" :fps {CAP_FPS} cap, so the cap was not what limited it")
        on, on_areas = drag_cost(work, arguments.tomoe, arguments.pointer, True, zigzag,
                                 arguments.verbose)
        off, off_areas = drag_cost(work, arguments.tomoe, arguments.pointer, False, zigzag)
        report["cost"] = {"on": summary(on, on_areas), "off": summary(off, off_areas)}
        if len(on) < DRAGS or len(off) < DRAGS:
            failures.append(f"cost: too few frames to compare ({len(on)} with the trail, "
                            f"{len(off)} without)")
        carried = report["cost"]["on"]["median"]
        plain = report["cost"]["off"]["median"]
        if carried > plain * COST_GROWTH + COST_SLACK:
            failures.append(f"cost: with the trail a drag frame took {carried} ms at the median,"
                            f" against {plain} ms without it")
    finally:
        compositor.stop()
        report["failures"] = failures
        print(json.dumps(report, indent=2))
        if arguments.report:
            with open(arguments.report, "w") as handle:
                json.dump(report, handle, indent=2)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
