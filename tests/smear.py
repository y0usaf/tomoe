#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SHADER_EXTENSION = """(define-extension "smear-shader" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore state event))
  (values nil
          (loop for window in (context snapshot :windows)
                collect (apply #'window-properties (getf window :id) +window-shader+))
          nil))
"""

POLICY = """(in-package #:tomoe-user)

(defparameter +window-shader+
  (list :shader "{shader}" :shader-margin {margin} :shader-settle {settle}))

{shader_extension}
(define-extension "smear-drag" (:reads (:windows :button :grab) :state nil) (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (window (first windows))
         (id (and window (getf window :id)))
         (type (getf event :type)))
    (if (null id)
        (values state nil nil)
        (progn
          (setf state (or state (list :x {x} :y {y} :dragging nil)))
          (cond ((and (eq type :button) (eq (getf event :state) :pressed)
                      (eql (getf event :id) id))
                 (setf (getf state :dragging) t
                       (getf state :origin-x) (getf event :x)
                       (getf state :origin-y) (getf event :y)
                       (getf state :base-x) (getf state :x)
                       (getf state :base-y) (getf state :y)))
                ((and (eq type :grab) (getf state :dragging))
                 (setf (getf state :x) (+ (getf state :base-x)
                                          (- (getf event :x) (getf state :origin-x)))
                       (getf state :y) (+ (getf state :base-y)
                                          (- (getf event :y) (getf state :origin-y)))))
                ((and (eq type :button) (eq (getf event :state) :released))
                 (setf (getf state :dragging) nil)))
          (values state
                  (append (list (place id (getf state :x) (getf state :y)
                                       (getf window :width) (getf window :height) t))
                          (when (getf state :dragging) (list (grab id :move))))
                  nil)))))
"""

class Failure(Exception):
    pass

def parse_frames(text):
    entries = {}
    for match in re.finditer(r':NAME "([^"]+)"([^\n]*)', text, re.IGNORECASE):
        fields = { key.lower(): value for key, value in
            re.findall(r':([A-Za-z-]+)\s+([-\w]+)', match.group(2)) }
        entries[match.group(1)] = fields
    return entries

class Compositor:
    def __init__(self, work, tomoe, shader, margin, settle, start, plain=False):
        self.tomoe = tomoe
        self.socket = "smear"
        self.runtime = os.path.join(work, "run")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.log_path = os.path.join(work, "tomoe.log")
        self.log = open(self.log_path, "w")
        policy = os.path.join(work, "policy.lisp")
        with open(policy, "w") as handle:
            handle.write(POLICY.format(shader=shader, margin=margin, settle=settle,
                x=start[0], y=start[1], shader_extension="" if plain else SHADER_EXTENSION))
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

    def windows(self):
        reply = self.call("inspect")[1]
        match = re.search(r":WINDOWS \(\(:ID (\d+)", reply)
        return int(match.group(1)) if match else 0

    def frames(self):
        reply = self.call("frames")[1]
        parsed = parse_frames(reply)
        if not parsed:
            print("smear: unreadable frames reply: " + reply[:200], file=sys.stderr)
        return parsed

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

def measure_idle(compositor, seconds):
    before = compositor.frames()
    wakes = compositor.wakeups()
    time.sleep(seconds)
    after = compositor.frames()
    wakes = compositor.wakeups() - wakes
    drawn = 0
    for name, fields in after.items():
        drawn += int(fields.get("frame", 0)) - int(before.get(name, {}).get("frame", 0))
    return drawn, wakes

def capture_drag(compositor, pointer, work, path, interval):
    shots = []
    stop = threading.Event()

    def capture():
        index = 0
        while not stop.is_set():
            frame = os.path.join(work, f"frame-{index:03d}.ppm")
            try:
                compositor.screenshot(frame)
            except Failure:
                break
            shots.append(frame)
            index += 1
            time.sleep(interval)

    thread = threading.Thread(target=capture, daemon=True)
    thread.start()
    time.sleep(0.2)
    before = compositor.frames()
    result = subprocess.run([os.path.abspath(pointer), "--path", path, "--steps", "5", "--interval-ms", "16",
            "--hold-ms", "600", "--delay-ms", "100", "--extent", "1280x720"],
        env=compositor.environment, capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure("pointer-path failed: " + result.stderr.strip())
    time.sleep(0.9)
    after = compositor.frames()
    stop.set()
    thread.join()
    return shots, before, after

def frame_cost(before, after):
    rows = []
    for name, fields in after.items():
        old = before.get(name)
        if not old:
            continue
        frames = int(fields.get("frame", 0)) - int(old.get("frame", 0))
        drawn = int(fields.get("drawn-pixels", 0)) - int(old.get("drawn-pixels", 0))
        cost = int(fields.get("frame-ns", 0)) - int(old.get("frame-ns", 0))
        if frames:
            rows.append(f"{name}: {frames} frames, {drawn / max(frames, 1) / 1e6:.2f} Mpx drawn, "
                f"{cost / frames / 1e6:.2f} ms per frame")
    return rows

def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    parts = data.split(b"\n", 3)
    width, height = (int(value) for value in parts[1].split())
    return width, height, parts[3]

def lit_box(path, step=4):
    width, height, pixels = read_ppm(path)
    left, top, right, bottom = width, height, -1, -1
    for y in range(0, height, step):
        row = y * width * 3
        for x in range(0, width, step):
            at = row + x * 3
            if pixels[at] > 25 or pixels[at + 1] > 25 or pixels[at + 2] > 25:
                left = min(left, x)
                right = max(right, x)
                top = min(top, y)
                bottom = max(bottom, y)
    return left, top, right, bottom

def moving_frames(shots, count):
    first = lit_box(shots[0])
    start = 0
    for index, shot in enumerate(shots):
        if lit_box(shot) != first:
            start = index
            break
    return shots[max(0, start - 6):max(0, start - 6) + count]

def strip(shots, output, columns, scale, crop):
    if not shots:
        raise Failure("captured no frames")
    listing = os.path.join(os.path.dirname(shots[0]), "frames.txt")
    with open(listing, "w") as handle:
        handle.write("ffconcat version 1.0\n")
        for shot in shots:
            handle.write(f"file '{os.path.basename(shot)}'\nduration {1 / 30}\n")
    box = crop or lit_box(shots[0])
    left, top, right, bottom = box
    region = f"crop={right - left}:{bottom - top}:{left}:{top}"
    rows = max(1, (len(shots) + columns - 1) // columns)
    result = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0",
            "-i", listing, "-vf", f"{region},scale={scale},tile={columns}x{rows}",
            "-frames:v", "1", output], capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure("ffmpeg failed: " + result.stderr.strip())

def main():
    parser = argparse.ArgumentParser(description="Scripted drag smear capture and cost check.")
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--pointer", required=True)
    parser.add_argument("--image", required=True, help="P6 PPM for the window contents")
    parser.add_argument("--output", required=True, help="frame strip PNG")
    parser.add_argument("--shader", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "examples", "shaders", "drag-smear.glsl"))
    parser.add_argument("--plain", action="store_true",
        help="no window shader; measures the compositor without the effect")
    parser.add_argument("--margin", type=int, default=512)
    parser.add_argument("--settle", type=float, default=0.45)
    parser.add_argument("--path", default="450,410;620,300;800,300;950,450;1000,600")
    parser.add_argument("--idle-seconds", type=float, default=3)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--columns", type=int, default=6)
    parser.add_argument("--scale", default="426:240")
    parser.add_argument("--work", default=None)
    arguments = parser.parse_args()
    if not shutil.which("grim") or not shutil.which("ffmpeg"):
        raise Failure("grim and ffmpeg must be on PATH")

    work = arguments.work or tempfile.mkdtemp(prefix="tomoe-smear-")
    os.makedirs(work, exist_ok=True)
    compositor = None
    client = None
    try:
        compositor = Compositor(work, arguments.tomoe, os.path.abspath(arguments.shader),
            arguments.margin, arguments.settle, (240, 260), arguments.plain)
        client = subprocess.Popen([os.path.abspath(arguments.client), "--mode", "xdg",
                "--size", "420x300",
                "--image", os.path.abspath(arguments.image), "--seconds", "600"],
            env=compositor.environment, cwd=work,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 20
        while time.time() < deadline and not compositor.windows():
            time.sleep(0.1)
        if not compositor.windows():
            raise Failure("the client window never appeared:\n" + compositor.text())
        time.sleep(1.5)

        idle_frames, idle_wakes = measure_idle(compositor, arguments.idle_seconds)
        shots, before, after = capture_drag(compositor, arguments.pointer, work,
            arguments.path, 0.02)
        costs = frame_cost(before, after)
        settle_frames, settle_wakes = measure_idle(compositor, arguments.idle_seconds)
        chosen = moving_frames(shots, arguments.frames)
        boxes = [lit_box(shot) for shot in chosen]
        box = (min(b[0] for b in boxes), min(b[1] for b in boxes),
            max(b[2] for b in boxes), max(b[3] for b in boxes))
        strip(chosen, arguments.output, arguments.columns, arguments.scale, box)
        print(f"idle: {idle_frames} frames, {idle_wakes} wakeups in {arguments.idle_seconds:.1f} s")
        for row in costs:
            print("drag: " + row)
        print(f"settled: {settle_frames} frames, {settle_wakes} wakeups in "
            f"{arguments.idle_seconds:.1f} s")
        print(f"captured {len(shots)} frames into {arguments.output}")
    finally:
        if client and client.poll() is None:
            client.terminate()
        if compositor:
            compositor.stop()

if __name__ == "__main__":
    try:
        main()
    except Failure as failure:
        print(f"smear: {failure}", file=sys.stderr)
        print(f"smear: work directory kept for inspection", file=sys.stderr)
        sys.exit(1)
