CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
DEVICE = f"/dev/dri/{CARD}"
DRM = {"TOMOE_DEBUG": "1"}
PLANES = "tomoe-planes-probe"
OUTPUT = "Virtual-1"
COUNTERS = (":FRAME", ":DRAWN-FRAME", ":BUFFER-AGE", ":DRAWN-PIXELS", ":PIXELS", ":DRAWS",
            ":PLANES", ":DEFERRALS", ":FRAME-NS", ":MISSED", ":PLANE-TESTS",
            ":PLANE-REFUSALS", ":UNDERLAY-PLAN")
STABLE = 6


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


def shift(base, observed):
    return (observed[0] - base[0], observed[1] - base[1])


def frames_all():
    reply = read(cli("frames"))
    assert reply[1] == ":OK", show(reply)
    return [plist(entry) for entry in items(reply[2])]


def frames():
    for entry in frames_all():
        if ":NAME" in entry and text(entry[":NAME"]) == OUTPUT:
            return entry
    raise AssertionError(f"{OUTPUT} is missing from frames")


def render_stats():
    for entry in frames_all():
        if ":GL-CALLS" in entry:
            return {key: int(value) for key, value in entry.items()}
    raise AssertionError("render counters missing from frames")


def counters():
    entry = frames()
    return {key: int(entry[key]) for key in COUNTERS if key in entry}


def frame_times():
    entry = frames()
    if ":FRAME-US" not in entry:
        raise AssertionError("frame times missing from frames")
    return [int(value) for value in items(entry[":FRAME-US"])]


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        raise AssertionError("no frames were timed")
    def at(fraction):
        return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]
    return ordered[len(ordered) // 2], at(0.95), ordered[-1]


def video_frame_times(what):
    times = frame_times()
    if len(times) < 10:
        raise AssertionError(f"{what}: only {len(times)} frames were timed")
    median, p95, worst = percentiles(times)
    print(f"{what}: frames={len(times)} median_us={median} p95_us={p95} worst_us={worst}",
          flush=True)
    return median, p95, worst


def same_pixels(first, second, box, what):
    assert machine.execute(f"test -s {first} && test -s {second}")[0] == 0, \
        f"{what}: a frame is missing"
    print(f"{what}: " + machine.succeed(
        f"python3 /run/ppm_diff.py {first} {second} "
        f"{','.join(str(value) for value in box)}"), flush=True)


def report_pixels(first, second, box, what):
    code, output = machine.execute(
        f"python3 /run/ppm_diff.py {first} {second} "
        f"{','.join(str(value) for value in box)}")
    print(f"{what} (exit {code}): " + " ".join(output.split()), flush=True)
    return code


def log_count(pattern):
    return int(machine.succeed(
        f"journalctl -u tomoe -o cat | grep -c '{pattern}' || true").strip().splitlines()[-1])


def feedback_stats(unit):
    log = machine.succeed(f"journalctl -u {unit} -o cat")
    rounds = [line for line in log.splitlines() if line.startswith("feedback round=")]
    reallocs = [line for line in log.splitlines() if line.startswith("realloc ")]
    return len(rounds), len(reallocs)


def switch_planes(label, trigger, want):
    before = counters()
    probe(trigger)

    def reached():
        entry = counters()
        return entry if entry[":PLANES"] == want else None

    wait_for(reached, label)
    after = counters()
    assert after[":DRAWN-FRAME"] > before[":FRAME"], (label, before, after)
    full = after[":DRAWN-PIXELS"] == after[":PIXELS"]
    reused = after[":DRAWN-PIXELS"] == 0 and after[":FRAME"] > after[":DRAWN-FRAME"]
    assert full or reused, \
        f"{label} left {after[':DRAWN-PIXELS']} of {after[':PIXELS']} pixels to draw"
    assert after[":DEFERRALS"] == before[":DEFERRALS"], (label, before, after)
    missed = after[":MISSED"] - before[":MISSED"]
    assert missed <= 1, (label, before, after)
    assert inspect()[":LAST-ERROR"] == "NIL", inspect()
    print(f"{label}: drew {after[':DRAWN-PIXELS']} pixels in frame {after[':DRAWN-FRAME']} "
          f"({'full redraw' if full else 'reused after a full redraw'}), {missed} missed "
          f"refreshes, planes {before[':PLANES']} -> {after[':PLANES']}", flush=True)
    return after


def start_video(unit, frames=5, extra="", size=None, colour="336699"):
    buf_w, buf_h = WINDOW[0] + 32, WINDOW[1] + 32
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{DMABUF_CLIENT} --format nv12 --colour {colour} "
        f"--buffer-size {buf_w}x{buf_h} "
        f"--viewport-src 16,16,{WINDOW[0]}x{WINDOW[1]} "
        f"--viewport-dst {WINDOW[0]}x{WINDOW[1]} "
        f"--frames {frames} --feedback {extra}")
    wanted = size or f"{WINDOW[0]}x{WINDOW[1]}"
    machine.wait_until_succeeds(
        f"journalctl -u {unit} -o cat | grep -qx 'mapped {wanted}'", timeout=60)


