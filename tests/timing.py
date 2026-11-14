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

def memory():
    reply = read(cli("memory"))
    assert reply[1] == ":OK", show(reply)
    fields = plist(reply[2])
    return int(fields[":BYTES-CONSED"]), int(fields[":GC-COUNT"])

def stall_table(timings):
    threshold = int(timings[":THRESHOLD"])
    intervals = [(int(entry[0]), int(entry[1])) for entry in items(timings[":GC-INTERVALS"])]
    rows = []
    for entry in items(timings[":STALLS"]):
        start, duration = int(entry[0]), int(entry[1])
        end = start + duration
        overlapped = [(a, b) for a, b in intervals if a < end and b > start]
        rows.append((duration, start, overlapped))
    return threshold, rows

runs = int(RUNS)
idle_start, idle_frames = ticks(), frames()
idle_consed, idle_gcs = memory()
machine.sleep(2)
idle_ticks, idle_frames = ticks() - idle_start, frames() - idle_frames
idle_consed_end, idle_gcs_end = memory()
print(f"idle: {idle_ticks * 10000} us, {idle_frames} frames, "
      f"{(idle_consed_end - idle_consed) / 2:.0f} bytes consed/s, "
      f"{idle_gcs_end - idle_gcs} collections in 2 s")

for run in range(runs):
    cli("timings reset")
    before_ticks, before_frames, before_consed = ticks(), frames(), memory()[0]
    result = machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client --count " + COUNT)
    busy_ticks, busy_frames = ticks() - before_ticks, frames() - before_frames
    consed, gcs = memory()
    consed = consed - before_consed
    reply = read(cli("timings"))
    assert reply[1] == ":OK", show(reply)
    timings = plist(reply[2])
    assert timings[":ENABLED"] == "T", show(timings)
    channels = {}
    for entry in items(timings[":CHANNELS"]):
        fields = plist(entry)
        channels[text(fields[":NAME"])] = {
            key: int(fields.get(":" + key.upper(), "0")) for key in
            ("count", "mean", "p50", "p90", "p99", "max")
        }
    consed_by_channel = {text(plist(entry)[":NAME"]): int(plist(entry)[":CONSED"])
                         for entry in items(timings[":CONSED"])}
    events = int(COUNT) * 4
    for line in result.splitlines():
        if line.startswith("external:"):
            print(f"run {run}: " + line)
    print(f"run {run}: {busy_ticks * 10000} us of cpu over {busy_frames} frames and "
          f"{events} events: {busy_ticks * 10000 / max(busy_frames, 1):.0f} us/frame, "
          f"{busy_ticks * 10000 / events:.1f} us/event")
    print(f"run {run}: {consed} bytes consed over {busy_frames} frames and {events} events: "
          f"{consed / max(busy_frames, 1):.0f} B/frame, {consed / events:.0f} B/event, "
          f"{gcs} collections")
    print(f"run {run}: layout folded {timings.get(':LAYOUT-REBUILDS', '0')} times and reused "
          f"{timings.get(':LAYOUT-REUSES', '0')} times")
    print(f"{'path':<14}{'count':>7}{'mean':>10}{'p50':>10}{'p90':>10}{'p99':>10}{'max':>10}"
          f"{'bytes':>12}   (µs)")
    for name, fields in channels.items():
        print(f"{name:<14}{fields['count']:>7}" + "".join(
            f"{fields[key] / 1000:>10.1f}" for key in ("mean", "p50", "p90", "p99", "max"))
            + f"{consed_by_channel.get(name, 0):>12}")

    threshold, rows = stall_table(timings)
    print(f"run {run}: {len(rows)} frames over {threshold / 1000000:.2f} ms; "
          f"longest {max([row[0] for row in rows], default=0) / 1000000:.2f} ms; "
          f"{len(items(timings[':GC-INTERVALS']))} collections logged")
    print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
    for duration, start, overlapped in rows[:40]:
        print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
              f"{('yes' if overlapped else 'no'):>8}"
              f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")

    for name in ("pointer", "key", "drag", "frame-total", "commit-frame"):
        assert channels[name]["count"] > 0, f"{name} recorded nothing: {channels}"
    assert channels["frame-total"]["p50"] > 0, channels["frame-total"]


machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -t ppm /run/timing.ppm")
print("screenshot: " + machine.succeed("sha256sum /run/timing.ppm").split()[0])

cli("unmount timing-policy")
cli(f"mount {STALL_POLICY}")
time.sleep(1.5)
cli("timings reset")
machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client --count 60 --skip-commit")
machine.sleep(1)
reply = read(cli("timings"))
assert reply[1] == ":OK", show(reply)
timings = plist(reply[2])
threshold, rows = stall_table(timings)
frames_total = {text(plist(entry)[":NAME"]): plist(entry)
                for entry in items(timings[":CHANNELS"])}["frame-total"]
print(f"animated: {int(frames_total[':COUNT'])} frames with the clock and shader wallpaper; "
      f"p50 {int(frames_total[':P50']) / 1000:.1f} us, p99 {int(frames_total[':P99']) / 1000:.1f} us")
print(f"animated: {len(rows)} frames over {threshold / 1000000:.2f} ms; "
      f"longest {max([row[0] for row in rows], default=0) / 1000000:.2f} ms; "
      f"{len(items(timings[':GC-INTERVALS']))} collections logged")
phases = {text(plist(entry)[":NAME"]): plist(entry) for entry in items(timings[":CHANNELS"])}
print("animated frame work: " + ", ".join(
    f"{name} p50 {int(phases[name].get(':P50', '0')) / 1000:.1f} us"
    f" p99 {int(phases[name].get(':P99', '0')) / 1000:.1f} us"
    for name in ("frame-scene", "frame-draw", "frame-raster", "frame-submit", "frame-total",
                 "transaction")))
print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
for duration, start, overlapped in rows:
    print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
          f"{('yes' if overlapped else 'no'):>8}"
          f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")

cli(f"mount {CHURN_POLICY}")
time.sleep(1)
cli("timings reset")
machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-timing-client --count 40 --skip-commit")
machine.sleep(1)
reply = read(cli("timings"))
assert reply[1] == ":OK", show(reply)
timings = plist(reply[2])
threshold, rows = stall_table(timings)
tagged = len([row for row in rows if row[2]])
print(f"churn: {len(rows)} frames over {threshold / 1000000:.2f} ms, {tagged} of them while "
      f"a collection ran; {len(items(timings[':GC-INTERVALS']))} collections logged")
print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
for duration, start, overlapped in rows[:20]:
    print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
          f"{('yes' if overlapped else 'no'):>8}"
          f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")

quit()
