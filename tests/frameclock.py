import re
import time

FRAME = re.compile(r":FRAME (\d+)", re.IGNORECASE)
MISSED = re.compile(r":MISSED (\d+)", re.IGNORECASE)
COST = re.compile(r":COST-NS (-?\d+)", re.IGNORECASE)
KEYS = ("count", "mean", "p50", "p90", "p99", "max")
DRM = {
    "TOMOE_DEBUG": "1",
    "LIBSEAT_BACKEND": "noop",
    "WLR_NO_HARDWARE_CURSORS": "1",
    "TOMOE_TIMINGS": "1",
}
PHASES = (
    ("motion", "--motion-stream 1000 3000", "present-input"),
    ("commit", "--commit-stream 500 3000", "present-commit"),
)
MODES = (("late-latch", {}), ("immediate", {"TOMOE_FRAME_CLOCK": "0"}))


def ticks():
    fields = machine.succeed("cat /proc/$(systemctl show -P MainPID tomoe)/stat").split()
    return int(fields[13]) + int(fields[14])


def frames():
    return sum(int(value) for value in FRAME.findall(cli("frames")))


def missed():
    return sum(int(value) for value in MISSED.findall(cli("frames")))


def costs():
    return [int(value) for value in COST.findall(cli("frames"))]


def channels():
    reply = read(cli("timings"))
    assert reply[1] == ":OK", show(reply)
    result = {}
    for entry in items(plist(reply[2])[":CHANNELS"]):
        fields = plist(entry)
        result[text(fields[":NAME"])] = {
            key: int(fields.get(":" + key.upper(), "0")) for key in KEYS
        }
    return result


def output(name):
    for entry in items(inspect()[":OUTPUTS"]):
        fields = plist(entry)
        if text(fields[":NAME"]) == name:
            return fields
    return None


def wait_output(name):
    for _ in range(600):
        fields = output(name)
        if fields and text(fields[":WIDTH"]) != "0":
            return fields
        time.sleep(0.1)
    raise AssertionError(f"no output named {name}")


def settle(limit=60):
    previous = frames()
    for _ in range(limit):
        time.sleep(0.25)
        current = frames()
        if current == previous:
            return
        previous = current
    raise AssertionError("frames never settled")


def measure(channel, arguments):
    cli("timings reset")
    before, before_missed = frames(), missed()
    time.sleep(0.4)
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client " + arguments)
    time.sleep(0.4)
    data = channels()[channel]
    drawn, dropped = frames() - before, missed() - before_missed
    print(f"{channel}: p50 {data['p50'] / 1000:.2f} ms p99 {data['p99'] / 1000:.2f} ms "
          f"max {data['max'] / 1000:.2f} ms count {data['count']} over {drawn} frames, "
          f"missed {dropped}, cost {max(costs() or [0])} ns")
    assert data["count"] > 100, f"{channel} recorded {data['count']} samples"
    return dict(data, frames=drawn, missed=dropped)


def phases():
    return {channel: measure(channel, arguments) for _, arguments, channel in PHASES}


def idle():
    started, started_frames = ticks(), frames()
    machine.sleep(2)
    woke, drawn = ticks() - started, frames() - started_frames
    print(f"idle: {woke * 10000} us and {drawn} frames in 2 s")
    return woke, drawn


CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
driven = {}
for mode, extra in MODES:
    start("--bare", f"--drm_device /dev/dri/{CARD}", backend="drm", env=dict(DRM, **extra))
    cli(f"mount {FRAMECLOCK_POLICY}")
    fields = wait_output("Virtual-1")
    assert (text(fields[":WIDTH"]), text(fields[":HEIGHT"])) == ("1920", "1080"), fields
    state = inspect()
    assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
    settle()
    woke, drawn = idle()
    assert woke == 0, f"{mode} woke {woke} times while idle"
    assert drawn == 0, f"{mode} drew {drawn} frames while idle"
    result = phases()
    quit()
    driven[mode] = result

for _, _, channel in PHASES:
    late, immediate = driven["late-latch"][channel], driven["immediate"][channel]
    print(f"vkms {channel}: late-latch p50 {late['p50'] / 1000:.2f} ms versus "
          f"immediate p50 {immediate['p50'] / 1000:.2f} ms, "
          f"p99 {late['p99'] / 1000:.2f} versus {immediate['p99'] / 1000:.2f} ms")
    assert late["p50"] * 100 < immediate["p50"] * 88, \
        f"vkms {channel} latency did not drop: {late} versus {immediate}"
    assert late["missed"] <= immediate["missed"] + late["frames"] // 20, \
        f"vkms {channel} dropped more frames: {late['missed']} versus {immediate['missed']}"

fast = {}
for mode, extra in MODES:
    start(env=dict(TOMOE_TIMINGS="1", **extra))
    cli(f"mount {FRAMECLOCK_POLICY}")
    wait_output("HEADLESS-1")
    settle()
    woke, drawn = idle()
    assert woke == 0 and drawn == 0, f"240 Hz {mode} was busy while idle: {woke} {drawn}"
    fast[mode] = phases()
    quit()

for _, _, channel in PHASES:
    late, immediate = fast["late-latch"][channel], fast["immediate"][channel]
    print(f"240 Hz {channel}: late-latch p50 {late['p50'] / 1000:.2f} ms versus "
          f"immediate p50 {immediate['p50'] / 1000:.2f} ms, "
          f"p99 {late['p99'] / 1000:.2f} versus {immediate['p99'] / 1000:.2f} ms")
    assert late["p50"] * 100 < immediate["p50"] * 75, \
        f"240 Hz {channel} latency did not drop: {late} versus {immediate}"