def video_stopped(unit, frames):
    machine.wait_until_succeeds(
        f"journalctl -u {unit} -o cat | grep -qx 'frames {frames}'", timeout=120)


def start_main():
    machine.execute(
        "systemd-run --unit=check-bus --property=Type=exec "
        f"dbus-daemon --session --nofork --address={BUS}")
    machine.wait_for_file("/run/check-bus")
    machine.succeed(
        "systemd-run --unit=tomoe-main --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={BUS} "
        "--setenv=HOME=/root "
        "--setenv=PATH=/run/current-system/sw/bin --setenv=MESA_LOADER_DRIVER_OVERRIDE=zink "
        f"--setenv={THREADS} --setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        "--setenv=TOMOE_DEBUG=1 "
        f"{BASELINE} --socket main --backend drm --no-watch --drm_device {DEVICE}")
    for _ in range(600):
        if machine.execute(ENV + f"{BASELINE} --socket main inspect")[0] == 0:
            return
        if machine.execute("systemctl show -P SubState tomoe-main | grep -qx running")[0] != 0:
            raise AssertionError("main stopped while starting:\n"
                + machine.succeed("journalctl -u tomoe-main -o cat | tail -30"))
        time.sleep(0.1)
    raise AssertionError("main did not answer inspect within 60 s")


def stop_main():
    cli("quit", "main")
    machine.wait_until_succeeds(
        "systemctl show -P SubState tomoe-main | grep -qx 'exited\\|failed'", timeout=60)


def flap_feedback(hold_ms):
    machine.succeed("systemctl stop tomoe check-bus")
    start("--bare", f"--test-feedback-hold-ms={hold_ms}", f"--drm_device {DEVICE}",
          backend="drm", env=DRM)
    cli(f"mount {PLANES_PROBE}")
    park()
    probe("chrome-on")
    start_video("flap-video", frames=100000, extra="--seconds 60")
    wait_for(lambda: counters()[":PLANES"] == 1, "the underlay engaging before the flap")
    start_frame = counters()[":FRAME"]
    wait_for(lambda: counters()[":FRAME"] >= start_frame + 10, "the video streaming")
    rounds_before, reallocs_before = feedback_stats("flap-video")
    counters_before = counters()
    started = time.monotonic()
    for _ in range(8):
        probe("blur-on")
        time.sleep(0.05)
        probe("blur-off")
        time.sleep(0.05)
    elapsed = time.monotonic() - started
    time.sleep(0.8)
    rounds, reallocs = feedback_stats("flap-video")
    rounds, reallocs = rounds - rounds_before, reallocs - reallocs_before
    after = counters()
    assert after[":PLANES"] == 1, after
    assert inspect()[":LAST-ERROR"] == "NIL", inspect()
    print(f"flap hold={hold_ms}ms: {rounds} feedback sets and {reallocs} reallocations in "
          f"{elapsed:.1f}s ({reallocs / elapsed:.1f} reallocations/s), "
          f"{after[':FRAME'] - counters_before[':FRAME']} frames, "
          f"{after[':DEFERRALS'] - counters_before[':DEFERRALS']} deferrals, "
          f"{after[':MISSED'] - counters_before[':MISSED']} missed refreshes", flush=True)
    machine.succeed("systemctl stop flap-video")
    quit()
    return rounds, reallocs, elapsed


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


