import statistics
import time

COLOURS = ["336699", "99cc66", "cc6666", "6699cc"]
BOUND = 0.6
FLOOR = 5.0
STALL_MS = 900
REPORT = "/run/capture/report"
CHECK = "/run/capture-check.py"
SHOT = "/run/capture/grim.png"

REPORT_CHECK = """cat > /run/capture-check.py <<'CAPTURECHECK'
targets, frames, ended = {}, [], []
report = open("/run/capture/report").read().splitlines()
for line in report:
    parts = line.split()
    if not parts:
        continue
    if parts[0] == "target" and len(parts) >= 5:
        targets[int(parts[1])] = parts[4]
    elif parts[0] == "frame" and len(parts) >= 6:
        frames.append((int(parts[1]), parts[3], float(parts[4]),
                       int(parts[5], 16) & 0xFFFFFF))
    elif parts[0] == "end":
        ended = parts[1:]
ready = [frame for frame in frames if frame[1] == "ready"]
failed = [frame for frame in frames if frame[1] == "failed"]
wrong = [frame for frame in ready if targets.get(frame[0]) != "c%06x" % frame[3]]
print("ready", len(ready))
print("failed", len(failed))
print("wrong", len(wrong))
print("targets", *["%d:%s" % (key, value) for key, value in sorted(targets.items())])
print("first", *["%d:%s:%06x" % (frame[0], frame[1], frame[3]) for frame in ready[:4]])
print("ready-latency", "%.3f" % (ready[0][2] if ready else 0.0))
print("end", *ended)
print("lines", len(report))
print("head", report[0] if report else "")
print("tail", report[-1] if report else "")
CAPTURECHECK"""


def call(arguments, timeout=60):
    return machine.succeed(ENV + "tomoe --socket check " + arguments, timeout=timeout)


def record():
    reply = read(call("frames"))
    assert reply[1] == ":OK", show(reply)
    return plist(items(items(reply)[2])[0])


def measure(label, window=2.0):
    before = record()
    time.sleep(window)
    after = record()
    frames = int(after[":FRAME"]) - int(before[":FRAME"])
    us = [int(value) for value in items(after[":FRAME-US"])]
    median = statistics.median(us) if us else 0.0
    peak = max(us) if us else 0
    missed = int(after[":MISSED"]) - int(before[":MISSED"])
    print(f"{label} {frames / window:.1f} fps median-cost {median:.0f} us peak {peak} us"
          f" missed {missed}", flush=True)
    return frames / window, median, missed


def window(unit, colour, rate=120):
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"tomoe-test-client --seconds 3600 --app-id capture-target --title c{colour} "
        f"--size 200x150 --color {colour} --commit-rate {rate} --repaint")
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx mapped", timeout=60)


def capture(unit, options):
    machine.execute(f"systemctl stop {unit}", check_return=False)
    machine.succeed(f"mkdir -p /run/capture; rm -f {REPORT} {REPORT}.err")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --property=RemainAfterExit=yes "
        f"--property=StandardOutput=file:{REPORT} "
        f"--property=StandardError=file:{REPORT}.err "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{CAPTURE_CLIENT} {options}")


def finished(unit, timeout=90):
    machine.wait_until_succeeds(f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'",
                                timeout=timeout)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", f"{unit} exited {status}: " + machine.execute(
        f"cat {REPORT}.err; head -n 4 {REPORT}")[1]
    counts = {}
    for line in machine.succeed(f"python3 {CHECK}").splitlines():
        parts = line.split()
        if parts:
            counts[parts[0]] = parts[1:]
    print(f"{unit} {counts}", flush=True)
    return counts


def check_pixels(counts, count, windows=True):
    ready = int(counts["ready"][0])
    assert ready >= count, f"{ready} captured frames, wanted {count}: {counts}"
    if windows:
        assert counts["wrong"] == ["0"], f"{counts['wrong'][0]} frames do not show their window"


machine.wait_for_unit("multi-user.target")
machine.succeed(REPORT_CHECK)
start("--bare")
cli(f"mount {CAPTURE_POLICY}")
for index, colour in enumerate(COLOURS):
    window(f"win-{index}", colour)
