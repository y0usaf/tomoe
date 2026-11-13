def start_binary(binary, unit, socket, bus=False):
    machine.wait_for_unit("multi-user.target")
    if bus:
        machine.execute(
            "systemd-run --unit=check-bus --property=Type=exec "
            f"dbus-daemon --session --nofork --address={BUS}")
        machine.wait_for_file("/run/check-bus")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={BUS} "
        "--setenv=TOMOE_TIMINGS=1 --setenv=HOME=/root "
        "--setenv=PATH=/run/current-system/sw/bin "
        f"--setenv=MESA_LOADER_DRIVER_OVERRIDE=zink --setenv={THREADS} "
        "--setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        f"{binary} --socket {socket} --backend headless --no-watch")
    for _ in range(600):
        if machine.execute(ENV + f"{binary} --socket {socket} inspect")[0] == 0:
            return
        assert machine.execute(
            f"systemctl show -P SubState {unit} | grep -qx running")[0] == 0, (
                "tomoe stopped while starting:\n"
                + machine.succeed(f"journalctl -u {unit} -o cat | tail -30"))
        time.sleep(0.1)
    raise AssertionError("tomoe did not answer inspect within 60 s")

def stop_binary(binary, unit, socket):
    machine.succeed(ENV + f"{binary} --socket {socket} quit")
    machine.wait_until_succeeds(
        f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'", timeout=60)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", (
        f"{unit} exited {status}: " + machine.succeed(f"journalctl -u {unit} -o cat | tail -40"))

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

def extensions(socket="check"):
    return {text(plist(entry)[":NAME"]): (int(plist(entry)[":DISPATCHES"]),
                                          int(plist(entry)[":FAILURES"]))
            for entry in items(inspect(socket)[":EXTENSIONS"])}

def measure(label, socket="check", unit="tomoe", gate=True):
    before = extensions(socket)
    probe = inspect(socket)
    invocations = int(probe[":INVOCATIONS"]) if ":INVOCATIONS" in probe else None
    cli("timings reset", socket)
    before_ticks, before_frames, before_consed = ticks(unit), frames(socket), memory(socket)[0]
    result = machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} tomoe-timing-client --count " + COUNT)
    busy_ticks, busy_frames = ticks(unit) - before_ticks, frames(socket) - before_frames
    consed, gcs = memory(socket)
    consed = consed - before_consed
    reply = read(cli("timings", socket))
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
    after = extensions(socket)
    ran = {name: after[name][0] - before.get(name, (0, 0))[0] for name in after}
    used = sum(ran.values())
    invoked = None if invocations is None else int(inspect(socket)[":INVOCATIONS"]) - invocations
    events = int(COUNT) * 4
    for key in ("pointer", "key", "drag", "frame-total", "commit-frame"):
        assert channels[key]["count"] > 0, f"{label}: {key} recorded nothing: {channels}"
    assert channels["frame-total"]["p50"] > 0, channels["frame-total"]
    assert all(after[name][1] == 0 for name in after), f"{label}: an extension failed"
    if gate:
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
    print(f"{label}: {used} Lisp runs and "
          + ("no invocation counter" if invoked is None else f"{invoked} invocations")
          + f" for {events} events")
    print(f"{'path':<14}{'count':>7}{'mean':>10}{'p50':>10}{'p90':>10}{'p99':>10}{'max':>10}"
          f"{'bytes':>12}   (us)")
    for name, fields in channels.items():
        print(f"{name:<14}{fields['count']:>7}" + "".join(
            f"{fields[key] / 1000:>10.1f}" for key in ("mean", "p50", "p90", "p99", "max"))
            + f"{consed_by_channel.get(name, 0):>12}")

    threshold, rows = stall_table(timings)
    print(f"{label}: {len(rows)} frames over {threshold / 1000000:.2f} ms; "
          f"longest {max([row[0] for row in rows], default=0) / 1000000:.2f} ms; "
          f"{len(items(timings[':GC-INTERVALS']))} collections logged")
    print(f"{'start us':>12}{'length ms':>11}{'gc':>8}{'gc ms':>8}  frame stall")
    for duration, start, overlapped in rows[:40]:
        print(f"{start % 1000000000:>12}{duration / 1000000:>11.2f}"
              f"{('yes' if overlapped else 'no'):>8}"
              f"{sum(b - a for a, b in overlapped) / 1000000:>8.2f}")
    return channels, used, invoked, ran