def settled(kind, base=None, move=None, at=None, what=None, timeout=30):
    label = what or f"the {kind} plane"
    deadline = time.monotonic() + timeout
    last = None
    stable = 0
    while time.monotonic() < deadline:
        now = box_of(plane(kind))
        stable = stable + 1 if now == last else 0
        last = now
        if stable >= STABLE and (base is None or now != base) and (
                move is None or shift(base, now) == move) and (at is None or now[:2] == at):
            return now
        time.sleep(0.1)
    seen = shift(base, last) if base is not None and last is not None else None
    raise AssertionError(f"{label} did not settle at {at or base} + {move}: last seen {last} "
                         f"({seen} from the base)")


def frame_settles(what, since=None, timeout=30):
    deadline = time.monotonic() + timeout
    last = None
    stable = 0
    while time.monotonic() < deadline:
        entry = counters()
        now = (entry.get(":FRAME", 0), entry.get(":DRAWN-FRAME", 0), entry.get(":DRAWS", 0))
        stable = stable + 1 if now == last else 0
        last = now
        if stable >= STABLE and (since is None or now[0] > since.get(":FRAME", 0)):
            return now
        time.sleep(0.1)
    raise AssertionError(f"{what} did not finish drawing; last seen {last}")


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
    before = box_of(plane("CURSOR"))
    unit = f"planes-move-{dx}-{dy}".replace("-", "n")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --move {dx},{dy}"
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx moved", timeout=30)
    return before


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
    return settled("CURSOR", at=(SIZE[0], SIZE[1]), what="the parked cursor")


def wiggle(ms):
    unit = f"planes-wiggle-{ms}"
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --wiggle {ms}"
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -q '^waggled'", timeout=60)
    line = machine.succeed(f"journalctl -u {unit} -o cat | grep '^waggled'").split()
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
wait_for(lambda: counters()[":PLANES"] == 1 and box_of(plane("PRIMARY")) == HOME_BOX,
         "a notification promoting the window to an underlay plane")
note_displayed = displayed("planes-note")
note_shot = screenshot("planes-note")
assert note_displayed == note_shot, (note_displayed, note_shot)
probe("note-off")
wait_for(overlay_holds(HOME_BOX), "the overlay plane returning after the notification")

probe("blur-on")
wait_for(lambda: plane("OVERLAY")["fb"] == "0" and counters()[":PLANES"] == 0,
         "a blurred overlay forcing compositing")
composited = counters()
probe("blur-off")
wait_for(overlay_holds(HOME_BOX), "the overlay plane returning after blur")
parked = park()
move_by(*SHIFT)
moved = settled("CURSOR", base=parked, move=SHIFT, what="the cursor after a move")
assert shift(parked, moved) == SHIFT, (parked, moved)
deferrals = int(frames()[":DEFERRALS"])
assert deferrals == 0, f"{deferrals} commits met an outstanding flip"

for wrong, what in (((SHIFT[0] + 1, SHIFT[1]), "one step off the commanded move"),
                    (None, "never moved from where it started")):
    try:
        settled("CURSOR", base=parked if wrong else moved, move=wrong, timeout=1,
                what="the cursor")
    except AssertionError as error:
        print(f"negative control: a cursor {what} is rejected:", error, flush=True)
    else:
        raise AssertionError(f"a cursor {what} was reported settled")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-refuse-planes", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
scene()
assert plane("OVERLAY")["fb"] == "0", plane("OVERLAY")
assert counters()[":PLANES"] == 0, counters()
before = counters()
probe("move")
frame_settles("the window drawn at the moved position", since=before)
windowed = counters()
probe("unmove")
frame_settles("the window composited back home", since=windowed)
reference = counters()
reference_displayed = displayed("reference")
reference_shot = screenshot("reference")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-stall-fence", "--test-stall-fence-ms=900", "--test-cursor-early",
    f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
move_by(20000, 20000)
scene()
wait_for(overlay_holds(HOME_BOX), "the overlay plane holding the window while flips stall")
probe("move")
probe("unmove")
wait_for(overlay_holds(HOME_BOX), "the overlay plane returning while flips stall")
stalled = settled("CURSOR", base=move_by(-300, -300), timeout=40,
                  what="the cursor after the jump")
