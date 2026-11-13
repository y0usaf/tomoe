FRAME = re.compile(r"(?<!-):FRAME (\d+)", re.IGNORECASE)
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
    ("load", "--motion-stream 1000 3000", "present-input"),
)
MODES = (("late-latch", {}), ("immediate", {"TOMOE_FRAME_CLOCK": "0"}))


def ticks():
    fields = machine.succeed("cat /proc/$(systemctl show -P MainPID tomoe)/stat").split()
    return int(fields[13]) + int(fields[14])


def switches():
    status = machine.succeed("cat /proc/$(systemctl show -P MainPID tomoe)/status")
    return tuple(int(field) for field in re.findall(r"^(?:non)?voluntary_ctxt_switches:\s+(\d+)",
                                                    status, re.MULTILINE))


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


def measure(name, channel, arguments):
    if name == "load":
        machine.succeed("systemd-run --unit=frameclock-load --property=Type=exec "
                        "/bin/sh -c 'while :; do :; done'")
    cli("timings reset")
    before, before_missed = frames(), missed()
    time.sleep(0.4)
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client " + arguments)
    time.sleep(0.4)
    if name == "load":
        machine.succeed("systemctl stop frameclock-load")
    all_channels = channels()
    data = all_channels[channel]
    drawn, dropped = frames() - before, missed() - before_missed
    print(f"{name} {channel}: p50 {data['p50'] / 1000:.2f} ms p99 {data['p99'] / 1000:.2f} ms "
          f"max {data['max'] / 1000:.2f} ms count {data['count']} over {drawn} frames, "
          f"missed {dropped}, cost {max(costs() or [0])} ns")
    print("  clock: " + cli("frames").strip())
    for probe in ("frame-wait", "frame-total", "latch-input", "latch-commit", "present-input",
                  "present-commit"):
        entry = all_channels[probe]
        print(f"  {probe}: count {entry['count']} p50 {entry['p50'] / 1000:.2f} "
              f"p99 {entry['p99'] / 1000:.2f} max {entry['max'] / 1000:.2f} us")
    assert data["count"] > 100, f"{channel} recorded {data['count']} samples"
    return dict(data, frames=drawn, missed=dropped)


def phases():
    return {name: measure(name, channel, arguments) for name, arguments, channel in PHASES}


def idle():
    started, started_frames, started_switches = ticks(), frames(), switches()
    machine.sleep(2)
    woke, drawn = ticks() - started, frames() - started_frames
    switched = sum(switches()) - sum(started_switches)
    print(f"idle: {woke * 10000} us, {switched} switches and {drawn} frames in 2 s")
    assert drawn == 0, f"drew {drawn} frames while idle"
    assert switched <= 8, f"woke {switched} times while idle"
    return woke, drawn


CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
driven = {}
for index, (mode, extra) in enumerate(MODES):
    if index:
        machine.succeed("systemctl stop tomoe")
    start("--bare", f"--drm_device /dev/dri/{CARD}", backend="drm", bus=not index,
          env=dict(DRM, **extra))
    cli(f"mount {FRAMECLOCK_POLICY}")
    fields = wait_output("Virtual-1")
    assert (text(fields[":WIDTH"]), text(fields[":HEIGHT"])) == ("1920", "1080"), fields
    state = inspect()
    assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
    settle()
    idle()
    result = phases()
    quit()
    driven[mode] = result

for name, _, channel in PHASES:
    late, immediate = driven["late-latch"][name], driven["immediate"][name]
    print(f"vkms {name}: late-latch p50 {late['p50'] / 1000:.2f} ms versus "
          f"immediate p50 {immediate['p50'] / 1000:.2f} ms, "
          f"p99 {late['p99'] / 1000:.2f} versus {immediate['p99'] / 1000:.2f} ms")
    assert late["p50"] * 100 < immediate["p50"] * 88, \
        f"vkms {name} latency did not drop: {late} versus {immediate}"
    assert late["missed"] <= immediate["missed"] + late["frames"] // 50, \
        f"vkms {name} dropped more frames: {late['missed']} versus {immediate['missed']}"
    assert late["missed"] * 20 <= late["frames"], \
        f"vkms {name} lost {late['missed']} of {late['frames']} frames"