def measure_policy(label, policy, name, socket="check", unit="tomoe", gate=True):
    cli(f"mount {policy}", socket)
    try:
        return measure(label, socket, unit, gate)
    finally:
        cli(f"unmount {name}", socket)

runs = int(RUNS)
main = []
if BASELINE:
    start_binary(BASELINE, "tomoe-main", "main", bus=True)
    for run in range(runs):
        main.append(measure_policy(f"origin/main {run}", TIMING_POLICY, "timing-policy",
                                   "main", "tomoe-main", gate=False))
    stop_binary(BASELINE, "tomoe-main", "main")

start(env={"TOMOE_TIMINGS": "1"})
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
      f"{(idle_consed_end - idle_consed) / 2:.0f} bytes consed/s, "
      f"{idle_gcs_end - idle_gcs} collections in 2 s")

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
    if not runs_of:
        return "n/a"
    low, median, high = spread(runs_of, channel, field)
    return f"{low:.1f}..{median:.1f}..{high:.1f}"

print("p50 of each channel over " + str(runs) + " runs (us, min..median..max)")
for channel in ("pointer", "drag", "key", "transaction", "policy"):
    print(f"{channel:<12} origin/main {column(main, channel, 'p50'):<22}"
          f" no contract {column(plain, channel, 'p50'):<22}"
          f" contract {column(contract, channel, 'p50')}")
print("p99 of each channel over " + str(runs) + " runs (us, min..median..max)")
for channel in ("pointer", "drag", "key"):
    print(f"{channel:<12} origin/main {column(main, channel, 'p99'):<22}"
          f" no contract {column(plain, channel, 'p99'):<22}"
          f" contract {column(contract, channel, 'p99')}")
print("Lisp runs per " + str(int(COUNT) * 4) + " events")
before = sorted(run[1] for run in plain)
after = sorted(run[1] for run in contract)
print(f"{'runs':<12} origin/main "
      + (f"{min(run[1] for run in main)}..{max(run[1] for run in main)}" if main else "n/a")
      + f"   no contract {before[0]}..{before[-1]}   contract {after[0]}..{after[-1]}")
if main:
    print(f"{'runs':<12} origin/main median {sorted(run[1] for run in main)[len(main) // 2]}")

drag_after = spread(contract, "drag", "p50")[1]
drag_p99_before = spread(plain, "drag", "p99")[1]
drag_p99_after = spread(contract, "drag", "p99")[1]
print(f"drag p50 {spread(plain, 'drag', 'p50')[1]:.1f} -> {drag_after:.1f} us, "
      f"p99 {drag_p99_before:.1f} -> {drag_p99_after:.1f} us")
assert drag_after < spread(plain, "drag", "p50")[1], (
    "the native drag was not faster than the Lisp path: "
    + show([spread(plain, "drag", "p50"), spread(contract, "drag", "p50")]))
if main:
    assert drag_after < spread(main, "drag", "p50")[1], (
        "the native drag was not faster than origin/main: "
        + show([spread(main, "drag", "p50"), spread(contract, "drag", "p50")]))
    assert min(run[1] for run in plain) == min(run[1] for run in main), (
        "the no-contract path ran a different number of reducers than origin/main: "
        + show([[run[1] for run in main], [run[1] for run in plain]]))
assert min(run[1] for run in contract) < min(run[1] for run in plain), (
    "the contract published no fewer Lisp runs: "
    + show([[run[1] for run in plain], [run[1] for run in contract]]))
assert max(run[2] for run in contract) < min(run[2] for run in plain), (
    "the contract published no fewer Lisp invocations: "
    + show([[run[2] for run in plain], [run[2] for run in contract]]))

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
