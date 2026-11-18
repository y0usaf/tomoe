import json
import time


machine.execute("rm -f /run/input-frames.jsonl /run/input-subscribed /run/input-stop")

start("--bare")
machine.succeed(
    "systemd-run --unit=input-reader --property=Type=exec "
    "--setenv=PATH=/run/current-system/sw/bin --setenv=HOME=/root "
    f"{EVENT_READER} "
    f"{RUNTIME}/tomoe.check.sock /run/input-frames.jsonl /run/input-subscribed "
    "/run/input-stop retry")
machine.wait_for_file("/run/input-subscribed", timeout=30)

machine.succeed(
    "systemd-run --unit=input-target --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    "tomoe-test-client --seconds 3600 --mode xdg --app-id capture-target "
    "--title target --size 200x150 --color 336699")
machine.wait_until_succeeds("journalctl -u input-target -o cat | grep -qx mapped", timeout=60)
cli(f"mount {INPUT_POLICY}")


def windows():
    return json.loads(cli("msg windows"))


target = None
for _ in range(200):
    found = [window for window in windows()
             if window["app_id"] == "capture-target" and window["mapped"] and window["focused"]]
    if found:
        target = found[0]["id"]
        break
    time.sleep(0.05)
assert target, windows()
time.sleep(0.3)

state = inspect()
geometries = {int(plist(entry)[":ID"]): plist(entry)
              for entry in items(state.get(":WINDOW-GEOMETRY"))}
assert target in geometries, (target, state.get(":WINDOW-GEOMETRY"))
origin = (int(geometries[target][":X"]), int(geometries[target][":Y"]))
assert origin == (100, 100), origin

outputs = json.loads(cli("msg outputs"))
assert outputs and outputs[0]["name"] == "HEADLESS-1", outputs
output, screen = outputs[0]["name"], outputs[0]["geometry"]


def frames():
    return [json.loads(line) for line in
            machine.succeed("cat /run/input-frames.jsonl 2>/dev/null || true").splitlines()
            if line.strip()]


def inject(unit, *commands):
    machine.execute(f"systemctl stop {unit} 2>/dev/null")
    machine.execute(f"systemctl reset-failed {unit} 2>/dev/null")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{SCRIPT} --extent 1280x720 " + " ".join(f"'{command}'" for command in commands))
    machine.wait_until_succeeds(
        f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'", timeout=60)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", (status, machine.succeed(f"journalctl -u {unit} -o cat"))


def wait_for(predicate, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        current = frames()
        if predicate(current):
            return current
        time.sleep(0.05)
    raise AssertionError(f"input stream stayed at {len(frames())} frames: {frames()}")


inject("input-inject-1", "move 100 50", "sleep 250", "move 300 200", "sleep 250",
       "press 272", "sleep 150", "release 272", "sleep 150",
       "key 30 down", "sleep 150", "key 30 up", "sleep 150", "sync")
phase = wait_for(lambda current: sum(1 for frame in current if frame["event"] == "key") >= 2)
time.sleep(0.3)
phase = frames()

motions = [frame for frame in phase if frame["event"] == "pointer_motion"]
buttons = [frame for frame in phase if frame["event"] == "pointer_button"]
keys = [frame for frame in phase if frame["event"] == "key"]
assert len(motions) == 2, motions
assert (motions[0]["payload"]["x"], motions[0]["payload"]["y"]) == (100, 50), motions
assert (motions[-1]["payload"]["x"], motions[-1]["payload"]["y"]) == (300, 200), motions
assert len(buttons) == 2, buttons
assert [frame["payload"]["button"] for frame in buttons] == [272, 272], buttons
assert [frame["payload"]["state"] for frame in buttons] == ["pressed", "released"], buttons
assert all((frame["payload"]["x"], frame["payload"]["y"]) == (300, 200) for frame in buttons), buttons
assert len(keys) == 2, keys
assert [frame["payload"]["keycode"] for frame in keys] == [30, 30], keys
assert [frame["payload"]["state"] for frame in keys] == ["pressed", "released"], keys
assert [frame["payload"]["keysym"] for frame in keys] == ["a", "a"], keys
assert all(frame["payload"]["modifiers"] == [] for frame in keys), keys


def at(predicate):
    return next(index for index, frame in enumerate(phase) if predicate(frame))


first = phase.index(motions[0])
newest = at(lambda frame: frame["event"] == "pointer_motion"
            and (frame["payload"]["x"], frame["payload"]["y"]) == (300, 200))
press = at(lambda frame: frame["event"] == "pointer_button" and frame["payload"]["state"] == "pressed")
release = at(lambda frame: frame["event"] == "pointer_button" and frame["payload"]["state"] == "released")
keydown = at(lambda frame: frame["event"] == "key" and frame["payload"]["state"] == "pressed")
keyup = at(lambda frame: frame["event"] == "key" and frame["payload"]["state"] == "released")
assert first < newest < press < release < keydown < keyup, (
    first, newest, press, release, keydown, keyup)

for frame in phase:
    payload = frame["payload"]
    assert payload["window"] == target, (frame, target)
    assert isinstance(payload["time"], int) and payload["time"] > 0, frame
for frame in motions + buttons:
    payload = frame["payload"]
    assert payload["output"] == output, frame
    assert (payload["output_x"], payload["output_y"]) == (
        payload["x"] - screen["x"], payload["y"] - screen["y"]), frame
for frame in buttons:
    payload = frame["payload"]
    assert (payload["window_x"], payload["window_y"]) == (
        payload["x"] - origin[0], payload["y"] - origin[1]), (frame, origin)

stamps = [frame["payload"]["time"] for frame in motions]
assert stamps == sorted(stamps), stamps
uptime = float(machine.succeed("cut -d' ' -f1 /proc/uptime"))
assert all(abs(stamp / 1e6 - uptime) < 60 for stamp in stamps), (stamps, uptime)

baseline = len(frames())
cli(f"mount {SHORTCUT_POLICY}")
time.sleep(0.3)
inject("input-inject-2", "move 300 200", "sleep 200", "key 30 down", "sleep 150",
       "key 30 up", "sleep 150")
wait_for(lambda current: sum(1 for frame in current[baseline:] if frame["event"] == "key") >= 2)
time.sleep(0.3)
bound = [frame for frame in frames()[baseline:] if frame["event"] == "key"]
assert [frame["payload"]["state"] for frame in bound] == ["pressed", "released"], bound
assert all(frame["payload"]["keycode"] == 30 and frame["payload"]["keysym"] == "a"
           for frame in bound), bound
assert all(frame["payload"]["window"] == target for frame in bound), (bound, target)

machine.execute("touch /run/input-stop")
quit()
print("input-stream: coordinates, output and window offsets, monotonic times, coalesced motion, "
      "button and key order, and a consumed binding's key all reached the JSON stream")
