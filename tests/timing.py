def ticks(unit="tomoe"):
    fields = machine.succeed(f"cat /proc/$(systemctl show -P MainPID {unit})/stat").split()
    return int(fields[13]) + int(fields[14])

def frames(socket="check"):
    return sum(int(value) for value in re.findall(r":FRAME (\d+)", cli("frames", socket),
                                                  re.IGNORECASE))

def memory(socket="check"):
    reply = read(cli("memory", socket))
    assert reply[1] == ":OK", show(reply)
    fields = plist(reply[2])
    if ":LISP" not in fields:
        return int(fields[":BYTES-CONSED"]), int(fields[":GC-COUNT"])
    lisp = plist(fields[":LISP"])
    return int(lisp[":ARENA-HIGH-WATER"]), int(lisp[":RECLAIM-COUNT"])

def stall_table(timings):
    threshold = int(timings[":THRESHOLD"])
    rows = []
    for entry in items(timings[":STALLS"]):
        start, duration = int(entry[0]), int(entry[1])
        end = start + duration
        overlapped = []
        rows.append((duration, start, overlapped))
    return threshold, rows

def extensions(socket="check"):
    return {text(plist(entry)[":NAME"]): (int(plist(entry)[":DISPATCHES"]),
                                          int(plist(entry)[":FAILURES"]))
            for entry in items(inspect(socket)[":EXTENSIONS"])}

def measure(label):
    before = extensions()
    probe = inspect()
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
    after = extensions()
    ran = {name: after[name][0] - before.get(name, (0, 0))[0] for name in after}
    used = sum(ran.values())
    events = int(COUNT) * 4
    for key in ("pointer", "key", "drag", "frame-total", "commit-frame"):
        assert channels[key]["count"] > 0, f"{label}: {key} recorded nothing: {channels}"
    assert channels["frame-total"]["p50"] > 0, channels["frame-total"]
    assert all(after[name][1] == 0 for name in after), f"{label}: an extension failed"
    print(f"{label}: gate {show(probe.get(':MOTION-PLAN', []))}")
    for line in result.splitlines():
        if line.startswith("external:"):
            print(f"{label}: " + line)
    print(f"{label}: {busy_ticks * 10000} us of cpu over {busy_frames} frames and "
          f"{events} events: {busy_ticks * 10000 / max(busy_frames, 1):.0f} us/frame, "
          f"{busy_ticks * 10000 / events:.1f} us/event")
    print(f"{label}: {consed} bytes consed over {busy_frames} frames and {events} events: "
          f"{consed / max(busy_frames, 1):.0f} B/frame, {consed / events:.0f} B/event, "
          f"{gcs} collections")
    print(f"{label}: layout folded {timings.get(':LAYOUT-REBUILDS', '0')} times and reused "
          f"{timings.get(':LAYOUT-REUSES', '0')} times")
    print(f"{label}: {used} Lisp runs for {events} events")
    print(f"{'path':<14}{'count':>7}{'mean':>10}{'p50':>10}{'p90':>10}{'p99':>10}{'max':>10}"
          f"{'bytes':>12}   (us)")
    for name, fields in channels.items():
        print(f"{name:<14}{fields['count']:>7}" + "".join(
            f"{fields[key] / 1000:>10.1f}" for key in ("mean", "p50", "p90", "p99", "max"))
            + f"{consed_by_channel.get(name, 0):>12}")

    threshold, rows = stall_table(timings)
    print(f"{label}: {len(rows)} frames over {threshold / 1000000:.2f} ms; "
          f"longest {max([row[0] for row in rows], default=0) / 1000000:.2f} ms")
    print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
    for duration, start, overlapped in rows[:40]:
        print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
              f"{('yes' if overlapped else 'no'):>8}"
              f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")
    return channels, used, ran

def measure_policy(label, policy, name):
    cli(f"mount {policy}")
    try:
        return measure(label)
    finally:
        cli(f"unmount {name}")

runs = int(RUNS)
start("--test-timings")
cli(f"mount {TIMING_POLICY}")

state = inspect()
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
for output in items(state[":OUTPUTS"]):
    fields = plist(output)
    print("output " + text(fields[":NAME"]) + " " + text(fields[":WIDTH"]) + "x" +
          text(fields[":HEIGHT"]) + " refresh " + text(fields.get(":REFRESH-MHZ", "?")))

idle_start, idle_frames = ticks(), frames()
idle_consed, idle_gcs = memory()
machine.sleep(2)
idle_ticks, idle_frames = ticks() - idle_start, frames() - idle_frames
idle_consed_end, idle_gcs_end = memory()
print(f"idle: {idle_ticks * 10000} us, {idle_frames} frames, "
      f"Lisp arena high water {idle_consed_end} bytes, "
      f"{idle_gcs_end - idle_gcs} reclaims in 2 s")

cli("unmount timing-policy")
plain = []
contract = []
for run in range(runs):
    plain.append(measure_policy(f"no contract {run}", TIMING_POLICY, "timing-policy"))
    contract.append(measure_policy(f"contract {run}", TIMING_CONTRACT_POLICY, "timing-contract"))

def spread(runs_of, channel, field):
    values = sorted(run[0][channel][field] for run in runs_of)
    return values[0] / 1000, values[len(values) // 2] / 1000, values[-1] / 1000

def column(runs_of, channel, field):
    low, median, high = spread(runs_of, channel, field)
    return f"{low:.1f}..{median:.1f}..{high:.1f}"

print("p50 of each channel over " + str(runs) + " runs (us, min..median..max)")
for channel in ("pointer", "drag", "key", "transaction", "policy"):
    print(f"{channel:<12} no contract {column(plain, channel, 'p50'):<22}"
          f" contract {column(contract, channel, 'p50')}")
print("p99 of each channel over " + str(runs) + " runs (us, min..median..max)")
for channel in ("pointer", "drag", "key"):
    print(f"{channel:<12} no contract {column(plain, channel, 'p99'):<22}"
          f" contract {column(contract, channel, 'p99')}")
print("Lisp runs per " + str(int(COUNT) * 4) + " events")
before = sorted(run[1] for run in plain)
after = sorted(run[1] for run in contract)
print(f"{'runs':<12} no contract {before[0]}..{before[-1]}   contract {after[0]}..{after[-1]}")

drag_after = spread(contract, "drag", "p50")[1]
drag_p99_before = spread(plain, "drag", "p99")[1]
drag_p99_after = spread(contract, "drag", "p99")[1]
print(f"drag p50 {spread(plain, 'drag', 'p50')[1]:.1f} -> {drag_after:.1f} us, "
      f"p99 {drag_p99_before:.1f} -> {drag_p99_after:.1f} us")
assert drag_after < spread(plain, "drag", "p50")[1], (
    "the native drag was not faster than the Lisp path: "
    + str([spread(plain, "drag", "p50"), spread(contract, "drag", "p50")]))
assert min(run[1] for run in contract) < min(run[1] for run in plain), (
    "the contract published no fewer Lisp runs: "
    + str([[run[1] for run in plain], [run[1] for run in contract]]))

machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -t ppm /run/timing.ppm")
print("screenshot: " + machine.succeed("sha256sum /run/timing.ppm").split()[0])

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
      f"longest {max([row[0] for row in rows], default=0) / 1000000:.2f} ms")
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
      f"Lisp reclaimed")
print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
for duration, start, overlapped in rows[:20]:
    print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
          f"{('yes' if overlapped else 'no'):>8}"
          f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")

quit()