steps = wiggle(1000)
after = settled("CURSOR", base=stalled, move=(steps, steps), timeout=40,
                what="the cursor after the wiggle")
late_cursor = settled("CURSOR", base=move_by(*SHIFT), move=SHIFT, timeout=40,
                      what="the late cursor")
recovered = settled("CURSOR", base=move_by(-25, -15), move=(-25, -15), timeout=40,
                    what="the recovered cursor")
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

machine.succeed("cat > /run/ppm_diff.py <<'EOF'\n"
    "import sys\n"
    "def load(p):\n"
    "    raw = open(p, 'rb').read()\n"
    "    _, wh, mx, body = raw.split(b'\\n', 3)\n"
    "    w, h = map(int, wh.split())\n"
    "    return w, h, body\n"
    "w1, h1, b1 = load(sys.argv[1])\n"
    "w2, h2, b2 = load(sys.argv[2])\n"
    "assert (w1, h1) == (w2, h2) and len(b1) == len(b2)\n"
    "diffs = [abs(x - y) for x, y in zip(b1, b2)]\n"
    "mx = max(diffs)\n"
    "mean = sum(diffs) / len(diffs)\n"
    "def px(buf, x, y):\n"
    "    i = (y * w1 + x) * 3\n"
    "    return tuple(buf[i:i+3])\n"
    "hx, hy, hw, hh = map(int, sys.argv[3].split(','))\n"
    "print(f'max_diff={mx} mean_diff={mean:.5f} bar={px(b1, hx + hw // 2, hy + 10)} '\n"
    "      f'corner_out={px(b1, hx, hy + hh - 1)} video={px(b1, hx + hw // 2, hy + hh // 2)}')\n"
    "assert mx <= 3, f'max channel diff {mx} > 3'\n"
    "assert mean < 0.1, f'mean channel diff {mean} >= 0.1'\n"
    "assert px(b1, hx + hw // 2, hy + 10) == (255, 136, 0), px(b1, hx + hw // 2, hy + 10)\n"
    "assert px(b1, hx, hy + hh - 1) == px(b2, hx, hy + hh - 1)\n"
    "EOF")

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
probe("chrome-on")
buf_w, buf_h = WINDOW[0] + 32, WINDOW[1] + 32
machine.succeed(
    "systemd-run --unit=underlay-video --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
    f"{DMABUF_CLIENT} --format nv12 --colour 336699 "
    f"--buffer-size {buf_w}x{buf_h} "
    f"--viewport-src 16,16,{WINDOW[0]}x{WINDOW[1]} "
    f"--viewport-dst {WINDOW[0]}x{WINDOW[1]} "
    "--frames 60 --feedback")
wait_for(lambda: counters()[":PLANES"] == 1 and box_of(plane("PRIMARY")) == HOME_BOX,
         "the NV12 rounded-corner window under a bar engaging the underlay plane")
base_frame = counters()[":FRAME"]
wait_for(lambda: counters()[":FRAME"] >= base_frame + 5, "underlay cutout setup frames")
steady_f1 = counters()[":FRAME"]
steady_gl1 = render_stats()[":GL-CALLS"]
steady_q1 = render_stats()[":QUADS"]
machine.wait_until_succeeds(
    "journalctl -u underlay-video -o cat | grep -qx 'frames 60'", timeout=30)
steady_f2 = counters()[":FRAME"]
steady_gl2 = render_stats()[":GL-CALLS"]
steady_q2 = render_stats()[":QUADS"]
steady_drawn = counters()[":DRAWN-PIXELS"]
print(f"underlay steady: frames={steady_f2 - steady_f1} gl_delta={steady_gl2 - steady_gl1} "
      f"quads_delta={steady_q2 - steady_q1} drawn_pixels={steady_drawn}")
assert steady_f2 - steady_f1 >= 20, (steady_f1, steady_f2)
assert steady_gl2 - steady_gl1 == 0, (steady_gl1, steady_gl2)
assert steady_q2 - steady_q1 == 0, (steady_q1, steady_q2)
assert steady_drawn == 0, steady_drawn
assert counters()[":PLANES"] == 1, counters()

machine.wait_until_succeeds(
    "journalctl -u underlay-video -o cat | grep -q 'feedback .* scanout=1'", timeout=10)
