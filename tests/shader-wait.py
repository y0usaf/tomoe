#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
import time

LINES = 4000
WAIT = 180
SLOWEST = 0.25
ANSWERS = 20
QUICK = 0.2
FACTOR = 25

POLICY = """(in-package #:tomoe-user)

(define-extension "shader-wait" (:reads (:outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (append
             (when output
               (list (configure-output (getf output :name) :mode (list 1280 720)
                                       :scale 1 :position '(0 0))))
             (list (shell-surface :shader-wait (ui :stack)
                                  :anchors '(:top :left)
                                  :layer :top
                                  :click-through t
                                  :width {size} :height {size}
                                  :margin '({top} 0 0 {left})
                                  :background (list :shader "{shader}" :fps 0))))
            nil)))
"""

SIZE = 64
SURFACE = "SHADER-WAIT"
FIRST_AT = (480, 360)
SECOND_AT = (480, 520)


class Failure(Exception):
    pass


def heavy_shader(seed):
    steps = "".join("    s = s * 1.0001 + at.y * %.6f;\n" % (seed + index * 0.000001)
                    for index in range(LINES))
    return ("void mainImage(out vec4 color, in vec2 at) {\n"
            "    float s = 0.0;\n" + steps +
            "    color = vec4(fract(s), fract(s * 1.7), 0.35, 1.0);\n}\n")


def write_shader(path, seed):
    with open(path, "w") as handle:
        handle.write(heavy_shader(seed))
    return path


def write_policy(work, shader, at):
    path = os.path.join(work, "policy.lisp")
    with open(path, "w") as handle:
        handle.write(POLICY.format(size=SIZE, left=at[0], top=at[1], shader=shader))
    return path


