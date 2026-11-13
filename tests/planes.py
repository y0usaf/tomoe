CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
DEVICE = f"/dev/dri/{CARD}"
DRM = {"TOMOE_DEBUG": "1", "LIBSEAT_BACKEND": "noop"}
PLANES = "tomoe-planes-probe"
OUTPUT = "Virtual-1"
COUNTERS = (":FRAME", ":DRAWN-FRAME", ":BUFFER-AGE", ":DRAWN-PIXELS", ":PIXELS", ":DRAWS",
            ":PLANES", ":DEFERRALS", ":FRAME-NS")


def plane_state():
    found = []
    for line in machine.succeed(f"{PLANES} dump {DEVICE}").splitlines():
        if not line.startswith("plane "):
            continue
        words = line.split()
        entry = {"id": words[1], "dst": None, "src": None}
        for word in words[2:]:
            name, _, value = word.partition("=")
            entry[name] = value
        found.append(entry)
    return found


def plane(kind):
    for entry in plane_state():
        if entry.get("type") == kind:
            return entry
    raise AssertionError(f"the output has no {kind} plane")


def box_of(entry):
    return tuple(int(value) for value in
                 entry["dst"].replace(",", "x").replace("+", "x").split("x"))


def frames():
    reply = read(cli("frames"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(reply[2]):
        entry = plist(entry)
        if text(entry[":NAME"]) == OUTPUT:
            return entry
    raise AssertionError(f"{OUTPUT} is missing from frames: {show(reply)}")


def counters():
    entry = frames()
    return {key: int(entry[key]) for key in COUNTERS if key in entry}


def output_size():
    for entry in items(inspect()[":OUTPUTS"]):
        entry = plist(entry)
        if text(entry[":NAME"]) == OUTPUT:
            return int(entry[":WIDTH"]), int(entry[":HEIGHT"])
    raise AssertionError(f"{OUTPUT} is missing from inspect")


def probe(command):
    cli(f"command {command}")


def wait_for(condition, what, timeout=20):
    deadline = time.monotonic() + timeout
    seen = None
    while time.monotonic() < deadline:
        seen = condition()
        if seen:
            return
        time.sleep(0.1)
    raise AssertionError(f"{what} did not happen; last seen {seen}")


def overlay_holds(box):
    def check():
        entry = plane("OVERLAY")
        return entry.get("fb") != "0" and box_of(entry) == box
    return check


def settled(kind, timeout=20):
    deadline = time.monotonic() + timeout
    last = None
    stable = 0
    while time.monotonic() < deadline:
        now = box_of(plane(kind))
        stable = stable + 1 if now == last else 0
        last = now
        if stable >= 6:
            return now
        time.sleep(0.1)
    return last


def displayed(name):
    print(machine.succeed(f"{PLANES} compose {DEVICE} /run/{name}-display.ppm"))
    return machine.succeed(f"sha256sum /run/{name}-display.ppm").split()[0]


def region(path, box):
    return machine.succeed(f"python3 /run/crop.py {path} {box[0]},{box[1]},{box[2]},{box[3]} "
        "| sha256sum").split()[0]


def screenshot(name):
    machine.succeed(ENV + f"WAYLAND_DISPLAY=check grim -t ppm - > /run/{name}-shot.ppm")
    return machine.succeed(f"sha256sum /run/{name}-shot.ppm").split()[0]


def move_by(dx, dy):
    unit = f"planes-move-{dx}-{dy}".replace("-", "n")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --move {dx},{dy}"
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx moved", timeout=30)
    time.sleep(0.5)


def scene():
    machine.succeed(
        "systemd-run --unit=planes-client --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --color 336699"
    )
    machine.wait_until_succeeds("journalctl -u planes-client -o cat | grep -q '^mapped'",
        timeout=60)
    machine.wait_until_succeeds(
        f"journalctl -u planes-client -o cat | grep -qx 'mapped {SIZE[0] // 2}x{SIZE[1] // 2}'",
        timeout=30)


machine.wait_for_unit("multi-user.target")
machine.succeed(r"""cat > /run/crop.py <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
width, height = map(int, parts[1].split())
x, y, w, h = map(int, sys.argv[2].split(','))
rows = [parts[3][(y + r) * width * 3 + x * 3:(y + r) * width * 3 + (x + w) * 3]
        for r in range(h)]
sys.stdout.buffer.write(b''.join(rows))
EOF""")
machine.succeed("mount -t debugfs none /sys/kernel/debug || true")
kinds = sorted(entry["type"] for entry in plane_state())
assert "OVERLAY" in kinds and "CURSOR" in kinds, f"vkms exposes only {kinds}"
print("planes:", kinds)

experiment = machine.succeed(f"{PLANES} experiment {DEVICE}")
print(experiment)
contract = {}
for line in experiment.splitlines():
    if " ret=" in line:
        name, _, value = line.partition(" ret=")
        contract[name.split()[1]] = value
assert contract["commit-during-flip"] == "Device or resource busy", contract
assert contract["test-only-during-flip"] == "ok", contract
assert contract["async-during-flip"] != "ok", contract
assert contract["commit-after-drain"] == "ok", contract

start("--bare", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
SIZE = output_size()
WINDOW = (SIZE[0] // 2, SIZE[1] // 2)
HOME_BOX = (SIZE[0] // 8, SIZE[1] // 8, WINDOW[0], WINDOW[1])
MOVED_BOX = (SIZE[0] // 4, SIZE[1] // 4, WINDOW[0], WINDOW[1])
SHIFT = (-50, -30)
print("output:", SIZE, "window:", WINDOW, "home:", HOME_BOX, "moved:", MOVED_BOX)


def park():
    move_by(20000, 20000)


def wiggle(ms):
    unit = f"planes-wiggle-{ms}"
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --wiggle {ms}"
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -q '^waggled'", timeout=60)
    line = machine.succeed(f"journalctl -u {unit} -o cat | grep '^waggled'").split()
    time.sleep(0.5)
    return int(line[1])

park()
scene()
try:
    wait_for(overlay_holds(HOME_BOX), "the overlay plane holding the window")
except AssertionError:
    print(machine.succeed(f"{PLANES} dump {DEVICE}"))
    print(frames())
    print(cli("inspect"))
    print(machine.succeed("journalctl -u tomoe -o cat | tail -25"))
    raise
assert plane("CURSOR")["fb"] != "0", plane("CURSOR")
assert plane("PRIMARY")["fb"] != "0", plane("PRIMARY")
print("overlay: fb", plane("OVERLAY")["fb"], "dst", plane("OVERLAY")["dst"], "src",
    plane("OVERLAY")["src"])
print("cursor: fb", plane("CURSOR")["fb"], "dst", plane("CURSOR")["dst"])
print("primary: fb", plane("PRIMARY")["fb"], "dst", plane("PRIMARY")["dst"])
assert counters()[":PLANES"] == 1, counters()
probe("move")
wait_for(overlay_holds(MOVED_BOX), "the overlay plane following the window")
overlaid = counters()
probe("unmove")
wait_for(overlay_holds(HOME_BOX), "the overlay plane returning with the window")
planes_displayed = displayed("planes")
planes_shot = screenshot("planes")
planes_region = region("/run/planes-display.ppm", HOME_BOX)
planes_crop = region("/run/planes-shot.ppm", HOME_BOX)

probe("note-on")
wait_for(lambda: plane("OVERLAY")["fb"] == "0", "a notification forcing compositing")
composited = counters()
probe("note-off")
wait_for(overlay_holds(HOME_BOX), "the overlay plane returning after the notification")

parked = settled("CURSOR")
move_by(*SHIFT)
moved = settled("CURSOR")
assert (moved[0] - parked[0], moved[1] - parked[1]) == SHIFT, (parked, moved)
deferrals = int(frames()[":DEFERRALS"])
assert deferrals == 0, f"{deferrals} commits met an outstanding flip"
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", f"--drm_device {DEVICE}", backend="drm", env=dict(DRM, TOMOE_NO_PLANES="1"))
cli(f"mount {PLANES_PROBE}")
park()
scene()
assert plane("OVERLAY")["fb"] == "0", plane("OVERLAY")
assert counters()[":PLANES"] == 0, counters()
probe("move")
machine.sleep(1)
probe("unmove")
machine.sleep(1)
reference = counters()
reference_displayed = displayed("reference")
reference_shot = screenshot("reference")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", f"--drm_device {DEVICE}", backend="drm",
    env=dict(DRM, TOMOE_TEST_STALL_FENCE="1", TOMOE_TEST_STALL_FENCE_MS="900",
        TOMOE_TEST_CURSOR_EARLY="1"))
cli(f"mount {PLANES_PROBE}")
park()
scene()
wait_for(overlay_holds(HOME_BOX), "the overlay plane holding the window while flips stall")
move_by(-300, -300)
stalled = settled("CURSOR", timeout=40)
steps = wiggle(1000)
after = settled("CURSOR", timeout=40)
assert (after[0] - stalled[0], after[1] - stalled[1]) == (steps, steps), (stalled, after,
    steps)
move_by(*SHIFT)
late_cursor = settled("CURSOR", timeout=40)
assert (late_cursor[0] - after[0], late_cursor[1] - after[1]) == SHIFT, (after,
    late_cursor)
move_by(-25, -15)
recovered = settled("CURSOR", timeout=40)
assert (recovered[0] - after[0], recovered[1] - after[1]) == (SHIFT[0] - 25,
    SHIFT[1] - 15), (after, recovered)
assert overlay_holds(HOME_BOX)(), plane("OVERLAY")
late = counters()
missing = int(machine.succeed("journalctl -o cat "
    "| grep -cF 'flip event missing' || true").strip().splitlines()[-1])
assert late[":DEFERRALS"] > 0, late
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
print("stalled flips noticed:", missing, "deferrals:", late[":DEFERRALS"], "steps:", steps,
    "cursor:", stalled, after, late_cursor, recovered)
quit()

print("planes:", overlaid, "composited:", composited, "reference:", reference)
print("displayed", planes_displayed, reference_displayed, "window", planes_region,
    planes_crop, "shots", planes_shot, reference_shot)
assert planes_region == planes_crop, (planes_region, planes_crop)
assert planes_displayed == reference_displayed, (planes_displayed, reference_displayed)
window_pixels = WINDOW[0] * WINDOW[1]
assert composited[":PLANES"] == 0, composited
assert overlaid[":DRAWN-PIXELS"] <= overlaid[":PIXELS"] - window_pixels + 8192, overlaid
assert reference[":DRAWN-PIXELS"] >= window_pixels, reference
assert planes_shot == reference_shot, (planes_shot, reference_shot)

machine.succeed("systemctl stop tomoe check-bus")