displayed("underlay")
screenshot("underlay")
hb_csv = ",".join(str(v) for v in HOME_BOX)
print(machine.succeed(
    f"python3 /run/ppm_diff.py /run/underlay-display.ppm /run/underlay-shot.ppm {hb_csv}"))

wait_for(lambda: counters()[":PLANES"] == 1, "underlay restored after screenshot")
probe("blur-on")
wait_for(lambda: counters()[":PLANES"] == 0, "blur disengaging underlay")
machine.wait_until_succeeds(
    "test $(journalctl -u underlay-video -o cat | grep -c 'scanout=0') -ge 2", timeout=10)
probe("blur-off")
wait_for(lambda: counters()[":PLANES"] == 1, "underlay re-engaging after blur")
machine.wait_until_succeeds(
    "test $(journalctl -u underlay-video -o cat | grep -c 'scanout=1') -ge 2", timeout=10)
machine.succeed("systemctl stop underlay-video")

machine.succeed(
    "systemd-run --unit=underlay-scaled --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
    f"{DMABUF_CLIENT} --format nv12 --colour 336699 "
    f"--buffer-size 256x192 --viewport-src 0,0,256x192 "
    f"--viewport-dst {WINDOW[0]}x{WINDOW[1]} --feedback")
machine.wait_until_succeeds(
    f"journalctl -u underlay-scaled -o cat | grep -qx 'mapped {WINDOW[0]}x{WINDOW[1]}'", timeout=20)
frame_settles("scaled NV12 fallback to GL")
assert counters()[":PLANES"] == 0, counters()
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
screenshot("underlay-scaled")
print(machine.succeed(
    f"python3 /run/ppm_diff.py /run/underlay-shot.ppm /run/underlay-scaled-shot.ppm {hb_csv}"))
machine.succeed("systemctl stop underlay-scaled")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
failed_before = log_count("commit failed")
async_before = log_count("refused an async page flip")
park()
probe("chrome-on")
frame_times()
frame_before = counters()[":FRAME"]
gl_before = render_stats()
started = time.monotonic()
start_video("audit-video", frames=60)
wait_for(lambda: counters()[":PLANES"] == 1, "the underlay engaging for the audit")
assert counters()[":UNDERLAY-PLAN"] in (1, 2), counters()
video_stopped("audit-video", 60)
streamed = time.monotonic() - started
gl_after = render_stats()
streamed_drawn = counters()[":DRAWN-PIXELS"]
streamed_frames = counters()[":FRAME"] - frame_before
video_frame_times("underlay video")
print(f"underlay video: {streamed_frames} frames in {streamed:.2f} s "
      f"({streamed_frames / streamed:.1f} frames/s) "
      f"gl_delta={gl_after[':GL-CALLS'] - gl_before[':GL-CALLS']} "
      f"quads_delta={gl_after[':QUADS'] - gl_before[':QUADS']} drawn_pixels={streamed_drawn} "
      f"plan={counters()[':UNDERLAY-PLAN']} plane_tests={counters()[':PLANE-TESTS']}", flush=True)
assert gl_after[":GL-CALLS"] == gl_before[":GL-CALLS"], (gl_before, gl_after)
assert gl_after[":QUADS"] == gl_before[":QUADS"], (gl_before, gl_after)
assert streamed_drawn == 0, streamed_drawn
assert counters()[":PLANE-TESTS"] <= 8, counters()

switch_planes("blur disengaging the underlay", "blur-on", 0)
switch_planes("blur re-engaging the underlay", "blur-off", 1)
assert counters()[":UNDERLAY-PLAN"] in (1, 2), counters()
plane_video = displayed("plane-video")

machine.succeed("cat > /run/capture-loop.sh <<'SH'\n"
    "#!/bin/sh\n"
    "i=0\n"
    "while [ $i -lt 20 ]; do\n"
    "  i=$((i + 1))\n"
    "  " + ENV + "WAYLAND_DISPLAY=check grim -t ppm - > /run/capture.ppm || exit 1\n"
    "done\n"
    "echo captured\n"
    "SH\nchmod +x /run/capture-loop.sh")
machine.succeed("systemd-run --unit=capture-loop --property=Type=exec /run/capture-loop.sh")
captured_in_gl = False
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    if counters()[":PLANES"] == 0:
        captured_in_gl = True
        break
    time.sleep(0.01)