class Session:
    def __init__(self, work, tomoe):
        self.tomoe = tomoe
        self.work = work
        self.socket = "shader-wait"
        self.runtime = os.path.join(work, "run")
        self.home = os.path.join(work, "home")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        os.makedirs(os.path.join(self.home, ".cache"), exist_ok=True)
        self.cache = os.path.join(self.home, ".cache", "tomoe", "shaders")
        self.log_path = os.path.join(work, "tomoe.log")
        self.environment = dict(os.environ)
        self.environment.update({
            "HOME": self.home,
            "XDG_RUNTIME_DIR": self.runtime,
            "XDG_CACHE_HOME": os.path.join(self.home, ".cache"),
            "WAYLAND_DISPLAY": self.socket,
            "TOMOE_DEBUG": "1",
            "MESA_LOADER_DRIVER_OVERRIDE": "zink",
            "LIBGL_ALWAYS_SOFTWARE": "1",
        })
        self.log = open(self.log_path, "w")

    def start(self, policy):
        self.process = subprocess.Popen(
            [self.tomoe, "--socket", self.socket, "--backend", "headless", "--bare",
             "--no-watch", "--config", policy],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise Failure("tomoe exited while starting:\n" + self.text())
            if self.call("inspect")[0] == 0:
                return
        raise Failure("tomoe never answered inspect:\n" + self.text())

    def call(self, *arguments, timeout=WAIT):
        result = subprocess.run([self.tomoe, "--socket", self.socket, *arguments],
                                env=self.environment, capture_output=True, text=True,
                                timeout=timeout)
        return result.returncode, result.stdout + result.stderr

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def screenshot(self, path):
        try:
            result = subprocess.run(["grim", "-t", "ppm", path], env=self.environment,
                                    capture_output=True, text=True, timeout=15)
        except subprocess.TimeoutExpired:
            raise Failure("the session did not answer a screen capture: it was not "
                          "drawing")
        if result.returncode != 0:
            raise Failure("cannot capture the screen: " + result.stderr)


def read_ppm(path):
    data = open(path, "rb").read()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    return int(fields[1]), int(fields[2]), data[at + 1:]


def surface_ink(path, box):
    width, height, pixels = read_ppm(path)
    x, y, w, h = box
    column, row = max(0, x - 8), max(0, y - 8)
    background = pixels[(row * width + column) * 3:(row * width + column) * 3 + 3]
    lit = 0
    for row in range(y, min(y + h, height)):
        for column in range(x, min(x + w, width)):
            offset = (row * width + column) * 3
            if pixels[offset:offset + 3] != background:
                lit += 1
    return lit


def settle(session):
    deadline = time.monotonic() + 30
    times, quick = [], 0
    while time.monotonic() < deadline:
        at = time.monotonic()
        status, _ = session.call("inspect")
        took = time.monotonic() - at
        quick = quick + 1 if status == 0 and took < QUICK else 0
        times.append(took)
        if quick >= 3:
            times.sort()
            return times[len(times) // 2]
        time.sleep(0.05)
    raise Failure("the session never answered %d times inside %.0f ms:\n%s"
                  % (3, QUICK * 1000, session.text()))


def probe(session, label, settled):
    limit = max(SLOWEST, settled * FACTOR)
    before = set(os.listdir(session.cache))
    answered, slowest, when = [], 0.0, 0.0
    start = time.monotonic()
    while True:
        at = time.monotonic()
        status, reply = session.call("inspect")
        took = time.monotonic() - at
        if status != 0:
            raise Failure(f"{label}: inspect failed while a shader compiled: {reply[:400]}")
        answered.append(took)
        if took > slowest:
            slowest, when = took, time.monotonic() - start
        if set(os.listdir(session.cache)) - before:
            break
        if time.monotonic() - start > WAIT:
            raise Failure(f"{label}: the copy of Tomoe never wrote a program within "
                          f"{WAIT} s:\n{session.text()}")
        time.sleep(0.01)
    took = time.monotonic() - start
    answered.sort()
    median = answered[len(answered) // 2]
    print(f"{label}: {len(answered)} answers while the copy compiled for {took:.2f} s, "
          f"the median {median * 1000:.0f} ms and the slowest {slowest * 1000:.0f} ms at "
          f"{when:.2f} s, settled {settled * 1000:.0f} ms, limit {limit * 1000:.0f} ms")
    if len(answered) < ANSWERS:
        raise Failure(f"{label}: only {len(answered)} answers in {took:.2f} s, so the "
                      f"session was not running while the driver compiled")
    if median > limit:
        raise Failure(f"{label}: the session took {median * 1000:.0f} ms at the median to "
                      f"answer while a shader compiled, past {limit * 1000:.0f} ms: it "
                      f"waited for the copy")
    return took


def surface_box(session):
    reply = session.call("inspect")[1]
    match = re.search(r':NAME :' + SURFACE + r' :OUTPUT "[^"]*" :X (-?\d+) :Y (-?\d+) '
                      r':WIDTH (\d+) :HEIGHT (\d+)', reply)
    return tuple(int(value) for value in match.groups()) if match else None


def fallback(session, label):
    path = os.path.join(session.work, label + "-fallback.ppm")
    for _ in range(200):
        box = surface_box(session)
        if box:
            session.screenshot(path)
            lit = surface_ink(path, box)
            print(f"{label}: {lit} of {box[2] * box[3]} pixels of its surface at {box} "
                  f"drew before the program landed")
            return lit
        time.sleep(0.05)
    raise Failure(f"{label}: the session never staged the surface:\n" + session.text())


def wait_drawn(session, label, shader):
    path = os.path.join(session.work, label + ".ppm")
    deadline = time.monotonic() + WAIT
    while time.monotonic() < deadline:
        box = surface_box(session)
        if not box:
            time.sleep(0.05)
            continue
        session.screenshot(path)
        lit = surface_ink(path, box)
        if lit >= box[2] * box[3] * 9 // 10:
            print(f"{label}: the {os.path.getsize(shader) // 1024} KiB shader drew "
                  f"{lit} of {box[2] * box[3]} pixels of its surface at {box}")
            return
        time.sleep(0.05)
    raise Failure(f"{label}: the shader's surface never drew once its program landed\n"
                  + session.text())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--work", required=True)
    arguments = parser.parse_args()
    work = os.path.abspath(arguments.work)
    os.makedirs(work, exist_ok=True)
    first = write_shader(os.path.join(work, "first.glsl"), 0.25)
    second = write_shader(os.path.join(work, "second.glsl"), 0.75)
    session = Session(work, os.path.abspath(arguments.tomoe))
    try:
        session.start(write_policy(work, first, FIRST_AT))
        settled = settle(session)
        fallback(session, "first")
        started = time.monotonic()
        probe(session, "first", settled)
        wait_drawn(session, "first", first)
        print(f"first: drawn {time.monotonic() - started:.2f} s after it was asked for")
        write_policy(work, second, SECOND_AT)
        status, reply = session.call("reload")
        if status != 0:
            raise Failure("the config with a second shader did not load: " + reply[:400])
        fallback(session, "second")
        started = time.monotonic()
        probe(session, "second", settled)
        wait_drawn(session, "second", second)
        print(f"second: drawn {time.monotonic() - started:.2f} s after it was asked for")
        fatal = re.search(r"shader copy (could not|did not finish)", session.text())
        if fatal:
            raise Failure("the copy of Tomoe did not compile a shader: " + fatal.group(0))
        print("shader-wait: a shader the driver had not built kept every answer inside "
              "the session's own range, and each surface drew once its program landed")
    except Failure as failure:
        print("shader-wait: " + str(failure))
        return 1
    finally:
        if session.process.poll() is None:
            session.call("quit", timeout=10)
            session.process.wait(timeout=60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
