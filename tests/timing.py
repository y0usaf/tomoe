start(env={"TOMOE_TIMINGS": "1"})
cli(f"mount {TIMING_POLICY}")

state = inspect()
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
for output in items(state[":OUTPUTS"]):
    fields = plist(output)
    print("output " + text(fields[":NAME"]) + " " + text(fields[":WIDTH"]) + "x" +
          text(fields[":HEIGHT"]) + " refresh " + text(fields.get(":REFRESH-MHZ", "?")))

def ticks():
    fields = machine.succeed("cat /proc/$(systemctl show -P MainPID tomoe)/stat").split()
    return int(fields[13]) + int(fields[14])

def frames():
    return sum(int(value) for value in re.findall(r":FRAME (\d+)", cli("frames"), re.IGNORECASE))

idle_start, idle_frames = ticks(), frames()
machine.sleep(2)
idle_ticks, idle_frames = ticks() - idle_start, frames() - idle_frames

before_ticks, before_frames = ticks(), frames()
result = machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client --count 200")
busy_ticks, busy_frames = ticks() - before_ticks, frames() - before_frames
print(f"cpu: {busy_ticks * 10000} us over {busy_frames} frames and 800 events: "
      f"{busy_ticks * 10000 / max(busy_frames, 1):.0f} us/frame, "
      f"{busy_ticks * 10000 / 800:.1f} us/event; idle {idle_ticks * 10000} us "
      f"over {idle_frames} frames")
for line in result.splitlines():
    if line.startswith("external:"):
        print(line)

reply = read(cli("timings"))
assert reply[1] == ":OK", show(reply)
timings = plist(reply[2])
assert timings[":ENABLED"] == "T", show(timings)
channels = {}
for entry in items(timings[":CHANNELS"]):
    fields = plist(entry)
    channels[text(fields[":NAME"])] = {
        key: int(fields[":" + key.upper()]) for key in
        ("count", "mean", "p50", "p90", "p99", "max")
    }
print(f"{'path':<14}{'count':>7}{'mean':>10}{'p50':>10}{'p90':>10}{'p99':>10}{'max':>10}   (µs)")
for name, fields in channels.items():
    print(f"{name:<14}{fields['count']:>7}" + "".join(
        f"{fields[key] / 1000:>10.1f}" for key in ("mean", "p50", "p90", "p99", "max")))

for name in ("pointer", "key", "drag", "frame-total", "commit-frame"):
    assert channels[name]["count"] > 0, f"{name} recorded nothing: {channels}"
assert channels["frame-total"]["p50"] > 0, channels["frame-total"]

machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -t ppm /run/timing.ppm")
print("screenshot: " + machine.succeed("sha256sum /run/timing.ppm").split()[0])

quit()
