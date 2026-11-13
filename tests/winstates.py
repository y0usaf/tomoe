#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

POLICY = """
(in-package #:tomoe-user)

(define-extension "winstates-wm" (:reads (:windows :outputs :focus :key)
                                  :state (list :focused nil :chosen nil))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (focused (getf state :focused))
         (chosen (getf state :chosen)))
    (when (and (eq (getf event :type) :key) (equal (getf event :owner) "winstates-wm")
               (equal (getf event :command) "next"))
      (setf focused (or (second (member focused ids)) (first ids)) chosen t))
    (when (and (not chosen) ids) (setf focused (car (last ids))))
    (values (list :focused focused :chosen chosen)
            (append (when output
                      (loop for window in windows
                            for index from 0
                            collect (place (getf window :id)
                                           (+ (getf output :x) 10 (* index 420))
                                           (+ (getf output :y) 10) 400 400)))
                    (list (focus focused) (bind-key '(:super) "Tab" :next)))
            nil)))
"""

ROTATE = """
(in-package #:tomoe-user)

(define-extension "winstates-rotate" (:reads (:windows :focus) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((first (first (context snapshot :windows))))
    (values nil
            (when first
              (list (window-properties (getf first :id)
                                       :rotation 18 :translate '(:x 60 :y 30))))
            nil)))
"""

EXAMPLES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "examples")
BACKGROUND = (12, 12, 12)


class Failure(Exception):
    pass


