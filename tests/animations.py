#!/usr/bin/env python3

import argparse
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REFRESH_HZ = 240

POLICY = """
(in-package #:tomoe-user)

(define-extension "anim-probe"
    (:reads (:windows :outputs :command)
     :state (list :moved nil :viewed nil :bar nil :hidden nil))
    (snapshot state event)
  (let* ((command (and (eq (getf event :type) :command) (getf event :name)))
         (output (first (context snapshot :outputs)))
         (windows (context snapshot :windows)))
    (cond ((equal command "move") (setf (getf state :moved) (not (getf state :moved))))
          ((equal command "view") (setf (getf state :viewed) (not (getf state :viewed))))
          ((equal command "bar") (setf (getf state :bar) t))
          ((equal command "hide") (setf (getf state :hidden) t))
          ((equal command "show") (setf (getf state :hidden) nil)))
    (let ((offset (if (getf state :moved) 600 0))
          (left (if (getf state :bar) 600 40))
          (view (if (getf state :viewed) 120 0)))
      (values state
              (append
               (list (configure-output (getf output :name) :mode '(1280 720 240)
                                       :scale 1 :position '(0 0))
                     (settings
                      :animations
                      '(:window-move (:spring (:stiffness 16000 :epsilon 1/1000))
                        :window-open (:ease (:duration-ms 90 :curve :ease-out-expo))
                        :window-view (:spring (:stiffness 16000 :epsilon 1/1000))))
                     (shell-surface :bar
                                    (ui :rect :width 200 :height 60 :background "#ff0000ff")
                                    :anchors '(:top :left)
                                    :margin (list 560 0 0 left)
                                    :slide t :layer :overlay)
                     (set-view view 0)
                     (define-command "move")
                     (define-command "view")
                     (define-command "bar")
                     (define-command "hide")
                     (define-command "show"))
               (when output
                 (loop for window in windows
                       for index from 0
                       append (if (getf state :hidden)
                                  (list (hide-window (getf window :id)))
                                  (list (show-window (getf window :id))
                                        (place (getf window :id)
                                               (+ (getf output :x) offset (* index 220))
                                               (+ (getf output :y) 200)
                                               180 180)))))
              nil)))))
"""


class Failure(Exception):
    pass


REPORT = []


def say(line):
    REPORT.append(line)
    print(line, flush=True)


def tokenize(text):
    stack = [[]]
    index = 0
    while index < len(text):
        character = text[index]
        if character in " \n\t":
            index += 1
        elif character == "(":
            stack.append([])
            index += 1
        elif character == ")":
            done = stack.pop()
            stack[-1].append(done)
            index += 1
        elif character == '"':
            end = index + 1
            while end < len(text) and text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            stack[-1].append(text[index:end + 1])
            index = end + 1
        else:
            end = index
            while end < len(text) and text[end] not in ' \n\t()"':
                end += 1
            stack[-1].append(text[index:end])
            index = end
    return stack[0][0]


def plist(value):
    return dict(zip(value[::2], value[1::2])) if isinstance(value, list) else {}


def unwrap(output):
    value = tokenize(output)
    if isinstance(value, list) and len(value) >= 3 and value[1] == ":OK":
        value = value[2]
    while isinstance(value, list) and len(value) == 1:
        value = value[0]
    return value


def lower_plist(value):
    return {key.lstrip(":").lower() if isinstance(key, str) else key: item
            for key, item in plist(value).items()}


