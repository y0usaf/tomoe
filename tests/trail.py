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

(define-extension "cursor-trail" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (shell-surface :cursor-trail (ui :stack)
                               :anchors '(:top :right :bottom :left)
                               :layer :overlay
                               :click-through t
                               :background '(:shader "{shader}" :fps {fps} :idle {idle})))
          nil))
"""

TOLERANCE = 6.0
MAXIMUM = 20.0


class Failure(Exception):
    pass


def parse_frames(text):
    entries = {}
    for match in re.finditer(r':NAME "([^"]+)"([^\n]*)', text, re.IGNORECASE):
        fields = {key.lower(): value for key, value in
                  re.findall(r':([A-Za-z-]+)\s+([-\w]+)', match.group(2))}
        times = re.search(r':FRAME-TIMES \(([^)]*)\)', match.group(2), re.IGNORECASE)
        fields["frame-times"] = [int(value) for value in times.group(1).split()] if times else []
        entries[match.group(1)] = fields
    return entries


class Compositor:
    def __init__(self, work, tomoe, shader, fps=60, idle=0.5):
        self.tomoe = tomoe
        self.socket = "trail"
        self.runtime = os.path.join(work, "run")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.log_path = os.path.join(work, "tomoe.log")
        self.log = open(self.log_path, "w")
        policy = os.path.join(work, "policy.lisp")
        with open(policy, "w") as handle:
            handle.write(POLICY.format(shader=shader, fps=fps, idle=idle) if shader
                         else "(in-package #:tomoe-user)\n")
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
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def frames(self):
        reply = self.call("frames")[1]
        parsed = parse_frames(reply)
        if not parsed:
            raise Failure("unreadable frames reply: " + reply[:400])
        return parsed

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


def park(compositor, path):
    drag(compositor, [path[0], (path[0][0] + 1, path[0][1])], 1, 16, False, 100)
    time.sleep(1.2)


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


def main():
    parser = argparse.ArgumentParser(description="Cursor trail path capture and cost check.")
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--pointer", required=True)
    parser.add_argument("--shader", default=None, help="trail shader; omit for a plain run")
    parser.add_argument("--work", default=None)
    parser.add_argument("--emit", default=None, help="directory for PNG artifacts")
    parser.add_argument("--report", default=None)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--idle", type=float, default=0.5)
    arguments = parser.parse_args()
    work = arguments.work or tempfile.mkdtemp(prefix="trail-")
    os.makedirs(work, exist_ok=True)
    report, failures = {}, []

    compositor = Compositor(work, arguments.tomoe, arguments.shader, arguments.fps, arguments.idle)
    compositor.pointer = arguments.pointer
    compositor.work = work
    arc = arc_points(640, 440, 300, 3.2, 6.0, 48)
    zigzag = [(280, 520), (440, 200), (600, 520), (760, 200), (920, 520)]
    try:
        time.sleep(1.0)
        baseline = os.path.join(work, "idle.ppm")
        compositor.screenshot(baseline)
        report["idle_lit"] = len(lit_pixels(baseline))

        if not arguments.shader:
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