machine.wait_until_succeeds(
    "journalctl -u capture-loop -o cat | grep -qx captured", timeout=120)
print(f"screencopies took the underlay out of the scanout: {captured_in_gl}", flush=True)
same_pixels("/run/capture.ppm", "/run/plane-video-display.ppm", HOME_BOX, "screencopy")
wait_for(lambda: counters()[":PLANES"] == 1, "the underlay returning after the screencopies")
displayed("capture-back")
same_pixels("/run/capture-back-display.ppm", "/run/plane-video-display.ppm", HOME_BOX,
            "after the screencopies")
burst = counters()
print(f"after the screencopy burst: deferrals={burst[':DEFERRALS']} "
      f"missed={burst[':MISSED']} planes={burst[':PLANES']}", flush=True)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
print(f"audit: {log_count('commit failed') - failed_before} failed commits, "
      f"{log_count('refused an async page flip') - async_before} refused async flips",
      flush=True)
assert log_count("commit failed") == failed_before, "a commit failed during the audit"
assert log_count("refused an async page flip") == async_before, \
    "an async flip was refused during the audit"
machine.succeed("systemctl stop audit-video")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-refuse-planes", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
probe("chrome-on")
frame_times()
frame_before = counters()[":FRAME"]
gl_before = render_stats()
started = time.monotonic()
start_video("gl-video", frames=60)
video_stopped("gl-video", 60)
streamed = time.monotonic() - started
gl_after = render_stats()
streamed_frames = counters()[":FRAME"] - frame_before
video_frame_times("GL video")
assert counters()[":PLANES"] == 0, counters()
print(f"GL video: {streamed_frames} frames in {streamed:.2f} s "
      f"({streamed_frames / streamed:.1f} frames/s) "
      f"gl_calls={(gl_after[':GL-CALLS'] - gl_before[':GL-CALLS']) / streamed_frames:.1f} per frame "
      f"quads={(gl_after[':QUADS'] - gl_before[':QUADS']) / streamed_frames:.1f} per frame "
      f"drawn_pixels={counters()[':DRAWN-PIXELS']}", flush=True)
gl_video = displayed("gl-video")
print(f"GL frame {gl_video} underlay frame {plane_video}", flush=True)
same_pixels("/run/gl-video-display.ppm", "/run/plane-video-display.ppm", HOME_BOX, "GL video")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
machine.succeed("systemctl stop gl-video")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-refuse-planes", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
probe("chrome-on")
start_video("refused-video", frames=60)
video_stopped("refused-video", 60)
frame_settles("the video composited in GL after the plan was refused")
refused = counters()
refused_frame = displayed("refused")
print(f"refused plans: refusals={refused[':PLANE-REFUSALS']} tests={refused[':PLANE-TESTS']} "
      f"drawn={refused[':DRAWN-PIXELS']} frame {refused_frame}", flush=True)
assert refused[":PLANES"] == 0, refused
assert refused[":PLANE-REFUSALS"] >= 1, refused
assert refused[":PLANE-TESTS"] <= 8, refused
same_pixels("/run/refused-display.ppm", "/run/plane-video-display.ppm", HOME_BOX, "refused plan")
assert refused[":DEFERRALS"] == 0, refused
print(f"refused plan missed {refused[':MISSED']} refreshes", flush=True)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
machine.succeed("systemctl stop refused-video")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-zpos-stride=1", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
probe("chrome-on")
start_video("fixed-video", frames=30)
wait_for(lambda: counters()[":PLANES"] == 1, "the fixed-order underlay plan")
fixed = counters()
print(f"fixed-order zpos: plan={fixed[':UNDERLAY-PLAN']} planes={fixed[':PLANES']}", flush=True)
assert fixed[":UNDERLAY-PLAN"] == 2, fixed
video_stopped("fixed-video", 30)
displayed("fixed-order")
same_pixels("/run/fixed-order-display.ppm", "/run/plane-video-display.ppm", HOME_BOX,
            "fixed-order plan")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