class Compositor:
    def __init__(self, work, tomoe, socket=None):
        socket = socket or "winstates-%d" % os.getpid()
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
            raise Failure(f"mount {path} failed: {output}\n" + self.text())

    def unmount(self, name):
        status, output = self.call("unmount", name)
        if status != 0:
            raise Failure(f"unmount {name} failed: {output}\n" + self.text())

    def command(self, owner, name):
        status, output = self.call("command", owner, name)
        if status != 0:
            raise Failure(f"command {owner} {name} failed: {output}\n" + self.text())

    def windows(self):
        status, output = self.call("inspect")
        if status != 0:
            raise Failure("inspect failed: " + output)
        section = re.search(r":WINDOWS (\(.*?\))\s*:WINDOW-GEOMETRY", output, re.S)
        if not section:
            return []
        return [int(entry) for entry in re.findall(r":ID (\d+)", section.group(1))]

    def focus(self):
        status, output = self.call("inspect")
        if status != 0:
            raise Failure("inspect failed: " + output)
        match = re.search(r":FOCUS (\d+)", output)
        return int(match.group(1)) if match else 0

    def wait_new_windows(self, known, count, seconds=20):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ids = [entry for entry in self.windows() if entry not in known]
            if len(ids) >= count:
                return ids
            time.sleep(0.2)
        return [entry for entry in self.windows() if entry not in known]

    def wait_quiet(self, seconds=10):
        deadline = time.time() + seconds
        while time.time() < deadline and self.windows():
            time.sleep(0.2)

    def focus_window(self, target, seconds=2):
        for _ in range(3):
            if self.focus_is(target, seconds):
                return True
            self.command("winstates-wm", "next")
        return self.focus_is(target, seconds)

    def focus_is(self, target, seconds=2):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.focus() == target:
                return True
            time.sleep(0.05)
        return False

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def frame(self, name=None, save=None):
        last = ""
        for _ in range(4):
            result = subprocess.run(["grim", "-t", "ppm", "-"], env=self.environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode == 0:
                image = parse_ppm(result.stdout)
                if save and name:
                    write_png(os.path.join(save, name + ".png"), *image)
                return image
            last = result.stderr.decode().strip()
            time.sleep(0.05)
        raise Failure("grim could not copy the output: " + last)

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


class Client:
    def __init__(self, work, binary, name, colour, socket=None):
        socket = socket or "winstates-%d" % os.getpid()
        self.name = name
        self.colour = colour
        self.log_path = os.path.join(work, name + ".log")
        self.log = open(self.log_path, "w")
        self.environment = dict(os.environ)
        self.environment.update({"XDG_RUNTIME_DIR": os.path.join(work, "run"),
            "WAYLAND_DISPLAY": socket})
        self.process = subprocess.Popen(
            [binary, "--mode", "xdg", "--app-id", name, "--title", name,
             "--size", "400x400", "--color", colour, "--seconds", "3600"],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while time.time() < deadline:
            if self.text().find("mapped") >= 0:
                return
            if self.process.poll() is not None:
                raise Failure(f"{name} exited while mapping:\n" + self.text())
            time.sleep(0.05)
        raise Failure(f"{name} never mapped:\n" + self.text())

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


class Factory:

    def __init__(self, work, binary):
        self.work = work
        self.binary = binary
        self.count = 0
        self.running = []

    def start(self, name, colour):
        self.count += 1
        client = Client(self.work, self.binary, "%s-%d" % (name, self.count), colour)
        self.running.append(client)
        return client


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


def write_png(path, width, height, pixels):
    import zlib
    raw = bytearray()
    stride = width * 3
    for row in range(height):
        raw.append(0)
        raw += pixels[row * stride:(row + 1) * stride]

    def chunk(tag, payload):
        body = tag + payload
        return (len(payload).to_bytes(4, "big") + body +
                zlib.crc32(body).to_bytes(4, "big"))

    header = (width.to_bytes(4, "big") + height.to_bytes(4, "big") +
              bytes((8, 2, 0, 0, 0)))
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        handle.write(chunk(b"IEND", b""))


def pixel(image, x, y):
    width, height, pixels = image
    x = max(0, min(width - 1, int(x)))
    y = max(0, min(height - 1, int(y)))
    index = (y * width + x) * 3
    return tuple(pixels[index:index + 3])


def rgb(value):
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def near(a, b, tolerance=3):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def footprint(image, colour, tolerance=6):
    width, height, pixels = image
    left, top, right, bottom = width, height, -1, -1
    for y in range(0, height, 2):
        row = y * width * 3
        for x in range(0, width, 2):
            index = row + x * 3
            if (abs(pixels[index] - colour[0]) <= tolerance and
                    abs(pixels[index + 1] - colour[1]) <= tolerance and
                    abs(pixels[index + 2] - colour[2]) <= tolerance):
                left = min(left, x)
                right = max(right, x)
                top = min(top, y)
                bottom = max(bottom, y)
    if right < 0:
        return None
    return left, top, right + 2, bottom + 2


def visible_box(image, x0, y0, x1, y1):
    left, top, right, bottom = x1, y1, x0, y0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if not near(pixel(image, x, y), BACKGROUND, 8):
                left = min(left, x)
                right = max(right, x)
                top = min(top, y)
                bottom = max(bottom, y)
    if right < left or bottom < top:
        return None
    return left, top, right + 2, bottom + 2


def area(box):
    return 0 if box is None else (box[2] - box[0]) * (box[3] - box[1])


def brightest(image, colour, tolerance=90):
    width, height, pixels = image
    best = (0, (0, 0, 0))
    for y in range(0, height, 2):
        row = y * width * 3
        for x in range(0, width, 2):
            index = row + x * 3
            seen = (pixels[index], pixels[index + 1], pixels[index + 2])
            if all(abs(a - b) <= tolerance for a, b in zip(seen, colour)):
                total = sum(seen)
                if total > best[0]:
                    best = (total, seen)
    return best[1]


def wait_pixel(compositor, point, colour, tolerance=4, seconds=3, save=None, name="wait"):
    deadline = time.time() + seconds
    while True:
        image = compositor.frame(name, save)
        if near(pixel(image, *point), colour, tolerance):
            return image
        if time.time() > deadline:
            return image
        time.sleep(0.05)


def frames(compositor, count, interval, save, prefix):
    captured = []
    for index in range(count):
        captured.append((time.time(), compositor.frame(f"{prefix}-{index}", save)))
        time.sleep(interval)
    return captured


def first_success(times, run):
    error = None
    for _ in range(times):
        try:
            return run()
        except AssertionError as failure:
            error = failure
    raise error


def tile(index):
    return (10 + index * 420, 0, 10 + index * 420 + 400, 420)


def wait_drawn(compositor, colours, seconds=20):
    deadline = time.time() + seconds
    while time.time() < deadline:
        image = compositor.frame()
        if all(footprint(image, rgb(colour), 12) for colour in colours):
            return image
        time.sleep(0.1)
    raise Failure(f"windows {colours} never drew")


def start_pair(compositor, factory, prefix):
    known = compositor.windows()
    left = factory.start(prefix + "-left", "ffff00")
    right = factory.start(prefix + "-right", "00a0ff")
    ids = compositor.wait_new_windows(known, 2)[:2]
    assert len(ids) == 2, f"expected two new windows, saw {ids}"
    wait_drawn(compositor, [left.colour, right.colour])
    return ids


def scenario_inactive(compositor, factory, frames_dir):
    ids = start_pair(compositor, factory, "inactive")
    left = factory.running[0]
    right = factory.running[1]
    compositor.mount(os.path.join(EXAMPLES, "inactive.lisp"))
    assert compositor.focus_window(ids[0]), f"window {ids[0]} never held focus"
    image = wait_pixel(compositor, (210, 200), rgb(left.colour), 3, 4,
                       frames_dir, "inactive-first")
    focused = pixel(image, 210, 200)
    unfocused = pixel(image, 630, 200)
    assert near(focused, rgb(left.colour), 3), \
        f"the focused window is not its own colour: {focused} vs {left.colour}"
    assert not near(unfocused, rgb(right.colour), 3), \
        "the unfocused window ignored :alpha and :tint"
    scale = sum(unfocused) / sum(rgb(right.colour))
    assert 0.35 < scale < 0.8, \
        f"the unfocused window is not dimmed as expected: {unfocused} vs {right.colour}"
    assert pixel(image, 630, 380) != BACKGROUND, "the unfocused window is fully transparent"
    compositor.command("winstates-wm", "next")
    assert compositor.focus_is(ids[1]), f"focus never moved to window {ids[1]}"
    image = wait_pixel(compositor, (630, 200), rgb(right.colour), 3, 4,
                       frames_dir, "inactive-second")
    was_unfocused = pixel(image, 210, 200)
    was_focused = pixel(image, 630, 200)
    assert near(was_focused, rgb(right.colour), 3), \
        f"the newly focused window stayed dimmed: {was_focused}"
    assert not near(was_unfocused, rgb(left.colour), 3), \
        "the window that lost focus was not dimmed"
    return f"focused {right.colour} kept, unfocused drained to {unfocused}"


def scenario_flash(compositor, factory, frames_dir):
    ids = start_pair(compositor, factory, "flash")
    compositor.mount(os.path.join(EXAMPLES, "focus-flash.lisp"))
    assert compositor.focus_window(ids[0]), f"window {ids[0]} never held focus"
    settled = wait_pixel(compositor, (630, 200), rgb(factory.running[1].colour), 3, 4,
                         frames_dir, "flash-settled")
    bases = [visible_box(settled, *tile(index)) for index in (0, 1)]
    assert all(bases) and all(area(base) > 100000 for base in bases), \
        f"both windows should be on screen: {bases}"

    def pulse():
        compositor.command("winstates-wm", "next")
        captured = frames(compositor, 16, 0.015, frames_dir, "flash")
        for index, client in enumerate(factory.running):
            base = bases[index]
            smallest = min((area(visible_box(image, *tile(index)) or base)
                            for _, image in captured))
            flashed = max(sum(brightest(image, rgb(client.colour))) for _, image in captured)
            if smallest < area(base) * 0.95 and flashed > sum(rgb(client.colour)) + 40:
                return index, smallest, area(base), flashed
        raise AssertionError("no window shrank and flashed in the captured frames")

    index, smallest, base, flashed = first_success(3, pulse)
    colour = rgb(factory.running[index].colour)
    centre = tile(index)
    after = wait_pixel(compositor, ((centre[0] + centre[2]) // 2, 200), colour, 3, 4,
                       frames_dir, "flash-settled-again")
    restored = visible_box(after, *tile(index))
    assert restored and abs(area(restored) - base) < base * 0.04, \
        f"the window did not settle back to its size: {area(restored) if restored else None} vs {base}"
    return f"shrank to {smallest / base:.0%} of its box, peaked at {flashed}, settled back"


def scenario_close(compositor, factory, frames_dir):
    ids = start_pair(compositor, factory, "close")
    right = factory.running[1]
    compositor.mount(os.path.join(EXAMPLES, "close-fade.lisp"))
    assert compositor.focus_window(ids[0]), f"window {ids[0]} never held focus"
    before = wait_pixel(compositor, (630, 200), rgb(right.colour), 3, 4,
                        frames_dir, "close-before")
    box = footprint(before, rgb(right.colour), 12)
    assert box, "the closing window is not on screen"
    centre = ((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
    started = time.time()
    right.stop()
    captured = []
    for index in range(24):
        captured.append((time.time() - started, compositor.frame("close-%d" % index, frames_dir)))
        if time.time() - started > 0.5:
            break
    fade = [(elapsed, sum(pixel(image, *centre))) for elapsed, image in captured]
    visible = [(elapsed, level) for elapsed, level in fade if level > sum(BACKGROUND) + 6]
    assert len(visible) >= 2, "the closed window vanished without its close animation"
    assert visible[-1][0] > 0.03, f"the close animation lasted {visible[-1][0]:.3f}s"
    assert visible[-1][1] < visible[0][1], f"the closing window did not fade: {fade[:4]}"
    assert fade[-1][1] <= sum(BACKGROUND) + 6, "the closing window never left the screen"
    solid = sum(BACKGROUND) + (visible[0][1] - sum(BACKGROUND)) * 0.5
    boxes = [area(visible_box(image, box[0] + 4, box[1] + 4, box[2] - 4, box[3] - 4))
             for elapsed, image in captured
             if sum(pixel(image, *centre)) > solid]
    boxes = [size for size in boxes if size]
    assert boxes and min(boxes) < boxes[0] * 0.995, \
        f"the closing window did not shrink: {boxes}"
    return (f"faded {visible[0][1]} to {visible[-1][1]} over {visible[-1][0]:.2f}s, "
            f"down to {min(boxes) / boxes[0]:.0%} of its frame while still visible")


def scenario_switch(compositor, factory, frames_dir):
    known = compositor.windows()
    left = factory.start("spring-left", "ffff00")
    compositor.mount(os.path.join(EXAMPLES, "workspace-spring.lisp"))
    assert len(compositor.wait_new_windows(known, 1)) == 1, "the first window never mapped"
    wait_drawn(compositor, [left.colour])
    compositor.command("workspace-spring", "tag2")
    known = compositor.windows()
    right = factory.start("spring-right", "00a0ff")
    ids = compositor.wait_new_windows(known, 1)
    assert len(ids) == 1, f"expected one new window, saw {ids}"
    time.sleep(0.4)

    def swap():
        compositor.command("workspace-spring", "tag1")
        settled = wait_pixel(compositor, (200, 200), rgb(left.colour), 6, 3,
                             frames_dir, "switch-one")
        base = footprint(settled, rgb(left.colour), 12)
        assert base and area(base) > 100000, f"the first window is not on screen: {base}"
        compositor.command("workspace-spring", "tag2")
        captured = frames(compositor, 16, 0.02, frames_dir, "switch")
        growing = [area(footprint(image, rgb(right.colour), 55)) for _, image in captured]
        growing = [size for size in growing if size]
        assert growing, "the arriving window never drew"
        faded = [sum(pixel(image, 900, 200)) for _, image in captured]
        assert min(faded) < max(faded) * 0.8, \
            f"the arriving window did not fade in: {min(faded)} then {max(faded)}"
        assert min(growing) < max(growing) * 0.98, \
            f"the arriving window did not spring in: {min(growing)} then {max(growing)}"
        return base, growing

    base, growing = first_success(3, swap)
    after = wait_pixel(compositor, (900, 200), rgb(right.colour), 4, 4,
                       frames_dir, "switch-settled")
    landed = footprint(after, rgb(right.colour), 12)
    assert landed and abs(area(landed) - area(base)) < area(base) * 0.05, \
        f"the arriving window did not settle where it belongs: {landed} vs {base}"
    leftover = footprint(after, rgb(left.colour), 12)
    assert area(leftover) < area(base) * 0.05, f"the old workspace is still on screen: {leftover}"
    return (f"arriving window grew {min(growing)} to {max(growing)} px, "
            f"landed at {area(landed)} px")


def scenario_state(compositor, factory, frames_dir):
    path = os.path.join(factory.work, "rotate.lisp")
    with open(path, "w") as handle:
        handle.write(ROTATE)
    known = compositor.windows()
    left = factory.start("state-left", "ffff00")
    assert len(compositor.wait_new_windows(known, 1)) == 1, "the window never mapped"
    upright = wait_drawn(compositor, [left.colour])
    box = footprint(upright, rgb(left.colour), 12)
    assert box, "the window is not on screen"
    corner = (box[0] + 6, box[1] + 6)
    assert near(pixel(upright, *corner), rgb(left.colour), 12)
    compositor.mount(path)
    time.sleep(0.5)
    turned = compositor.frame("state-turned", frames_dir)
    assert near(pixel(turned, *corner), BACKGROUND, 8), \
        "a rotated window still fills the corner it started in"
    shifted = visible_box(turned, 0, 0, 1280, 720)
    assert shifted, "the rotated window is not drawn"
    grew = ((shifted[2] - shifted[0]) * (shifted[3] - shifted[1]) >
            (box[2] - box[0]) * (box[3] - box[1]) * 1.2)
    assert grew, f"a rotated square did not grow its frame: {box} then {shifted}"
    moved = ((shifted[0] + shifted[2] - box[0] - box[2]) // 2,
             (shifted[1] + shifted[3] - box[1] - box[3]) // 2)
    assert 50 < moved[0] < 70 and 20 < moved[1] < 40, \
        f"the translated window moved {moved}, wanted about (60, 30)"
    return f"corner cleared, frame {box} to {shifted}, moved {moved}"


def main():
    parser = argparse.ArgumentParser(description="Judge the window-state examples on a "
        "private headless compositor: inactive, flash, close, switch and state.")
    parser.add_argument("--tomoe", required=True, help="the tomoe launcher, from a checkout")
    parser.add_argument("--client", required=True, help="a tomoe-test-client binary")
    parser.add_argument("--frames", default=None, help="keep every judged frame here")
    parser.add_argument("--only", default=None, help="run one scenario")
    arguments = parser.parse_args()
    frames_dir = arguments.frames or tempfile.mkdtemp(prefix="winstates-frames-")
    os.makedirs(frames_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="winstates-")
    policy = os.path.join(work, "wm.lisp")
    with open(policy, "w") as handle:
        handle.write(POLICY)
    compositor = None
    factory = None
    failures = 0
    try:
        compositor = Compositor(work, arguments.tomoe)
        compositor.mount(policy)
        factory = Factory(work, arguments.client)
        scenarios = [("inactive", scenario_inactive),
                     ("flash", scenario_flash),
                     ("close", scenario_close),
                     ("switch", scenario_switch),
                     ("state", scenario_state)]
        for name, run in scenarios:
            if arguments.only and arguments.only != name:
                continue
            for client in factory.running:
                client.stop()
            factory.running.clear()
            compositor.wait_quiet()
            for document in ("inactive", "focus-flash", "close-fade", "workspace-spring"):
                compositor.call("unmount", document)
            try:
                detail = run(compositor, factory, frames_dir)
                print(f"{name}: ok, {detail}", flush=True)
            except Failure as failure:
                failures += 1
                print(f"{name}: FAILED: {failure}", flush=True)
                print(compositor.text()[-4000:], flush=True)
        print(f"frames: {frames_dir}", flush=True)
    finally:
        if factory:
            for client in factory.running:
                client.stop()
        if compositor:
            compositor.call("quit")
            compositor.stop()
        if os.environ.get("WINSTATES_KEEP"):
            print(f"work: {work}", flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
