COUNTS = [int(value) for value in DRAW_COUNTS.split(",")]
GOLDEN = [value for value in DRAW_GOLDEN.split(",") if value]
BLUR_DIGESTS = [value for value in BLUR_GOLDEN.split(",") if value]
COLOURS = ["3c5a78", "7a4a2c", "2c7a4a", "6a2c7a", "7a7a2c", "2c6a7a",
           "7a2c4a", "4a7a2c", "2c4a7a", "5a3c6a"]


def shot(socket, limit=12, cursors=False):
    previous = None
    for _ in range(limit):
        digest = machine.succeed(
            ENV + f"WAYLAND_DISPLAY={socket} grim {'-c ' if cursors else ''}-t ppm /run/draw.ppm"
            " && sha256sum /run/draw.ppm"
        ).split()[0][:16]
        if digest == previous:
            return digest
        previous = digest
        time.sleep(0.4)
    return previous


def tiles(socket):
    found = []
    for row in range(3):
        for column in range(4):
            found.append(machine.succeed(
                ENV + f"WAYLAND_DISPLAY={socket} grim -g '{column * 320},{row * 240} 320x240' "
                "-t ppm /run/tile.ppm && sha256sum /run/tile.ppm").split()[0][:12])
    return found


def ticks(unit):
    fields = machine.succeed(f"cat /proc/$(systemctl show -P MainPID {unit})/stat").split()
    return int(fields[13]) + int(fields[14])


def switches(unit):
    text = machine.succeed(
        f"grep ctxt_switches /proc/$(systemctl show -P MainPID {unit})/status")
    return sum(int(line.split()[-1]) for line in text.splitlines())


def try_cli(arguments, socket="check", attempts=8):
    for _ in range(attempts):
        status, output = machine.execute(ENV + f"tomoe --socket {socket} " + arguments)
        if status == 0:
            return output
        time.sleep(0.25)
    raise AssertionError(f"{arguments} kept failing: {output}")


def frames(socket):
    return sum(int(value) for value in re.findall(r":FRAME (\d+)",
                                                  try_cli("frames", socket), re.IGNORECASE))


def counters(socket):
    text = try_cli("frames", socket)
    found = {}
    for name in ("gl-calls", "quads", "runs", "uploads", "upload-bytes", "blurs", "blurs-reused"):
        match = re.search(r":" + name + r" (\d+)", text, re.IGNORECASE)
        if match:
            found[name] = int(match.group(1))
    return found


def channel(socket, name):
    reply = read(try_cli("timings", socket))
    assert reply[1] == ":OK", show(reply)
    for entry in items(plist(reply[2])[":CHANNELS"]):
        fields = plist(entry)
        if text(fields[":NAME"]) == name:
            return {key: int(fields.get(":" + key.upper(), 0)) for key in
                    ("count", "mean", "p50", "p90", "p99", "max")}
    return {}


def alive(unit):
    state = machine.execute(f"systemctl show -P SubState {unit}")[1].strip()
    assert state == "running", f"{unit} is {state}:\n" + machine.execute(
        f"journalctl -u {unit} -o cat | tail -20")[1]


def spawn(socket, first, count, repaint=False, unit=None):
    for index in range(first, count):
        if unit:
            alive(unit)
        client(f"{socket}-{index}", f"--mode xdg --app-id draw-{index} --size 320x200 "
            f"--color {COLOURS[index % len(COLOURS)]}"
            + (" --commit-rate 60" + (" --repaint" if repaint else "") if index == 0 else ""),
            socket=socket)


def measure(unit, socket, seconds=3):
    try_cli("timings reset", socket)
    time.sleep(0.3)
    before_frames, before, before_ticks = frames(socket), counters(socket), ticks(unit)
    machine.sleep(seconds)
    drawn = max(frames(socket) - before_frames, 1)
    spent = ticks(unit) - before_ticks
    after = counters(socket)
    total = channel(socket, "frame-total")
    return {"frames": drawn, "samples": total.get("count", 0), "cpu": spent * 10 / drawn,
            "counted": "blurs" in before,
            "blurs": (after.get("blurs", 0) - before.get("blurs", 0)) / drawn,
            "reused": (after.get("blurs-reused", 0) - before.get("blurs-reused", 0)) / drawn,
            "p50": total.get("p50", 0) / 1e6, "p99": total.get("p99", 0) / 1e6,
            "max": total.get("max", 0) / 1e6}


def describe(cost):
    return (f"{cost['frames']} frames in 3 s, {cost['cpu']:.2f} ms process CPU/frame, "
            f"frame {cost['p50']:.2f}/{cost['p99']:.2f}/{cost['max']:.2f} ms p50/p99/max, "
            f"{cost['blurs']:.2f} blurs and {cost['reused']:.2f} reused blurs/frame")


digests = []
idle_ticks = idle_frames = idle_calls = 0
for count in COUNTS:
    shot_unit, shot_socket = f"tomoe-shot{count}", f"shot{count}"
    start(env={"TOMOE_TIMINGS": "1"}, unit=shot_unit, socket=shot_socket)
    cli(f"mount {DRAW_POLICY}", shot_socket)
    state = inspect(shot_socket)
    assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
    spawn(shot_socket, 0, count, unit=shot_unit)
    time.sleep(1.5)
    digests.append(shot(shot_socket))
    print(f"windows {count}: screenshot {digests[-1]} tiles {' '.join(tiles(shot_socket))}")
    if count == COUNTS[-1]:
        machine.succeed(f"systemctl stop {shot_socket}-0")
        time.sleep(2)
        try_cli("timings reset", shot_socket)
        idle_start, idle_calls = ticks(shot_unit), switches(shot_unit)
        machine.sleep(2)
        idle_ticks = ticks(shot_unit) - idle_start
        idle_calls = switches(shot_unit) - idle_calls
        idle_frames = channel(shot_socket, "frame-total").get("count", 0)
        print(f"idle: {idle_ticks * 10000} us, {idle_frames} frames and "
              f"{idle_calls} wakeups in 2 s")
        print(f"counters {counters(shot_socket)}")
    print("kernel: " + machine.execute("dmesg | tail -3")[1].strip())
    quit(shot_unit, shot_socket)

