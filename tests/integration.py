import json

start()
state = inspect()
names = [text(plist(entry)[":NAME"]) for entry in items(state[":EXTENSIONS"])]
assert "wm" in names, f"shipped extensions not mounted: {names}"
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]

client("window", "--mode xdg --app-id check-window --title check --size 320x200")
version = json.loads(cli("msg version"))
assert version["version"], version
windows = json.loads(cli("msg windows"))
window = next(w for w in windows if w["app_id"] == "check-window")
usable = json.loads(cli("msg outputs"))[0]["usable"]
box = window["geometry"]
assert window["mapped"] and window["focused"], window
assert usable["x"] <= box["x"] and box["x"] + box["w"] <= usable["x"] + usable["w"], (box, usable)
assert usable["y"] <= box["y"] and box["y"] + box["h"] <= usable["y"] + usable["h"], (box, usable)
assert (box["w"], box["h"]) != (320, 200), f"the window kept its own size: {box}"
machine.succeed(f"journalctl -u window -o cat | grep -qx 'configured {box['w']}x{box['h']}'")

cli(f"mount {ZOOMER}")
status, output = machine.execute(ENV + "tomoe --socket check command zoomer zoom-scroll-in")
assert status == 1 and "tomoe event" in output, (status, output)
zoomer = next(plist(e) for e in items(inspect()[":EXTENSIONS"]) if text(plist(e)[":NAME"]) == "zoomer")
assert zoomer[":LAST-ERROR"] == "NIL", zoomer[":LAST-ERROR"]
cli("unmount zoomer")


def pixel(px, py):
    return [int(v) for v in machine.succeed(
        ENV + f"WAYLAND_DISPLAY=check grim -c -g '{px},{py} 1x1' -t ppm - | tail -c 3 | od -An -tu1"
    ).split()]


def shaded(color, factor):
    return all(abs(a - b * factor) <= 2 for a, b in zip(color, (0x33, 0x66, 0x99)))


screen = json.loads(cli("msg outputs"))[0]["geometry"]
x, y, w, h = box["x"], box["y"], box["w"], box["h"]
middle, corner = (x + w // 2, y + h // 2), (x + w // 8, y + h // 8)
cli(f"mount {SCREENSHOT_PROBE}")
cli("command screenshot-probe select")
assert all(shaded(pixel(*middle), 0.6) for _ in range(4)), "the screenshot overlay does not dim the output"
machine.succeed(
    ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --mode drag --size {screen['w']}x{screen['h']} "
    f"--drag {x + w // 4},{y + h // 4},{x + 3 * w // 4},{y + 3 * h // 4}"
)
assert all(shaded(pixel(*middle), 1) for _ in range(4)), "the screenshot selection is still dimmed"
assert shaded(pixel(*corner), 0.6), "outside the screenshot selection is not dimmed"

quit()
