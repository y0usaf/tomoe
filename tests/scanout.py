"""Direct scanout engages for an unobstructed full screen window and drops out
when UI paints over it.

The compositor runs on a headless output, so this measures the eligibility
decision rather than a plane commit, and it needs no GPU. A dma-buf client
stands in for a full screen game; the probe extension paints shell surfaces
above it, beside it or below it on demand, and a layer client stands in for a
menu.
"""

import collections

WINDOW = (0x33, 0x66, 0x99)
BAR = (0xFF, 0x88, 0x00)
NOTE = (0xFF, 0xFF, 0xFF)
MENU = (0xCC, 0x33, 0x33)
OUTPUT = "HEADLESS-1"
ENGAGED = "direct scanout engaged"
DISENGAGED = "direct scanout disengaged"


def journal():
    return machine.succeed("journalctl -u tomoe -o cat")


def frames():
    reply = read(cli("frames"))
    assert reply[1] == ":OK", show(reply)
    return items(reply[2])


def scanout(name=OUTPUT):
    for entry in frames():
        entry = plist(entry)
        if text(entry[":NAME"]) == name:
            return text(entry[":SCANOUT"]).lower()
    return None


def probe(command):
    cli(f"command {command}")


def wait_scanout(want, timeout=10):
    seen = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen = scanout()
        if seen == want:
            return
        time.sleep(0.1)
    raise AssertionError(f"direct scanout reported {seen}, wanted {want}")


def row(y, x=0, width=1280):
    values = [int(v) for v in machine.succeed(
        ENV + f"WAYLAND_DISPLAY=check grim -g '{x},{y} {width}x1' -t ppm - | od -An -v -tu1"
    ).split()][-3 * width:]
    return collections.Counter(zip(values[0::3], values[1::3], values[2::3]))


def has(seen, want, slack=2):
    return any(all(abs(a - b) <= slack for a, b in zip(pixel, want)) for pixel in seen)


def screenshot():
    machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -t ppm - > /tmp/scanout.ppm")
    return machine.succeed("sha256sum /tmp/scanout.ppm").split()[0]


observations = []


def observe(stage, want, window=None, extra=None, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and scanout() != want:
        time.sleep(0.1)
    time.sleep(0.2)
    seen = scanout()
    shot = screenshot()
    row_of = {'window': window, 'extra': extra}
    rows = {name: row(*box) for name, box in row_of.items() if box}
    observations.append((stage, seen, want, shot, rows))
    entry = plist(frames()[0])
    print(f"{stage}: scanout={seen} wanted={want} frame={text(entry[':FRAME'])} {shot}")


start("--bare", env={"TOMOE_DEBUG": "1"})
cli(f"mount {SCANOUT_PROBE}")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()

machine.succeed(
    "systemd-run --unit=scanout-client --property=Type=exec "
    "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
    f"{SCANOUT_CLIENT} --color 336699"
)
machine.wait_until_succeeds("journalctl -u scanout-client -o cat | grep -q '^mapped'", timeout=60)
machine.wait_until_succeeds("journalctl -u scanout-client -o cat | grep -qx 'mapped 1280x720'", timeout=30)
machine.wait_until_succeeds("journalctl -u scanout-client -o cat | grep -qx hidden", timeout=30)

observe("idle", "t", window=(700, 640, 40))

probe("empty-on")
observe("empty-on", "t", window=(700, 640, 40))

probe("bar-on")
observe("bar-on", "nil", window=(700, 640, 40), extra=(150, 100, 40))

probe("bar-off")
observe("bar-off", "t", window=(700, 640, 40))

probe("bar-bottom-on")
observe("bar-bottom-on", "t", window=(700, 640, 40))

probe("bar-bottom-off")
observe("bar-bottom-off", "t", window=(700, 640, 40))

probe("note-on")
observe("note-on", "nil", extra=(60, 1150, 60))

probe("note-off")
observe("note-off", "t")

probe("blank-on")
observe("blank-on", "t", window=(700, 640, 40))

probe("blank-off")
observe("blank-off", "t")

probe("away-on")
observe("away-on", "t", window=(700, 640, 40))

probe("away-off")
observe("away-off", "t")

probe("trail-on")
observe("trail-on", "t", window=(700, 640, 40))

machine.succeed(
    "systemd-run --unit=scanout-wiggle --property=Type=exec "
    "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
    f"{SCANOUT_CLIENT} --pointer 900,400,2500"
)
wait_scanout("nil", timeout=5)
observations.append(("trail-moved", "nil", "nil", None, {}))
print("trail-moved: scanout=nil wanted=nil")
machine.wait_until_succeeds("journalctl -u scanout-wiggle -o cat | grep -qx waggled", timeout=15)
observe("trail-still", "t", window=(700, 640, 40))

probe("trail-off")
observe("trail-off", "t")

machine.succeed(
    "systemd-run --unit=scanout-menu --property=Type=exec "
    "--setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check "
    "tomoe-test-client --mode layer --layer top --anchor top,left --size 200x120 "
    "--color cc3333 --seconds 600"
)
machine.wait_until_succeeds("journalctl -u scanout-menu -o cat | grep -qx mapped", timeout=60)
observe("menu", "nil", extra=(60, 20, 40))

machine.succeed("systemctl stop scanout-menu")
observe("menu-closed", "t", window=(700, 640, 40))

print("observed scanout:")
for stage, seen, want, sha, rows in observations:
    print(f"  {stage} {seen} wanted {want} {sha}")
log = journal()
assert ENGAGED in log, "the compositor never logged direct scanout engaging"
assert DISENGAGED in log, "the compositor never logged direct scanout disengaging"
for stage, seen, want, sha, rows in observations:
    assert seen == want, f"{stage}: direct scanout was {seen}, wanted {want}"
wanted_rows = {"bar-on": ("extra", BAR), "note-on": ("extra", NOTE), "menu": ("extra", MENU)}
for stage, seen, want, sha, rows in observations:
    if "window" in rows:
        assert has(rows["window"], WINDOW), f"{stage}: the full screen window is not on screen: {rows['window']}"
    if stage in wanted_rows:
        name, colour = wanted_rows[stage]
        assert has(rows[name], colour), f"{stage}: {colour} is not on screen: {rows[name]}"
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
quit()