start(env={"TOMOE_TIMINGS": "1", "TOMOE_DEBUG_DAMAGE": "1"}, unit="tomoe-cost",
      socket="cost")
cli(f"mount {DRAW_POLICY}", "cost")
assert inspect("cost")[":LAST-ERROR"] == "NIL", inspect("cost")[":LAST-ERROR"]

costs = []
seen = 0
for count in COUNTS:
    spawn("cost", seen, count, unit="tomoe-cost")
    seen = count
    time.sleep(2.5)
    try_cli("timings reset", "cost")
    time.sleep(0.3)
    before_frames, before = frames("cost"), counters("cost")
    machine.sleep(3)
    drawn = max(frames("cost") - before_frames, 1)
    after = counters("cost")
    draw = channel("cost", "frame-draw")
    calls = after.get("gl-calls", 0) - before.get("gl-calls", 0)
    quads = after.get("quads", 0) - before.get("quads", 0)
    runs = after.get("runs", 0) - before.get("runs", 0)
    uploads = after.get("uploads", 0) - before.get("uploads", 0)
    costs.append((count, calls / drawn, quads / drawn, runs / drawn,
                  draw.get("p50", 0), draw.get("p99", 0), draw.get("max", 0)))
    print(f"windows {count}: {calls / drawn:.1f} gl calls/frame, "
          f"{quads / drawn:.1f} quads/frame, {runs / drawn:.1f} runs/frame, "
          f"{uploads / drawn:.3f} uploads/frame, "
          f"draw {draw.get('p50', 0) / 1000:.1f}/{draw.get('p99', 0) / 1000:.1f}/"
          f"{draw.get('max', 0) / 1000:.1f} us p50/p99/max over {draw.get('count', 0)} frames")
quit("tomoe-cost", "cost")

assert idle_ticks <= 1, f"idle cost {idle_ticks * 10000} us over 2 s"
assert idle_frames == 0, f"idle drew {idle_frames} frames"
assert all(count[1] > 0 for count in costs), costs
if GOLDEN:
    assert digests == GOLDEN, f"the composited bytes changed: {digests} != {GOLDEN}"

blur_digests = []
for count in COUNTS:
    shot_unit, shot_socket = f"tomoe-blurshot{count}", f"blurshot{count}"
    start(env={"TOMOE_TIMINGS": "1"}, unit=shot_unit, socket=shot_socket)
    cli(f"mount {BLUR_POLICY}", shot_socket)
    cli(f"mount {BLUR_BACKDROP}", shot_socket)
    state = inspect(shot_socket)
    assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
    spawn(shot_socket, 0, count, repaint=True, unit=shot_unit)
    time.sleep(1.5)
    blur_digests += [shot(shot_socket), shot(shot_socket, cursors=True)]
    print(f"blur windows {count}: screenshot {blur_digests[-2]} presented {blur_digests[-1]}")
    quit(shot_unit, shot_socket)

start(env={"TOMOE_TIMINGS": "1"}, unit="tomoe-blur", socket="blur")
cli(f"mount {BLUR_POLICY}", "blur")
assert inspect("blur")[":LAST-ERROR"] == "NIL", inspect("blur")[":LAST-ERROR"]
blur_costs = {}
seen = 0
for count in COUNTS:
    spawn("blur", seen, count, repaint=True, unit="tomoe-blur")
    seen = count
    time.sleep(2.5)
    still = measure("tomoe-blur", "blur")
    cli(f"mount {BLUR_BACKDROP}", "blur")
    time.sleep(1)
    moving = measure("tomoe-blur", "blur")
    cli("unmount draw-backdrop", "blur")
    time.sleep(1)
    blur_costs[count] = (still, moving)
    print(f"blur windows {count}: static backdrop {describe(still)}")
    print(f"blur windows {count}: changing backdrop {describe(moving)}")
    if still["counted"]:
        assert still["reused"] >= 1, f"{count} blurred windows reused no blur: {describe(still)}"
        assert moving["blurs"] >= 1, \
            f"{count} blurred windows recomputed no blur: {describe(moving)}"
machine.succeed("systemctl stop blur-0")
settled = 0
for _ in range(20):
    before_settle = frames("blur")
    time.sleep(1)
    settled = settled + 1 if frames("blur") == before_settle else 0
    if settled:
        break
try_cli("timings reset", "blur")
blur_idle = ticks("tomoe-blur")
machine.sleep(2)
blur_idle = ticks("tomoe-blur") - blur_idle
blur_idle_frames = channel("blur", "frame-total").get("count", 0)
print(f"blur idle: {blur_idle * 10000} us and {blur_idle_frames} frames in 2 s")
quit("tomoe-blur", "blur")

assert blur_idle <= 1, f"blurred idle cost {blur_idle * 10000} us over 2 s"
assert blur_idle_frames == 0, f"blurred idle drew {blur_idle_frames} frames"
if BLUR_DIGESTS:
    assert blur_digests == BLUR_DIGESTS, \
        f"the blurred composited bytes changed: {blur_digests} != {BLUR_DIGESTS}"
