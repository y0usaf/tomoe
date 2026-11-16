#!/usr/bin/env python3

import argparse
import os
import pathlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

SHAPES = ("default context-menu help pointer progress wait cell crosshair text "
          "vertical-text alias copy move no-drop not-allowed grab grabbing e-resize "
          "n-resize ne-resize nw-resize s-resize se-resize sw-resize w-resize ew-resize "
          "ns-resize nesw-resize nwse-resize col-resize row-resize all-scroll zoom-in "
          "zoom-out dnd-ask all-resize").split()

ARROW_WIDTH = 12
ARROW_HEIGHT = 16

SETTINGS = """
(defparameter +test-settle+ 0.08)
"""

PARK = (400, 380)
ZIGZAG = [(PARK[0] + 70, PARK[1] - 50), (PARK[0] - 70, PARK[1] + 50)] * 6 + [PARK]

THROWAWAY = """
(defparameter +jelly+ (list :rotation (list :speed 0.035d0 :above 1d0 :below 1000d0)
                            :settle +test-settle+))
"""

TILT = """
(defparameter +tilt+ (list :rotation (list :vx 0.009d0 :below 5000d0)
                           :settle +test-settle+))
"""

STRETCH = """
(defparameter +stretch+ (list :stretch (list :speed (/ 1d0 4000d0) :above 200d0 :below 4000d0)
                              :stretch-angle (list -90d0 (list :angle 1d0))
                              :settle +test-settle+))
"""

SHAKE = """
(defparameter +shake+ (list :scale (list :shake 0.5d0 :above 6d0 :below 8d0) :settle 0.2d0))

(define-extension "test-shake" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "shake" +shake+ :order 20)) nil))
"""

SPRING = """
(defparameter +spring+ (list :rotation (list :vx 0.01d0 :below 3000d0) :settle 0.3d0))

(define-extension "test-spring" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "spring" +spring+ :order 5)) nil))
"""

TRAIL = """
(defparameter +trail+ (list :settle +test-settle+
                            :trail (list :cell 9d0 :life 0.35d0 :steps 4
                                         :color "#b8e0ff")))

(define-extension "test-trail" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "trail" +trail+ :order 30)) nil))
"""


class Failure(Exception):
    pass


def theme_cursor(color):
    pixels = []
    for y in range(ARROW_HEIGHT):
        for x in range(ARROW_WIDTH):
            lit = x <= min(y, 8) or (y >= 12 and x >= ARROW_WIDTH - 5)
            pixels.append(color if lit else 0)
    header = struct.pack("<4I3I9I", 0x72756358, 16, 0x10000, 1,
                         0xfffd0002, 24, 28,
                         36, 0xfffd0002, 24, 1, ARROW_WIDTH, ARROW_HEIGHT, 0, 0, 0)
    return header + struct.pack("<%dI" % (ARROW_WIDTH * ARROW_HEIGHT), *pixels)


BLUE, RED = 0xff0000ff, 0xffff0000


def write_theme(root):
    for variant, base in (("check-blue", BLUE), ("check-red", RED)):
        theme = root / "icons" / variant / "cursors"
        theme.mkdir(parents=True, exist_ok=True)
        for index, name in enumerate(SHAPES, 1):
            (theme / name).write_bytes(theme_cursor(base | (index << 8)))
    relink_theme(root / "icons", "check-blue")
    return str(root / "icons")


def relink_theme(icons, variant):
    link = icons / "check"
    staged = icons / ".check-relink"
    if staged.is_symlink():
        staged.unlink()
    staged.symlink_to(variant)
    os.replace(staged, link)


CONFIGURE = """
(define-extension "test-output" (:reads (:outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (when output
              (list (configure-output (getf output :name)
                                      :mode (list 1280 720 60) :scale 1
                                      :position '(0 0))))
            nil)))
"""


