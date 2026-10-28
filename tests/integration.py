import collections
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
screen = json.loads(cli("msg outputs"))[0]["geometry"]


def row(y):
    values = [int(v) for v in machine.succeed(
        ENV + f"WAYLAND_DISPLAY=check grim -g '0,{y} {screen['w']}x1' -t ppm - | od -An -v -tu1"
    ).split()][-3 * screen["w"]:]
    return collections.Counter(zip(values[0::3], values[1::3], values[2::3]))


def drawn(color, seen):
    want = tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
    return any(all(abs(a - b) <= 2 for a, b in zip(want, pixel)) for pixel in seen)


def dialog(opener, closer, want, absent=()):
    cli(f"command commands {opener}")
    deadline = time.monotonic() + 15
    while True:
        seen = row(screen["y"] + screen["h"] // 2)
        wrong = [c for c in want if not drawn(c, seen)] + [c for c in absent if drawn(c, seen)]
        if not wrong or time.monotonic() > deadline:
            break
        time.sleep(0.5)
    cli(f"event '(:type :key :owner \"commands\" :command \"{closer}\" :state :pressed)'")
    return wrong and (wrong, ["#%02x%02x%02x %d" % (*c, n) for c, n in seen.most_common(6)])


today = ["#1c1c21", "#7aa1f7"]
assert not (m := dialog("hotkeys", "hotkeys-dismiss", today)), f"the hotkey sheet lost its own colours: {m}"
assert not (m := dialog("quit", "exit-cancel", ["#1c1c21", "#bf2e38"])), f"the exit confirmation lost its own colours: {m}"
cli(f"mount {THEME_PROBE}")
assert not (m := dialog("hotkeys", "hotkeys-dismiss", ["#203040", "#c06030"], ["#1c1c21"])), f"the hotkey sheet ignores a published :theme: {m}"
assert not (m := dialog("quit", "exit-cancel", ["#203040", "#30a050"], ["#bf2e38"])), f"the exit confirmation ignores a published :theme: {m}"
cli("unmount theme-probe")
assert not (m := dialog("hotkeys", "hotkeys-dismiss", today, ["#203040", "#c06030"])), f"unmounting the :theme publisher left the hotkey sheet recoloured: {m}"


def pixel(px, py):
    return [int(v) for v in machine.succeed(
        ENV + f"WAYLAND_DISPLAY=check grim -c -g '{px},{py} 1x1' -t ppm - | tail -c 3 | od -An -tu1"
    ).split()]


def shaded(color, factor):
    return all(abs(a - b * factor) <= 2 for a, b in zip(color, (0x33, 0x66, 0x99)))


cli(f"mount {BLUR_PROBE}")
time.sleep(1)
frosted = [pixel(x, 250)[0] for x in range(150, 158)]
sharp = [pixel(x, 250)[0] for x in range(350, 358)]
assert max(frosted) - min(frosted) < 40 and 80 < sum(frosted) / 8 < 175, f"a shell surface with :blur t does not blur the stripes behind it: {frosted}"
assert min(sharp) < 20 and max(sharp) > 235, f"the stripes beside the blurred surface are not sharp: {sharp}"
cli("unmount blur-probe")

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

client("flood", "--mode xdg --app-id check-flood --commit-rate 1000")
cli(f"mount {OFFSCREEN_PROBE}")
flood = next(w for w in json.loads(cli("msg windows")) if w["app_id"] == "check-flood")
assert flood["geometry"]["x"] == 20000, f"the probe did not move the window off screen: {flood}"
time.sleep(20)
cli("unmount offscreen-probe")
time.sleep(3)
windows = {w["app_id"]: w for w in json.loads(cli("msg windows"))}
assert "check-flood" in windows and machine.execute("systemctl is-active flood")[0] == 0, (
    "tomoe dropped a client that drew off screen without waiting for frame callbacks:\n"
    + machine.succeed("journalctl -u flood -o cat | tail -3")
    + show(inspect()[":NATIVE-ERRORS"])
)
back = windows["check-flood"]["geometry"]
assert usable["x"] <= back["x"] < usable["x"] + usable["w"], f"the window did not come back on screen: {back}"
machine.succeed("systemctl stop flood")

quit()
