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

SOCKET = "horosphere"
WIDTH, HEIGHT = 1280, 720
DISK = (WIDTH // 2, HEIGHT // 2, min(WIDTH, HEIGHT) // 2 - 16)
WINDOW = (400, 300)

BASE_LAYOUT = """(in-package #:tomoe-user)

(define-extension "horosphere-strip" (:reads (:windows :outputs) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (width (floor (/ (getf output :width 1280) 4)))
         (height (floor (/ (getf output :height 720) 2))))
    (values nil
            (loop for window in windows
                  for index from 0 below 8
                  collect (place (getf window :id)
                                 (* width (mod index 4)) (* height (floor index 4))
                                 width height t))
            nil)))
"""

TOKEN = re.compile(r'\s*(?:(\()|(\))|("(?:[^"\\]|\\.)*")|([^\s()"]+))')


class Failure(Exception):
    pass


def read(text):
    stack = [[]]
    for match in TOKEN.finditer(text):
        if match.group(1):
            stack.append([])
        elif match.group(2):
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(match.group(3) or match.group(4))
    return stack[0][0]


def plist(value):
    return dict(zip(value[::2], value[1::2])) if isinstance(value, list) else {}


def items(value):
    return value if isinstance(value, list) else []


def text(value):
    if isinstance(value, str) and value.startswith('"'):
        return re.sub(r'\\(.)', r'\1', value[1:-1])
    return value


def real(value):
    return float(re.sub(r"[dDfFsSlL][-+]?\d*$", "", text(value)))


def integer(value):
    return int(real(value))


def entries(reply, key):
    return [plist(entry) for entry in items(plist(reply[2]).get(key))]


class Compositor:
    def __init__(self, work, tomoe, extension, pointer, name="horosphere", policy=None):
        self.tomoe = tomoe
        self.extension = extension
        self.pointer = pointer
        self.name = name
        self.runtime = os.path.join(work, "run")
        os.makedirs(self.runtime, mode=0o700, exist_ok=True)
        self.log_path = os.path.join(work, name + ".log")
        self.log = open(self.log_path, "w")
        self.policy = os.path.join(work, name + "-policy.lisp")
        with open(self.policy, "w") as handle:
            handle.write(BASE_LAYOUT if policy is None else policy)
        self.environment = dict(os.environ)
        self.environment.update({"XDG_RUNTIME_DIR": self.runtime, "WAYLAND_DISPLAY": SOCKET,
            "TOMOE_DEBUG": "1"})
        self.process = subprocess.Popen(
            [tomoe, "--socket", SOCKET, "--backend", "headless", "--bare", "--no-watch",
                "--config", self.policy],
            env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise Failure("tomoe exited while starting:\n" + self.text())
            if self.call("inspect")[0] == 0:
                return
            time.sleep(0.1)
        raise Failure("tomoe did not answer inspect:\n" + self.text())

    def call(self, *arguments):
        result = subprocess.run([self.tomoe, "--socket", SOCKET, *arguments],
            env=self.environment, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def succeed(self, *arguments):
        status, output = self.call(*arguments)
        if status != 0:
            raise Failure("tomoe " + " ".join(arguments) + " failed: " + output.strip())
        return output

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def inspect(self):
        reply = read(self.succeed("inspect"))
        if len(reply) < 3 or reply[1] != ":OK":
            raise Failure("unreadable inspect reply: " + str(reply)[:200])
        return reply[2]

    def windows(self):
        return entries(read(self.succeed("inspect")), ":WINDOWS")

    def layout(self):
        return [(integer(entry[":ID"]), (integer(entry[":X"]), integer(entry[":Y"]),
                 integer(entry[":WIDTH"]), integer(entry[":HEIGHT"])),
                 plist(entry.get(":PROPERTIES")))
                for entry in entries(read(self.succeed("inspect")), ":LAYOUT")]

    def extensions(self):
        return {text(entry.get(":NAME")): entry
                for entry in entries(read(self.succeed("inspect")), ":EXTENSIONS")}

    def frames(self):
        reply = self.succeed("frames")
        found = {}
        for match in re.finditer(r':NAME "([^"]+)"([^\n]*)', reply):
            fields = {key.lower(): value for key, value in
                re.findall(r':([A-Za-z-]+)\s+([-\w]+)', match.group(2))}
            found[match.group(1)] = fields
        if not found:
            raise Failure("unreadable frames reply: " + reply[:200])
        return found

    def scanout(self):
        return {name: fields.get("scanout") for name, fields in self.frames().items()}

    def wakeups(self):
        with open(f"/proc/{self.process.pid}/status") as handle:
            status = handle.read()
        return sum(int(found) for found in re.findall(r"ctxt_switches:\s+(\d+)", status))

    def drawn(self):
        return sum(int(fields.get("frame", 0)) for fields in self.frames().values())

    def screenshot(self, path):
        last = ""
        for _ in range(4):
            with open(path, "wb") as handle:
                result = subprocess.run(["grim", "-t", "ppm", "-"], env=self.environment,
                    stdout=handle, stderr=subprocess.PIPE, text=True)
            if result.returncode == 0:
                return image(path)
            last = result.stderr.strip()
            time.sleep(0.3)
        raise Failure("grim could not copy the output: " + last)

    def mount(self, path):
        self.succeed("mount", path)
        time.sleep(0.4)

    def unmount(self, name):
        self.succeed("unmount", name)
        time.sleep(0.4)

    def pointer_commands(self, *commands):
        result = subprocess.run([self.pointer, "--extent", f"{WIDTH}x{HEIGHT}", *commands],
            env=self.environment, capture_output=True, text=True)
        if result.returncode != 0:
            raise Failure("the pointer script failed: " + (result.stdout + result.stderr).strip())

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


class Client:
    def __init__(self, work, name, arguments, environment):
        self.name = name
        self.log_path = os.path.join(work, name + ".log")
        self.log = open(self.log_path, "w")
        self.process = subprocess.Popen(arguments, env=environment, cwd=work,
            stdout=self.log, stderr=subprocess.STDOUT)

    def text(self):
        self.log.flush()
        with open(self.log_path) as handle:
            return handle.read()

    def wait_line(self, wanted, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if wanted in self.text():
                return
            if self.process.poll() is not None:
                raise Failure(self.name + " exited before " + repr(wanted) + ":\n" + self.text())
            time.sleep(0.05)
        raise Failure(self.name + " never printed " + repr(wanted) + ":\n" + self.text())

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()


def image(path):
    with open(path, "rb") as handle:
        data = handle.read()
    parts = data.split(b"\n", 3)
    width, height = (int(value) for value in parts[1].split())
    return width, height, parts[3]


def pixel(picture, x, y):
    width, height, pixels = picture
    x = max(0, min(width - 1, int(round(x))))
    y = max(0, min(height - 1, int(round(y))))
    at = (y * width + x) * 3
    return tuple(pixels[at:at + 3])


def close(first, second, tolerance=10):
    return all(abs(a - b) <= tolerance for a, b in zip(first, second))


def tint(value):
    return tuple(int(value[index:index + 2], 16) for index in (0, 2, 4))


def box(entry):
    return (integer(entry[":X"]), integer(entry[":Y"]),
            integer(entry[":WIDTH"]), integer(entry[":HEIGHT"]))


def contains(field, point):
    x, y, width, height = field
    return x <= point[0] < x + width and y <= point[1] < y + height


def area(field):
    return field[2] * field[3]


def central(layout, point):
    hits = [(key, field) for key, field, _ in layout if contains(field, point)]
    if hits:
        return max(hits, key=lambda hit: area(hit[1]))
    return min(layout, key=lambda entry: abs(entry[1][0] + entry[1][2] / 2 - point[0]) +
               abs(entry[1][1] + entry[1][3] / 2 - point[1]))[:2]


def meshed(layout):
    return [(key, field) for key, field, properties in layout if ":MESH" in properties]


def boundary(picture, x, top, tone, limit=260):
    for y in range(max(0, top), min(picture[1], top + limit)):
        if close(pixel(picture, x, y), tone, 14):
            return y
    return None


def mesh_vertices(mesh, grid):
    nx, ny = grid
    points = [(real(mesh[2 * index]), real(mesh[2 * index + 1]))
              for index in range((nx + 1) * (ny + 1))]
    return nx, ny, points


def barycentric(point, first, second, third):
    denominator = ((second[1] - third[1]) * (first[0] - third[0]) +
                   (third[0] - second[0]) * (first[1] - third[1]))
    if denominator == 0:
        return None
    one = ((second[1] - third[1]) * (point[0] - third[0]) +
           (third[0] - second[0]) * (point[1] - third[1])) / denominator
    two = ((third[1] - first[1]) * (point[0] - third[0]) +
           (first[0] - third[0]) * (point[1] - third[1])) / denominator
    three = 1 - one - two
    return (one, two, three) if min(one, two, three) >= 0 else None


def mesh_area(mesh, grid):
    nx, ny, points = mesh_vertices(mesh, grid)
    boundary = ([points[column] for column in range(nx + 1)] +
                [points[row * (nx + 1) + nx] for row in range(1, ny + 1)] +
                [points[ny * (nx + 1) + column] for column in range(nx - 1, -1, -1)] +
                [points[row * (nx + 1)] for row in range(ny - 1, 0, -1)])
    total = 0.0
    for index, current in enumerate(boundary):
        following = boundary[(index + 1) % len(boundary)]
        total += current[0] * following[1] - following[0] * current[1]
    return abs(total) / 2.0


def mesh_surface(mesh, grid, point, size):
    nx, ny, points = mesh_vertices(mesh, grid)
    for row in range(ny):
        for column in range(nx):
            corner = row * (nx + 1) + column
            first = points[corner]
            second = points[corner + 1]
            third = points[corner + nx + 1]
            fourth = points[corner + nx + 2]
            for triangle, (one, two, three) in (
                    (0, (first, second, third)), (1, (second, fourth, third))):
                weights = barycentric(point, one, two, three)
                if not weights:
                    continue
                along = weights[0] + weights[1] if triangle else weights[1]
                down = weights[1] if triangle else weights[2]
                return ((column + along) / nx * size[0], (row + down) / ny * size[1])
    return None


def wait_for(description, check, timeout=20):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            found = check()
        except Failure as failure:
            last = str(failure)
            found = None
        if found:
            return found
        time.sleep(0.2)
    raise Failure(description + " never happened" + ((": " + last) if last else ""))


def idle(compositor, seconds):
    drawn = compositor.drawn()
    wakes = compositor.wakeups()
    time.sleep(seconds)
    return compositor.drawn() - drawn, compositor.wakeups() - wakes


def filled(picture, field, tone, inset=5, step=4):
    hits = total = 0
    for y in range(field[1] + inset, field[1] + field[3] - inset, step):
        for x in range(field[0] + inset, field[0] + field[2] - inset, step):
            total += 1
            if close(pixel(picture, x, y), tone, 22):
                hits += 1
    return hits / max(1, total)


def judge_rectangles(layout, picture, colours, stage):
    for key, field, _ in layout:
        if key not in colours:
            continue
        tone = pixel(picture, field[0] + field[2] / 2, field[1] + field[3] / 2)
        if not close(tone, colours[key], 14):
            raise Failure(stage + f": window {key} is not drawn in the middle of {field}")
        share = filled(picture, field, colours[key])
        if share < 0.9:
            raise Failure(stage + f": window {key} fills only {share:.2f} of {field}")


def judge_tiles(layout, picture, colours):
    if len(layout) < 5:
        raise Failure(f"only {len(layout)} windows were placed on the tiling")
    fields = [field for _, field, _ in layout]
    if len(set(fields)) != len(fields):
        raise Failure("two windows share a tile: " + repr(fields))
    if len(meshed(layout)) != len(layout):
        raise Failure(f"only {len(meshed(layout))} of {len(layout)} windows got a mesh")
    shapes = []
    for key, field, properties in layout:
        if key not in colours:
            continue
        tone = pixel(picture, field[0] + field[2] / 2, field[1] + field[3] / 2)
        if not close(tone, colours[key], 14):
            raise Failure(f"window {key} is not drawn in the middle of its tile {field}")
        fill = tile_area(properties) / (field[2] * field[3])
        if not 0.3 < fill < 0.9:
            raise Failure(f"window {key} fills {fill:.2f} of its box {field}: "
                          f"its edges are not geodesic arcs")
        grid = properties[":MESH-GRID"]
        if integer(grid[0]) < 8 or integer(grid[-1]) < 8:
            raise Failure(f"window {key} has only a {integer(grid[0])} by {integer(grid[-1])} mesh")
        shapes.append(fill)
    centre = owner(layout, DISK[:2])
    if not centre:
        raise Failure("no tile covers the middle of the disk")
    depth = boundary(picture, centre[1][0] + centre[1][2] // 2, centre[1][1],
                     colours[centre[0]])
    if depth is None or depth - centre[1][1] < 3:
        raise Failure(f"the central tile starts at the top of its box {centre[1]}: straight edge")
    return [f"tiles: {len(layout)} windows on {len(fields)} distinct tiles, each drawn to "
            f"{min(shapes):.2f}-{max(shapes):.2f} of its box, and the central tile begins "
            f"{depth - centre[1][1]} px below the top of its box"]


def tile_area(properties):
    return mesh_area(*mesh_deformed(properties))


def mesh_deformed(properties):
    grid = properties[":MESH-GRID"]
    return properties[":MESH"], (integer(grid[0]), integer(grid[-1]))


def owner(layout, point):
    hits = []
    for key, field, properties in layout:
        if ":MESH" not in properties:
            continue
        mesh, grid = mesh_deformed(properties)
        if mesh_surface(mesh, grid, (point[0] - field[0], point[1] - field[1]), (1, 1)):
            hits.append((key, field, properties))
    if not hits:
        return None
    return max(hits, key=lambda entry: tile_area(entry[2]))


def judge_pan(before, after):
    first = owner(before, DISK[:2])
    second = owner(after, DISK[:2])
    if first is None:
        raise Failure("no tile covers the middle of the disk before the drag")
    if second is None:
        raise Failure("no tile covers the middle of the disk after the drag: "
                      + repr([(key, field) for key, field, _ in after]))
    if first[0] == second[0]:
        raise Failure(f"the drag left window {first[0]} in the middle of the disk")
    previous = {key: properties for key, _, properties in before}
    grew = tile_area(second[2]) / max(1.0, tile_area(previous[second[0]]))
    if grew < 1.05:
        raise Failure(f"window {second[0]} became central without growing ({grew:.2f}x)")
    return [f"pan: window {first[0]} left the middle of the disk, window {second[0]} took it and "
            f"its tile grew {grew:.2f}x to {tile_area(second[2]):.0f} pixels squared"]


def configured_size(observer):
    found = re.findall(r"configured (\d+)x(\d+)", observer.text())
    if not found:
        raise Failure("the client never reported a configured size")
    return (int(found[-1][0]), int(found[-1][1]))


def judge_click(compositor, warp, layout, picture, colours, sizes):
    observer = compositor.clients[compositor.app_of[warp]]
    sizes = dict(sizes)
    sizes[warp] = configured_size(observer)
    found = [(key, field, properties) for key, field, properties in layout if key == warp]
    if not found:
        raise Failure("the reporting client was not placed on the tiling")
    key, field, properties = found[0]
    probe = (field[0] + field[2] * 0.31, field[1] + field[3] * 0.63)
    if not close(pixel(picture, probe[0], probe[1]), colours[key], 20):
        raise Failure(f"the window under the probe {probe} is not window {key}")
    mesh = properties[":MESH"]
    grid = properties[":MESH-GRID"]
    grid = (integer(grid[0]), integer(grid[-1]))
    expected = mesh_surface(mesh, grid, (probe[0] - field[0], probe[1] - field[1]), sizes[key])
    if expected is None:
        raise Failure(f"the probe {probe} is outside the mesh of window {key}")
    straight = ((probe[0] - field[0]) / field[2] * sizes[key][0],
                (probe[1] - field[1]) / field[3] * sizes[key][1])
    warp_size = max(abs(expected[0] - straight[0]), abs(expected[1] - straight[1]))
    if warp_size < 6:
        raise Failure(f"the probe is too close to the middle for a rectangle check ({warp_size:.1f})")
    compositor.pointer_commands("move %d %d" % probe, "sync", "press 0x110", "sync",
        "release 0x110", "sync", "sleep 150")
    deadline = time.time() + 10
    pressed = []
    while time.time() < deadline:
        pressed = [line for line in observer.text().splitlines()
                   if line.startswith("pointer button 272 pressed")]
        if pressed:
            break
        time.sleep(0.1)
    if not pressed:
        raise Failure("the click never reached the window:\n" + observer.text())
    got = (float(pressed[-1].split()[-2]), float(pressed[-1].split()[-1]))
    if abs(got[0] - expected[0]) > 3 or abs(got[1] - expected[1]) > 3:
        raise Failure(f"the click reached surface ({got[0]:.1f}, {got[1]:.1f}) but the mesh "
                      f"puts ({expected[0]:.1f}, {expected[1]:.1f}) there")
    return [f"click: screen ({probe[0]:.0f}, {probe[1]:.0f}) inside warped window {key} "
            f"reached surface ({got[0]:.1f}, {got[1]:.1f}), matching the mesh to "
            f"{max(abs(got[0] - expected[0]), abs(got[1] - expected[1])):.1f} px; "
            f"a rectangle would have said ({straight[0]:.1f}, {straight[1]:.1f})"]


def scanout_scenario(work, tomoe, dmabuf, pointer, extension, arguments, report):
    compositor = Compositor(work, os.path.abspath(tomoe), "", os.path.abspath(pointer),
        name="scanout", policy="(in-package #:tomoe-user)\n")
    client = None

    def engagements():
        return compositor.text().count("direct scanout engaged")

    def trigger():
        subprocess.run(["grim", "-t", "ppm", os.path.join(work, "scanout.ppm")],
            env=compositor.environment, capture_output=True)

    try:
        client = Client(work, "scanout-client", [os.path.abspath(dmabuf), "--colour", "204060",
            "--seconds", "600"], compositor.environment)
        client.wait_line("mapped")
        compositor.pointer_commands("move %d %d" % DISK[:2], "sync", "sleep 100", "sync")
        client.wait_line("hidden", timeout=20)
        time.sleep(0.3)

        def engaged():
            trigger()
            return engagements() > 0

        wait_for("direct scanout to engage under a full screen client", engaged, timeout=20)
        before = engagements()
        compositor.mount(os.path.abspath(extension))
        wait_for("the mesh to take the window out of scanout",
                 lambda: compositor.scanout().get("HEADLESS-1", "").lower() == "nil" and
                 compositor.text().count("direct scanout disengaged") > 0, timeout=20)
        compositor.unmount("horosphere")

        def returned():
            trigger()
            return engagements() > before

        wait_for("direct scanout to return once the extension is gone", returned, timeout=20)
        report.append(f"scanout: a full screen dmabuf client engaged direct scanout "
                      f"({before} time{'s' if before > 1 else ''} before the mount), stopped "
                      f"while the extension warped it, and engaged again after the unmount")
    finally:
        if client:
            client.stop()
        compositor.stop()


def frame_cost(before, after):
    rows = []
    for name, fields in after.items():
        old = before.get(name)
        if not old:
            continue
        frames = int(fields.get("frame", 0)) - int(old.get("frame", 0))
        cost = int(fields.get("frame-ns", 0)) - int(old.get("frame-ns", 0))
        if frames:
            rows.append(f"{frames} frames, {cost / frames / 1e6:.2f} ms each, {name}")
    return "; ".join(rows) if rows else "no frames drawn while dragging"


def to_png(source, destination):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", source, "-frames:v", "1", destination],
        check=True)


def ensure(path):
    os.makedirs(path, exist_ok=True)
    return path


def compare_scanout(tomoe, pointer, work, observed, report):
    base = Compositor(work, os.path.abspath(tomoe), "", pointer, name="base")
    client = None
    try:
        client = Client(work, "base", [os.path.abspath(ARGUMENTS.client), "--mode", "xdg",
            "--app-id", "101", "--size", "400x300", "--seconds", "120"], base.environment)
        client.wait_line("mapped")
        time.sleep(1.2)
        decision = base.scanout()
    finally:
        if client:
            client.stop()
        base.stop()
    if decision != observed:
        raise Failure(f"a base build reports scanout {decision}, this one {observed}")
    report.append(f"scanout: a base build agrees with {observed} for the same plain policy")


def main():
    global ARGUMENTS
    parser = argparse.ArgumentParser(description="Check that Tomoe tiles a hyperbolic disk.")
    parser.add_argument("--tomoe", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--warp-client", required=True)
    parser.add_argument("--pointer", required=True)
    parser.add_argument("--dmabuf-client", default=None,
        help="client that commits a full screen dmabuf, used for the scanout check")
    parser.add_argument("--extension", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "examples", "horosphere.lisp"))
    parser.add_argument("--base", default=None,
        help="another tomoe build whose scanout decision the unmounted one must match")
    parser.add_argument("--output", required=True, help="directory for the judged PNG frames")
    parser.add_argument("--idle-seconds", type=float, default=3)
    parser.add_argument("--work", default=None)
    ARGUMENTS = parser.parse_args()
    if not shutil.which("grim") or not shutil.which("ffmpeg"):
        raise Failure("grim and ffmpeg must be on PATH")

    work = ARGUMENTS.work or tempfile.mkdtemp(prefix="tomoe-horosphere-")
    ensure(work)
    output = ensure(ARGUMENTS.output)
    report = []
    clients = {}
    compositor = None
    try:
        compositor = Compositor(work, os.path.abspath(ARGUMENTS.tomoe),
            os.path.abspath(ARGUMENTS.extension), os.path.abspath(ARGUMENTS.pointer))
        compositor.clients = clients
        plain = {"101": tint("e04a3c"), "102": tint("3c9ee0"), "103": tint("e0c93c"),
                 "104": tint("5ad27a"), "105": tint("e07ac0"), "106": tint("7ae0d0"),
                 "107": tint("c0a05a")}
        tones = dict(plain)
        tones["108"] = tint("a05ad2")
        for app in sorted(plain):
            clients[app] = Client(work, "plain" + app, [os.path.abspath(ARGUMENTS.client),
                "--mode", "xdg", "--app-id", app, "--size", "%dx%d" % WINDOW,
                "--color", "".join("%02x" % part for part in tones[app]),
                "--seconds", "1800"], compositor.environment)
        options = ["--mode", "xdg", "--app-id", "108", "--size", "%dx%d" % WINDOW,
                   "--color", "a05ad2", "--seconds", "1800"]
        clients["108"] = Client(work, "warp", [os.path.abspath(ARGUMENTS.warp_client)] + options,
            compositor.environment)
        clients["108"].wait_line("ready")

        def placed():
            layout = compositor.layout()
            good = [(key, field, properties) for key, field, properties in layout
                    if field[2] > 0 and field[3] > 0]
            return good if len(good) == len(tones) else None

        wait_for("every window to settle on the plain strip", placed)
        base_layout = placed()
        records = {integer(entry.get(":ID")): entry for entry in compositor.windows()}
        identities = {key: text(entry.get(":APP-ID")) for key, entry in records.items()}
        colours = {key: tones[app] for key, app in identities.items() if app in tones}
        sizes = {key: (integer(entry[":WIDTH"]), integer(entry[":HEIGHT"]))
                 for key, entry in records.items()}
        compositor.app_of = identities
        warp = [key for key, app in identities.items() if app == "108"][0]
        base_picture = compositor.screenshot(os.path.join(work, "base.ppm"))
        judge_rectangles(base_layout, base_picture, colours, "before the mount")
        idle_base = idle(compositor, ARGUMENTS.idle_seconds)
        scanout_base = compositor.scanout()

        compositor.mount(ARGUMENTS.extension)
        wait_for("the mesh to reach every window",
                 lambda: len(meshed(compositor.layout())) == len(tones))
        tiled = placed()
        picture = compositor.screenshot(os.path.join(work, "tiles.ppm"))
        report += judge_tiles(tiled, picture, colours)
        to_png(os.path.join(work, "tiles.ppm"), os.path.join(output, "01-tiles.png"))

        idle_disk = idle(compositor, ARGUMENTS.idle_seconds)
        report.append(f"idle: {idle_base[0]} frames and {idle_base[1]} wakeups on the plain "
                      f"strip, {idle_disk[0]} frames and {idle_disk[1]} wakeups on the disk, "
                      f"in {ARGUMENTS.idle_seconds:.1f} s")
        if idle_disk[0] > 0:
            raise Failure(f"the idle disk drew {idle_disk[0]} frames")

        commands = ["move %d %d" % DISK[:2], "sync", "press 0x112", "sync"]
        for step in range(1, 7):
            commands += ["move %d %d" % (DISK[0] + 20 * step, DISK[1]), "sleep 25"]
        commands += ["sync", "sleep 250", "release 0x112", "sync"]
        before_drag = compositor.frames()
        compositor.pointer_commands(*commands)
        time.sleep(0.6)
        after_drag = compositor.frames()
        panned = placed()
        report += judge_pan(tiled, panned)
        report.append("pan cost: " + frame_cost(before_drag, after_drag))
        compositor.screenshot(os.path.join(work, "panned.ppm"))
        to_png(os.path.join(work, "panned.ppm"), os.path.join(output, "02-panned.png"))

        time.sleep(0.5)
        records = {integer(entry.get(":ID")): entry for entry in compositor.windows()}
        sizes = {key: (integer(entry[":WIDTH"]), integer(entry[":HEIGHT"]))
                 for key, entry in records.items()}
        picture = compositor.screenshot(os.path.join(work, "click.ppm"))
        report += judge_click(compositor, warp, panned, picture, colours, sizes)
        to_png(os.path.join(work, "click.ppm"), os.path.join(output, "03-click.png"))

        scanout_disk = compositor.scanout()
        compositor.unmount("horosphere")
        if "horosphere" in compositor.extensions():
            raise Failure("the extension is still mounted after unmount")

        def restored():
            layout = compositor.layout()
            if len(layout) != len(base_layout):
                return None
            if any(":MESH" in properties for _, _, properties in layout):
                return None
            return [(key, field) for key, field, _ in layout]

        after = wait_for("the plain layout to return", restored)

        def refilled():
            for key, field in after:
                app = compositor.app_of.get(key)
                client = compositor.clients.get(app)
                if client and configured_size(client) != (field[2], field[3]):
                    return None
            return True

        wait_for("the clients to refill their plain boxes", refilled, timeout=15)
        if sorted(after) != sorted((key, field) for key, field, _ in base_layout):
            raise Failure("unmounting left a different layout: " + repr(after))
        after_picture = compositor.screenshot(os.path.join(work, "unmounted.ppm"))
        judge_rectangles([(key, field, {}) for key, field in after], after_picture, colours,
                         "after the unmount")
        to_png(os.path.join(work, "unmounted.ppm"), os.path.join(output, "04-unmounted.png"))
        report.append(f"unmount: {len(after)} windows back on the plain strip, no mesh left")
        scanout_after = compositor.scanout()
        if scanout_after != scanout_base:
            raise Failure(f"unmounting changed the scanout decision: {scanout_base} then "
                          f"{scanout_after}")
        if any(str(value).lower() == "t" for value in scanout_disk.values()):
            raise Failure("a window with a mesh was scanned out: " + repr(scanout_disk))
        report.append(f"scanout: shared-memory windows report {scanout_base} while unmounted and "
                      f"{scanout_disk} with meshes; only dmabuf clients can be scanned out")
        failed = [name for name, entry in compositor.extensions().items()
                  if text(entry.get(":LAST-ERROR")) != "NIL"]
        if failed:
            raise Failure("extensions reported errors: " + ", ".join(failed))
        for client in clients.values():
            client.stop()
        clients = {}
        compositor.stop()
        compositor = None
        if ARGUMENTS.base:
            compare_scanout(ARGUMENTS.base, ARGUMENTS.pointer,
                            ensure(os.path.join(work, "base")), scanout_base, report)
        if ARGUMENTS.dmabuf_client:
            scanout_scenario(ensure(os.path.join(work, "scanout")), ARGUMENTS.tomoe,
                             ARGUMENTS.dmabuf_client, ARGUMENTS.pointer, ARGUMENTS.extension,
                             ARGUMENTS, report)
        for line in report:
            print(line)
        print(f"horosphere: {len(report)} checks passed; frames kept in {output}")
    finally:
        for client in clients.values():
            client.stop()
        if compositor:
            compositor.stop()


if __name__ == "__main__":
    try:
        main()
    except Failure as failure:
        print("horosphere: " + str(failure), file=sys.stderr)
        print("horosphere: work directory kept for inspection", file=sys.stderr)
        sys.exit(1)