machine.succeed("systemctl stop fixed-video")
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-zpos-stride=-1", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {PLANES_PROBE}")
park()
probe("chrome-on")
start_video("descending-video", frames=30)
video_stopped("descending-video", 30)
frame_settles("the video composited in GL after the zpos order was refused")
descending = counters()
print(f"descending zpos: refusals={descending[':PLANE-REFUSALS']} "
      f"tests={descending[':PLANE-TESTS']} planes={descending[':PLANES']}", flush=True)
assert descending[":PLANES"] == 0, descending
assert descending[":PLANE-REFUSALS"] >= 1, descending
assert descending[":PLANE-TESTS"] <= 8, descending
displayed("descending")
same_pixels("/run/descending-display.ppm", "/run/plane-video-display.ppm", HOME_BOX,
            "descending zpos")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
machine.succeed("systemctl stop descending-video")
quit()


def turned_frame(degrees, command, options=(), baseline=False):
    machine.succeed("systemctl stop tomoe check-bus")
    socket = "main" if baseline else "check"
    unit = f"turn-{degrees}-{socket}"
    if baseline:
        start_main()
    else:
        start("--bare", *options, f"--drm_device {DEVICE}", backend="drm", env=DRM)
    cli(f"mount {PLANES_PROBE}", socket)
    machine.succeed(
        f"systemd-run --unit={unit}-park --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --move 20000,20000")
    machine.wait_until_succeeds(
        f"journalctl -u {unit}-park -o cat | grep -qx moved", timeout=60)
    cli("command chrome-on", socket)
    cli(f"command {command}", socket)
    machine.succeed(
        f"systemd-run --unit={unit}-client --property=Type=exec "
        "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --color 336699")
    machine.wait_until_succeeds(
        f"journalctl -u {unit}-client -o cat | grep -q '^mapped'", timeout=60)
    machine.sleep(1)
    shot = displayed(f"turn-{degrees}-{socket}-{bool(options)}")
    if baseline:
        stop_main()
    else:
        assert counters()[":PLANES"] == 0, counters()
        assert counters()[":UNDERLAY-PLAN"] == 0, counters()
        assert inspect()[":LAST-ERROR"] == "NIL", inspect()
        quit()
    return shot


if BASELINE:
    for degrees, command in ((90, "rotate-ninety"), (270, "rotate-two-seventy")):
        scanned = turned_frame(degrees, command)
        composited = turned_frame(degrees, command, ("--test-refuse-planes",))
        main = turned_frame(degrees, command, baseline=True)
        print(f"rotated {degrees}: planes {scanned} composited {composited} main {main}",
              flush=True)
        assert scanned == composited, (degrees, scanned, composited)
        assert composited == main, (degrees, composited, main)

    def baseline_rounded_frame():
        machine.succeed("systemctl stop tomoe check-bus")
        start_main()
        cli(f"mount {PLANES_PROBE}", "main")
        machine.succeed(
            "systemd-run --unit=main-park --property=Type=exec "
            "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
            f"{SCANOUT_CLIENT} --move 20000,20000")
        machine.wait_until_succeeds(
            "journalctl -u main-park -o cat | grep -qx moved", timeout=60)
        cli("command chrome-on", "main")
        start_video("main-video", frames=30)
        video_stopped("main-video", 30)
        machine.sleep(1)
        shot = displayed("main-rounded")
        stop_main()
        return shot

    try:
        main_rounded = baseline_rounded_frame()
        print(f"main rounded video frame {main_rounded} underlay frame {plane_video} "
              f"GL frame {gl_video}", flush=True)
        report_pixels("/run/main-rounded-display.ppm", "/run/plane-video-display.ppm",
                      HOME_BOX, "main's own frame for the rounded video")
    except Exception as error:
        print(f"main's rounded video frame is unavailable: {error}", flush=True)

fast_rounds, fast_reallocs, fast_elapsed = flap_feedback(0)
slow_rounds, slow_reallocs, slow_elapsed = flap_feedback(250)
assert fast_reallocs >= 4, (fast_rounds, fast_reallocs, fast_elapsed)
assert slow_reallocs <= 1, (slow_rounds, slow_reallocs, slow_elapsed)
print(f"flap: undebounced {fast_reallocs / fast_elapsed:.1f} reallocations/s, "
      f"debounced {slow_reallocs / slow_elapsed:.1f} reallocations/s", flush=True)

machine.succeed("systemctl stop tomoe check-bus")