class Compositor:
    def __init__(self, work, tomoe, pointer, body, bare=True):
        self.tomoe = tomoe
        self.pointer = pointer
        self.socket = "cursor"
        self.root = pathlib.Path(work)
        self.runtime = self.root / "run"
        self.runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.policy = self.root / "policy.lisp"
        self.write_policy(body)
        self.log_path = self.root / "tomoe.log"
        self.log = open(self.log_path, "w")
        self.environment = {
            key: os.environ[key] for key in
            ("PATH", "HOME", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_DRIVERS_PATH",
             "__EGL_VENDOR_LIBRARY_FILENAMES", "LD_LIBRARY_PATH", "LIBGL_ALWAYS_SOFTWARE")
            if key in os.environ}
        self.icons = str(self.root.parent / "icons")
        self.environment.update(
            XDG_RUNTIME_DIR=str(self.runtime), WAYLAND_DISPLAY=self.socket,
            XCURSOR_PATH=self.icons, XCURSOR_THEME="check", TOMOE_DEBUG="1")
        arguments = [tomoe, "--socket", self.socket, "--backend", "headless", "--no-watch"]
        if bare:
            arguments.append("--bare")
        arguments += ["--config", str(self.policy)]
        self.process = subprocess.Popen(arguments, env=self.environment,
                                        stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise Failure("tomoe exited while starting:\n" + self.text())
            if self.call("inspect")[0] == 0:
                return
            time.sleep(0.1)
        raise Failure("tomoe did not answer inspect:\n" + self.text())

    def write_policy(self, body):
        text = "(in-package #:tomoe-user)\n" + SETTINGS + body
        with open(self.policy, "w") as handle:
            handle.write(text)

    def call(self, *arguments):
        result = subprocess.run([self.tomoe, "--socket", self.socket, *arguments],
                                env=self.environment, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def text(self):
        if self.log.closed:
            return ""
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def frames(self):
        reply = self.call("frames")[1]
        total = 0
        for match in re.finditer(r':NAME "([^"]+)"([^\n]*)', reply, re.IGNORECASE):
            fields = {key.lower(): value for key, value in
                      re.findall(r':([A-Za-z-]+)\s+([-\w]+)', match.group(2))}
            total += int(fields.get("frame", 0))
        if not total and "frame" not in reply.lower():
            raise Failure("unreadable frames reply: " + reply[:300])
        return total

    def wakeups(self):
        with open("/proc/%d/status" % self.process.pid) as handle:
            status = handle.read()
        return sum(int(value) for value in re.findall(r"ctxt_switches:\s+(\d+)", status))

    def screenshot(self, name):
        path = str(self.root / name)
        for attempt in range(4):
            try:
                result = subprocess.run(["grim", "-c", "-t", "ppm", path], env=self.environment,
                                        capture_output=True, text=True, timeout=10)
            except subprocess.TimeoutExpired:
                self.stroke([(400, 380 + attempt), (401, 380 + attempt)], steps=1, hold_ms=20)
                time.sleep(0.2)
                continue
            if result.returncode != 0:
                raise Failure("grim failed: " + result.stderr.strip())
            return path
        raise Failure("grim never received a frame")

    def stroke(self, path, steps=8, interval_ms=8, hold_ms=0):
        encoded = ";".join("%d,%d" % point for point in path)
        result = subprocess.run(
            [self.pointer, "--path", encoded, "--steps", str(steps),
             "--interval-ms", str(interval_ms), "--hold-ms", str(hold_ms),
             "--delay-ms", "50", "--extent", "1280x720"],
            env=self.environment, capture_output=True, text=True)
        if result.returncode != 0:
            raise Failure("pointer-path failed: " + result.stderr.strip())

    def stroke_async(self, path, steps=8, interval_ms=8, hold_ms=0):
        encoded = ";".join("%d,%d" % point for point in path)
        return subprocess.Popen(
            [self.pointer, "--path", encoded, "--steps", str(steps),
             "--interval-ms", str(interval_ms), "--hold-ms", str(hold_ms),
             "--delay-ms", "50", "--extent", "1280x720"],
            env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)

    def stroke_until(self, path, word, steps=8, interval_ms=8):
        mover = self.stroke_async(path, steps=steps, interval_ms=interval_ms)
        for line in mover.stdout:
            if line.strip().startswith(word):
                return mover
        mover.wait()
        raise Failure("pointer-path ended before it " + word + ": " + mover.stderr.read().strip())

    def reload(self):
        code, output = self.call("reload")
        if code != 0:
            raise Failure("reload failed: " + output)

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


def lit_points(path, threshold=25):
    width, height, pixels = read_ppm(path)
    span = width * 3
    points = []
    for y in range(height):
        row = pixels[y * span:(y + 1) * span]
        if max(row) <= threshold:
            continue
        for x in range(0, width):
            at = x * 3
            if row[at] > threshold or row[at + 1] > threshold or row[at + 2] > threshold:
                points.append((x, y))
    return points


def signature(path):
    points = lit_points(path)
    if not points:
        return (0, ())
    left = min(point[0] for point in points)
    top = min(point[1] for point in points)
    return (len(points), tuple(sorted((point[0] - left, point[1] - top) for point in points)))


def tint(path, x, y, radius=24):
    width, _, pixels = read_ppm(path)
    red = blue = 0
    for px, py in near(lit_points(path), x, y, radius):
        at = (py * width + px) * 3
        red += pixels[at]
        blue += pixels[at + 2]
    return red, blue


def near(points, x, y, radius):
    return [point for point in points
            if abs(point[0] - x) <= radius and abs(point[1] - y) <= radius]


def same(a, b):
    return signature(a) == signature(b)


def case(compositor, path, steps=16, interval_ms=20):
    shots = []
    stop = threading.Event()

    def capture():
        while not stop.is_set():
            try:
                shots.append(compositor.screenshot("frame-%03d.ppm" % len(shots)))
            except Failure:
                return
            time.sleep(0.012)

    park(compositor)
    rest = compositor.screenshot("rest.ppm")
    thread = threading.Thread(target=capture, daemon=True)
    thread.start()
    time.sleep(0.1)
    compositor.stroke(path, steps=steps, interval_ms=interval_ms)
    time.sleep(0.05)
    stop.set()
    thread.join()
    time.sleep(0.9)
    settled = compositor.screenshot("settled.ppm")
    return rest, shots, settled


def park(compositor):
    compositor.stroke([(PARK[0] - 24, PARK[1]), PARK], steps=6, hold_ms=120)
    time.sleep(0.7)


def idle(build, seconds=2.0):
    before = build.frames()
    wakeups = build.wakeups()
    time.sleep(seconds)
    drawn = build.frames() - before
    return drawn, build.wakeups() - wakeups


def main():
    parser = argparse.ArgumentParser(description="Dynamic cursor proof.")
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--pointer", required=True)
    parser.add_argument("--work", default=None)
    arguments = parser.parse_args()
    if not shutil.which("grim"):
        raise Failure("grim must be on PATH")

    root = pathlib.Path(arguments.work or tempfile.mkdtemp(prefix="tomoe-cursor-"))
    root.mkdir(parents=True, exist_ok=True)
    write_theme(root)
    tomoe = os.path.abspath(arguments.tomoe)
    pointer = os.path.abspath(arguments.pointer)
    failures = []

    def build(name, body, bare=True):
        work = root / name
        work.mkdir(exist_ok=True)
        compositor = Compositor(str(work), tomoe, pointer, CONFIGURE + body,
                                bare=bare)
        compositor.icons = str(root / "icons")
        return compositor

    plain = build("plain", "")
    try:
        time.sleep(1.0)
        park(plain)
        reference = plain.screenshot("plain-rest.ppm")
        plain_shape = signature(reference)
        moving = plain.stroke_async([(PARK[0], PARK[1]), (900, PARK[1])], steps=8,
                                    interval_ms=10)
        frames = []
        while moving.poll() is None:
            frames.append(plain.screenshot("walk-%03d.ppm" % len(frames)))
            time.sleep(0.02)
        time.sleep(0.6)
        if any(signature(frame)[:1] != plain_shape[:1] for frame in frames):
            failures.append("plain: the cursor changed size with no cursor behaviour")
        if any(signature(frame) != plain_shape for frame in frames):
            failures.append("plain: the cursor was deformed with no cursor behaviour")
        plain.stroke([(PARK[0], PARK[1]), (PARK[0] + 1, PARK[1])], steps=1, hold_ms=80)
        time.sleep(0.6)
        drawn, wakeups = idle(plain)
        print("plain: %d frames and %d wakeups while idle" % (drawn, wakeups))
        if drawn:
            failures.append("plain drew %d frames while idle" % drawn)
        plain_wakeups = wakeups
        for variant, want in (("check-red", "red"), ("check-blue", "blue")):
            relink_theme(root / "icons", variant)
            time.sleep(0.4)
            red, blue = tint(plain.screenshot("theme-%s.ppm" % want), *PARK)
            print("theme %s: red %d blue %d at the still pointer" % (variant, red, blue))
            if (red > blue) != (want == "red"):
                failures.append("plain: relinking the theme to %s left the still cursor %s"
                                % (variant, "red" if red > blue else "blue"))
    finally:
        plain.stop()
        for path in (root / "plain").glob("walk-*.ppm"):
            path.unlink()

    def check(name, body, expect, path=((PARK[0], PARK[1]), (900, PARK[1])), steps=16,
               interval_ms=20):
        work = root / name
        compositor = build(name, body)
        try:
            time.sleep(1.0)
            rest, shots, settled = case(compositor, list(path), steps, interval_ms)
            if not same(rest, reference):
                failures.append(name + ": the resting cursor is not the plain image")
            if not any(not same(shot, reference) for shot in shots):
                failures.append(name + ": the cursor never deformed while moving")
            if not same(settled, reference):
                failures.append(name + ": the cursor did not come back to the plain image")
            drawn, wakeups = idle(compositor)
            print("%s: %d captured frames, %d idle frames, %d idle wakeups"
                  % (name, len(shots), drawn, wakeups))
            if drawn:
                failures.append(name + ": drew %d frames while idle" % drawn)
            if wakeups > plain_wakeups + 2:
                failures.append("%s: %d idle wakeups against %d plain"
                                % (name, wakeups, plain_wakeups))
            expect(compositor, name, reference, shots, settled)
        finally:
            compositor.stop()
            for path in work.glob("frame-*.ppm"):
                path.unlink()

    def moved(compositor, name, reference, shots, settled):
        pass

    def magnified(compositor, name, reference, shots, settled):
        park(compositor)
        before = compositor.screenshot("shake-before.ppm")
        mover = compositor.stroke_until(ZIGZAG, "dragged", steps=3, interval_ms=6)
        magnified_shot = compositor.screenshot("shake-magnified.ppm")
        mover.wait()
        rest_lit = len(near(lit_points(before), PARK[0], PARK[1], 120))
        shaken_lit = len(near(lit_points(magnified_shot), PARK[0], PARK[1], 120))
        print("%s: %d lit pixels at rest, %d magnified" % (name, rest_lit, shaken_lit))
        if shaken_lit < rest_lit * 3:
            failures.append("%s: a shake did not magnify the cursor (%d against %d)"
                            % (name, shaken_lit, rest_lit))
        time.sleep(1.0)
        if not same(compositor.screenshot("shake-after.ppm"), reference):
            failures.append(name + ": the magnification did not time out to the plain image")

    def settled_late(compositor, name, reference, shots, settled):
        park(compositor)
        before = compositor.screenshot("spring-before.ppm")
        mover = compositor.stroke_until([(PARK[0], PARK[1]), (900, PARK[1])], "released",
                                        steps=8, interval_ms=10)
        early = compositor.screenshot("spring-early.ppm")
        time.sleep(0.12)
        middle = compositor.screenshot("spring-middle.ppm")
        time.sleep(1.2)
        after = compositor.screenshot("spring-after.ppm")
        mover.wait()
        if same(early, reference):
            failures.append(name + ": the cursor was already at rest when the motion stopped")
        if same(early, middle):
            failures.append(name + ": the cursor froze instead of settling")
        if not same(after, reference):
            failures.append(name + ": the cursor never settled to the plain image")
        print("%s: settled through %d frames" % (name, len(shots)))

    def trailed(compositor, name, reference, shots, settled):
        far = [len(lit_points(shot)) for shot in shots]
        print("%s: lit pixels per captured frame: %s" % (name, far))
        if not far or max(far) < 200:
            failures.append(name + ": the trail never drew")

    check("tilt", TILT + """
(define-extension "test-cursor" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "tilt" +tilt+)) nil))
""", moved)

    check("stretch", STRETCH + """
(define-extension "test-cursor" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "stretch" +stretch+)) nil))
""", moved)

    check("spring", SPRING, settled_late)

    check("trail", TRAIL, trailed)

    check("shake", SHAKE, magnified, path=ZIGZAG, steps=3, interval_ms=6)

    unload = root / "unload"
    unload.mkdir(exist_ok=True)
    compositor = build("unload", THROWAWAY + """
(define-extension "test-cursor" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "jelly" +jelly+)) nil))
""")
    try:
        time.sleep(1.0)
        rest, shots, settled = case(compositor, [(PARK[0], PARK[1]), (900, PARK[1])])
        if not same(rest, reference):
            failures.append("unload: the resting cursor is not the plain image")
        if not any(not same(shot, reference) for shot in shots):
            failures.append("unload: the user's own effect never deformed the cursor")
        if not same(settled, reference):
            failures.append("unload: the user's effect did not settle to the plain image")
        compositor.write_policy("")
        compositor.reload()
        time.sleep(0.6)
        park(compositor)
        compositor.stroke([(PARK[0], PARK[1]), (900, PARK[1])], steps=8, interval_ms=10)
        time.sleep(0.9)
        park(compositor)
        if not same(compositor.screenshot("unmounted.ppm"), reference):
            failures.append("unload: removing the effect did not restore the plain image")
        drawn, wakeups = idle(compositor)
        print("unload: %d idle frames, %d idle wakeups after the effect was removed"
              % (drawn, wakeups))
        if drawn:
            failures.append("unload: drew %d frames while idle after unmounting" % drawn)
    finally:
        compositor.stop()

    if failures:
        print("cursor: FAILED\n  " + "\n  ".join(failures), file=sys.stderr)
        print("cursor: work directory " + str(root), file=sys.stderr)
        return 1
    print("cursor: every mode deformed the cursor and returned to the plain image")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as failure:
        print("cursor: " + str(failure), file=sys.stderr)
        sys.exit(1)
