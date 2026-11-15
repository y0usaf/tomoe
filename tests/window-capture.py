import statistics
import time

COLOURS = ["336699", "99cc66", "cc6666", "6699cc"]
BOUND = 0.6
FLOOR = 5.0
STALL_MS = 900
REPORT = "/run/capture/report"


def call(arguments, timeout=60):
    return machine.succeed(ENV + "tomoe --socket check " + arguments, timeout=timeout)


def record():
    reply = read(call("frames"))
    assert reply[1] == ":OK", show(reply)
    return plist(items(reply)[0])


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
    return frames / window, median, missed, peak


def window(unit, colour, rate=120):
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"tomoe-test-client --seconds 3600 --app-id capture-target --title c{colour} "
        f"--size 200x150 --color {colour} --commit-rate {rate} --repaint")
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx mapped", timeout=60)


def capture(unit, options):
    machine.succeed("mkdir -p /run/capture")
    machine.execute(f"systemctl stop {unit}", check_return=False)
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--property=StandardOutput=file:{REPORT} "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{CAPTURE_CLIENT} {options}")


def finished(unit, timeout=90):
    machine.wait_until_succeeds(f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'",
                                timeout=timeout)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", f"{unit} exited {status}: " + machine.succeed(
        f"journalctl -u {unit} -o cat | tail -20")
    return machine.succeed(f"cat {REPORT}").splitlines()


def parse(lines):
    targets, frames = {}, []
    for line in lines:
        parts = line.split()
        if parts[0] == "target":
            targets[int(parts[1])] = parts[5]
        elif parts[0] == "frame":
            frames.append({"target": int(parts[1]), "result": parts[3],
                           "latency": float(parts[4]), "pixel": int(parts[5], 16) & 0xFFFFFF})
    return targets, frames


def check_pixels(lines, count):
    targets, frames = parse(lines)
    ready = [frame for frame in frames if frame["result"] == "ready"]
    assert len(ready) >= count, f"{len(ready)} captured frames, wanted {count}: {lines[-5:]}"
    for frame in ready:
        title = targets[frame["target"]]
        assert title.startswith("c"), title
        assert frame["pixel"] == int(title[1:], 16), (title, frame)


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

capture("capture-dmabuf", "--windows 2 --frames 12 --delay-ms 5 --seconds 20 --dmabuf")
check_pixels(finished("capture-dmabuf"), 8)

quit()
start("--bare", env={"TOMOE_TEST_STALL_CAPTURE": "1",
                     "TOMOE_TEST_STALL_CAPTURE_MS": str(STALL_MS)})
cli(f"mount {CAPTURE_POLICY}")
window("stall-win", COLOURS[0])
time.sleep(0.5)
stall_control, _, _ = measure("stall-control")
capture("capture-stalled", "--windows 1 --frames 3 --seconds 20 --reuse-check")
time.sleep(0.5)
stalled, _, _ = measure("stalled-capture")
assert stalled >= max(FLOOR, BOUND * stall_control), (stall_control, stalled)
lines = finished("capture-stalled")
_, frames = parse(lines)
failed = [frame for frame in frames if frame["result"] == "failed"]
assert failed, f"no frame was dropped when its fence stalled: {lines}"
assert failed[0]["latency"] >= STALL_MS * 0.9, f"failed after {failed[0]['latency']:.0f} ms"
intact = [line.split()[1] for line in lines if line.startswith("intact ")]
assert intact == ["deadbeef"], f"the buffer was written after the frame failed: {intact}"

quit()
start("--bare", env={"TOMOE_TEST_STALL_CAPTURE": "1"})
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