time.sleep(0.5)
control, control_median, _ = measure("control")
assert control >= FLOOR, f"the control scene runs at {control:.1f} fps"

capture("capture-shm", "--windows 4 --frames 100000 --delay-ms 50 --seconds 12")
time.sleep(0.5)
capturing, capturing_median, _ = measure("capturing")
assert capturing >= BOUND * control, f"{capturing:.1f} fps while capturing, control {control:.1f}"
assert capturing >= FLOOR, capturing
assert capturing_median <= 1.6 * control_median, (control_median, capturing_median)
check_pixels(finished("capture-shm"), 40)

if machine.execute("test -e /dev/udmabuf")[0] == 0:
    capture("capture-dmabuf", "--windows 2 --frames 12 --delay-ms 5 --seconds 20 --dmabuf")
    check_pixels(finished("capture-dmabuf"), 8)
else:
    print("no /dev/udmabuf: skipped the dma-buf capture phase", flush=True)

capture("capture-output", "--output --frames 12 --delay-ms 5 --seconds 20")
check_pixels(finished("capture-output"), 8, windows=False)

machine.succeed(f"{ENV}WAYLAND_DISPLAY=check timeout 60 grim {SHOT}")
machine.succeed(f"test -s {SHOT}")

quit()
start("--bare", "--test-stall-capture", "--test-stall-capture-ms=%d" % STALL_MS)
cli(f"mount {CAPTURE_POLICY}")
window("stall-win", COLOURS[0])
time.sleep(0.5)
stall_control, _, _ = measure("stall-control")
capture("capture-stalled", "--windows 1 --frames 3 --seconds 20")
time.sleep(0.5)
stalled, _, _ = measure("stalled-capture")
assert stalled >= max(FLOOR, BOUND * stall_control), (stall_control, stalled)
counts = finished("capture-stalled")
assert counts["failed"] == ["0"], f"a frame whose fence stalled was dropped: {counts}"
check_pixels(counts, 3)
latency = float(counts["ready-latency"][0])
assert latency >= STALL_MS * 0.9, f"ready after {latency:.0f} ms, before its fence signalled"

quit()
start("--bare", "--test-stall-capture")
cli(f"mount {CAPTURE_POLICY}")
window("hang-win", COLOURS[0])
time.sleep(0.5)
hang_control, _, _ = measure("hung-control")
capture("capture-hung", "--windows 1 --frames 1 --seconds 30")
time.sleep(0.5)
hung, _, _ = measure("hung-capture")
assert hung >= max(FLOOR, BOUND * hang_control), (hang_control, hung)
machine.execute("systemctl kill -s KILL capture-hung", check_return=False)
time.sleep(2)
after, _, _ = measure("hung-after-kill")
assert after >= max(FLOOR, BOUND * hang_control), f"{after:.1f} fps after the client died, control {hang_control:.1f}"
assert machine.execute(ENV + "tomoe --socket check inspect")[0] == 0

quit()
start("--bare", "--test-stall-capture", "--test-capture-watch-fail")
cli(f"mount {CAPTURE_POLICY}")
window("watch-win", COLOURS[0])
time.sleep(0.5)
watch_control, _, _ = measure("unwatched-control")
capture("capture-nowatch", "--windows 1 --frames 1 --seconds 8")
time.sleep(0.5)
nowatch, _, _ = measure("unwatched-capture")
assert nowatch >= max(FLOOR, BOUND * watch_control), (watch_control, nowatch)
assert "polling it from a timer" in machine.succeed("journalctl -u tomoe -o cat"), \
    "the capture fence was never polled from a timer"
counts = finished("capture-nowatch")
assert counts["end"] == ["1", "0", "0"], f"a frame reported before its fence signalled: {counts}"
after_watch, _, _ = measure("unwatched-after-client")
assert after_watch >= max(FLOOR, BOUND * watch_control), (watch_control, after_watch)
assert machine.execute(ENV + "tomoe --socket check inspect")[0] == 0
quit()