class Compositor:

    def __init__(self, work, tomoe, socket):
        self.tomoe = tomoe
        self.socket = socket
        self.runtime = os.path.join(work, "run")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.log_path = os.path.join(work, "tomoe.log")
        self.log = open(self.log_path, "w")
        self.environment = dict(os.environ)
        self.environment.update({"XDG_RUNTIME_DIR": self.runtime,
            "WAYLAND_DISPLAY": socket, "TOMOE_DEBUG": "1",
            "MESA_LOADER_DRIVER_OVERRIDE": "zink", "LIBGL_ALWAYS_SOFTWARE": "1"})
        self.process = subprocess.Popen(
            [tomoe, "--socket", socket, "--backend", "headless", "--bare", "--no-watch"],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 120
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

    def mount(self, path):
        status, output = self.call("mount", path)
        if status != 0:
            raise Failure("mount failed: " + output + self.text())

    def command(self, name):
        status, output = self.call("command", name)
        if status != 0:
            raise Failure(f"command {name} failed: {output}" + self.text())

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def animation(self):
        status, output = self.call("animation")
        if status != 0:
            return None
        return lower_plist(unwrap(output))

    def drawn(self):
        status, output = self.call("frames")
        if status != 0:
            return 0
        value = unwrap(output)
        entries = value if isinstance(value, list) and value and \
            isinstance(value[0], list) else [value]
        total = 0
        for entry in entries:
            fields = lower_plist(entry) if isinstance(entry, list) else {}
            try:
                total += int(fields.get("frame", 0))
            except (TypeError, ValueError):
                pass
        if not total:
            raise Failure("cannot read the output frame counters: " + output.strip())
        return total

    def frame(self, name=None, save=None):
        last = ""
        for _ in range(6):
            result = subprocess.run(["grim", "-t", "ppm", "-"], env=self.environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode == 0:
                width, height, pixels = parse_ppm(result.stdout)
                if save and name:
                    with open(os.path.join(save, name + ".ppm"), "wb") as handle:
                        handle.write(result.stdout)
                return width, height, pixels
            last = result.stderr.decode().strip()
            time.sleep(0.05)
        raise Failure("grim could not copy the output: " + last)

    def digest(self, name, save):
        width, height, pixels = self.frame(name, save)
        return hashlib.sha256(b"%dx%d:" % (width, height) + pixels).hexdigest()[:16]

    def switches(self):
        with open("/proc/%d/status" % self.process.pid) as handle:
            for line in handle:
                if line.startswith("voluntary_ctxt_switches") or \
                        line.startswith("nonvoluntary_ctxt_switches"):
                    yield int(line.split()[1])

    def wakeups(self, seconds=1.0):
        before = sum(self.switches())
        time.sleep(seconds)
        return sum(self.switches()) - before

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


class Client:

    def __init__(self, work, binary, name, colour, socket):
        self.log_path = os.path.join(work, name + ".log")
        self.log = open(self.log_path, "w")
        self.environment = dict(os.environ)
        self.environment.update({"XDG_RUNTIME_DIR": os.path.join(work, "run"),
            "WAYLAND_DISPLAY": socket})
        self.process = subprocess.Popen(
            [binary, "--mode", "xdg", "--app-id", name, "--title", name,
             "--size", "180x180", "--color", colour, "--seconds", "3600"],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.text().find("mapped") >= 0:
                return
            if self.process.poll() is not None:
                raise Failure(name + " exited while mapping:\n" + self.text())
            time.sleep(0.05)
        raise Failure(name + " never mapped:\n" + self.text())

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


def parse_ppm(data):
    assert data[:2] == b"P6", data[:16]
    fields, offset = [], 2
    while len(fields) < 3:
        while data[offset:offset + 1].isspace():
            offset += 1
        start = offset
        while not data[offset:offset + 1].isspace():
            offset += 1
        fields.append(int(data[start:offset]))
    width, height, _ = fields
    return width, height, data[offset + 1:offset + 1 + width * height * 3]


class Measurement:

    def __init__(self, label, before, after, drawn, digest, expected_ms, settle_ms):
        self.label = label
        self.expected_ms = expected_ms
        self.settle_ms = settle_ms
        self.digest = digest
        self.drawn = drawn
        counters = after or {}
        self.period = int(counters.get("period-ns", 0) or 0)
        self.refresh = int(counters.get("refresh-mhz", 0) or 0)
        self.frames = int(counters.get("frames", 0)) - int((before or {}).get("frames", 0))
        self.gaps = int(counters.get("gaps", 0)) - int((before or {}).get("gaps", 0))
        self.dead = int(counters.get("dead", 0)) - int((before or {}).get("dead", 0))
        self.span = int(counters.get("span-ns", 0) or 0)
        self.interval = int(counters.get("max-interval-ns", 0) or 0)
        self.steady = int(counters.get("steady-interval-ns", 0) or 0)
        self.first = int(counters.get("first-passed-ns", 0) or 0)
        self.lead_min = int(counters.get("lead-min-ns", 0) or 0)
        self.lead_max = int(counters.get("lead-max-ns", 0) or 0)
        self.counted = after is not None and before is not None

    @property
    def refreshes(self):
        return self.span / self.period if self.period else 0.0

    def line(self):
        if not self.counted:
            return f"{self.label}: counters unavailable, screen {self.digest}"
        return (f"{self.label}: {self.frames} animation frames over {self.refreshes:.1f}"
                f" refreshes ({self.span / 1e6:.1f} ms), {self.drawn} composited frames,"
                f" worst interval {self.interval / 1e6:.2f} ms (steady"
                f" {self.steady / 1e6:.2f} ms), period {self.period / 1e6:.2f}"
                f" ms, first frame after {self.first / 1e6:.2f} ms, sampled"
                f" {self.lead_min / 1e6:.2f}..{self.lead_max / 1e6:.2f} ms ahead,"
                f" {self.dead} dead, {self.gaps} skipped, screen {self.digest}")

    def check(self, problems):
        if not self.counted:
            return
        if self.refresh != REFRESH_HZ * 1000:
            problems.append(f"{self.label}: the output runs at {self.refresh / 1000:.0f} Hz,"
                            f" not {REFRESH_HZ}")
        if self.dead:
            problems.append(f"{self.label}: {self.dead} frame(s) showed the animation's start"
                            " position, so the first frame after the input did not move")
        if self.first <= 0:
            problems.append(f"{self.label}: no frame advanced the animation")
        if self.lead_min <= self.period // 2:
            problems.append(f"{self.label}: animation state was sampled only"
                            f" {self.lead_min / 1e6:.2f} ms ahead of the frame start, not at the"
                            " present time")
        if self.span < self.expected_ms * 800000:
            problems.append(f"{self.label}: the animation ran for {self.span / 1e6:.1f} ms of its"
                            f" {self.settle_ms} ms settle time")
        if self.frames < self.refreshes:
            problems.append(f"{self.label}: {self.frames} animation frames for"
                            f" {self.refreshes:.1f} refreshes: some refresh had no frame")
        if self.steady > self.period * 3 // 2:
            problems.append(f"{self.label}: {self.steady / 1e6:.2f} ms passed between two"
                            f" animation frames once the input had landed, more than one"
                            f" refresh ({self.period / 1e6:.2f} ms), so refreshes were skipped")
        if self.gaps:
            problems.append(f"{self.label}: {self.gaps} refresh(es) passed with no animation frame")
        if self.drawn and self.drawn < self.refreshes * 0.9:
            problems.append(f"{self.label}: the output composited {self.drawn} frames for"
                            f" {self.refreshes:.1f} refreshes")


def measure(compositor, label, action, expected_ms, settle_ms, frames_dir, scenario):
    before = compositor.animation()
    try:
        drawn = compositor.drawn()
    except Failure:
        drawn = 0
    action()
    time.sleep(max(0.8, settle_ms / 1000.0 * 3.5))
    after = compositor.animation()
    try:
        drawn = compositor.drawn() - drawn
    except Failure:
        drawn = 0
    measurement = Measurement(label, before, after, drawn,
                              compositor.digest(scenario, frames_dir), expected_ms, settle_ms)
    say(measurement.line())
    return measurement


def main():
    parser = argparse.ArgumentParser(description="Judge that Tomoe animations follow the"
        " display: a frame per refresh, sampled at the present, no dead first frame,"
        " nothing at rest.")
    parser.add_argument("--tomoe", required=True, help="the tomoe executable")
    parser.add_argument("--client", required=True, help="a tomoe-test-client binary")
    parser.add_argument("--frames", default=None, help="keep settled screens here")
    parser.add_argument("--allow-missing-counters", action="store_true",
        help="only hash the settled screens, for comparing another build")
    arguments = parser.parse_args()
    frames_dir = arguments.frames or tempfile.mkdtemp(prefix="animations-frames-")
    os.makedirs(frames_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="animations-")
    policy = os.path.join(work, "probe.lisp")
    with open(policy, "w") as handle:
        handle.write(POLICY)
    socket = "animations-%d" % os.getpid()
    compositor = None
    client = None
    problems = []
    try:
        compositor = Compositor(work, arguments.tomoe, socket)
        compositor.mount(policy)
        time.sleep(0.6)
        counters = compositor.animation()
        if counters is not None:
            say("configured: move settles in %s ms, open lasts %s ms, view settles in %s ms"
                % (counters["move-settle-ms"], counters["open-ms"],
                   counters["view-settle-ms"]))
            if int(counters["move-settle-ms"]) > 120:
                problems.append("the move spring settles in %s ms"
                                % counters["move-settle-ms"])
            if int(counters["open-ms"]) > 120:
                problems.append("the open ease lasts %s ms" % counters["open-ms"])
        measured = []

        measured.append(measure(compositor, "bar slide",
                                lambda: compositor.command("bar"), 73, 73,
                                frames_dir, "bar"))

        client = Client(work, arguments.client, "anim-window", "ffcc00", socket)
        time.sleep(0.6)
        compositor.command("hide")
        time.sleep(0.4)
        measured.append(measure(compositor, "window open",
                                lambda: compositor.command("show"), 90, 90,
                                frames_dir, "open"))

        def restore():
            compositor.command("show")

        restore()
        time.sleep(0.4)

        measured.append(measure(compositor, "window move",
                                lambda: compositor.command("move"), 73, 73,
                                frames_dir, "move"))
        if measured[-1].digest == measured[-2].digest:
            problems.append("the window never moved")

        resting = compositor.frame()
        compositor.command("move")
        early = compositor.frame()
        if early[2] == resting[2]:
            problems.append("the first frame after the window move input shows no motion")
        time.sleep(0.5)

        measured.append(measure(compositor, "workspace scroll",
                                lambda: compositor.command("view"), 73, 73,
                                frames_dir, "view"))
        if measured[-1].digest == measured[-2].digest:
            problems.append("the view never moved")

        if all(not measurement.counted for measurement in measured):
            if not arguments.allow_missing_counters:
                problems.append("the compositor does not report animation counters")
        for measurement in measured:
            measurement.check(problems)

        time.sleep(0.4)
        wakes = compositor.wakeups(1.0)
        say(f"rest: {wakes} wakeups/s")
        if wakes > 2:
            problems.append(f"the compositor woke {wakes} times in a second at rest")
        print(f"frames: {frames_dir}", flush=True)
    except (Failure, AssertionError, KeyError) as failure:
        problems.append(str(failure))
    finally:
        if client:
            client.stop()
        if compositor:
            compositor.call("quit")
            compositor.stop()
        shutil.rmtree(work, ignore_errors=True)
    for problem in problems:
        say("animation speed: FAILED: " + problem)
    if os.environ.get("ANIMATIONS_REPORT"):
        with open(os.environ["ANIMATIONS_REPORT"], "w") as handle:
            handle.write("\n".join(REPORT) + "\n")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
