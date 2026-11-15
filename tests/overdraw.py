COUNTS = [int(value) for value in OVERDRAW_COUNTS.split(",")]
LIMITS = {
    ("cover", 1): 1836375,
    ("cover", 10): 2271875,
    ("cover", 50): 13352625,
    ("grid", 1): 1829000,
    ("grid", 10): 3631250,
    ("grid", 50): 3953500,
    ("stack", 1): 2267375,
    ("stack", 10): 4248000,
    ("stack", 50): 16464125,
}
GOLDEN = [value for value in OVERDRAW_GOLDEN.split(",") if value]
BASELINE = OVERDRAW_BASELINE
WALLPAPER = OVERDRAW_WALLPAPER
COLOURS = ["3c5a78", "7a4a2c", "2c7a4a", "6a2c7a", "7a7a2c", "2c6a7a",
           "7a2c4a", "4a7a2c", "2c4a7a", "5a3c6a"]

POLICY = """
(in-package #:tomoe-user)

(defun overdraw-cell (index count width height)
  (let* ((columns (ceiling (sqrt count)))
         (rows (ceiling (/ count columns)))
         (cell-width (floor width columns))
         (cell-height (floor height rows)))
    (list (* (mod index columns) cell-width)
          (* (floor index columns) cell-height)
          cell-width cell-height)))

(define-extension "overdraw" (:reads (:windows :outputs) :state '(:stacked -1))
    (snapshot state event)
  (declare (ignore event))
  (let* ((output (first (context snapshot :outputs)))
         (width (or (and output (getf output :width)) 1280))
         (height (or (and output (getf output :height)) 720))
         (windows (sort (copy-list (context snapshot :windows))
                        (lambda (first second)
                          (string< (getf first :app-id) (getf second :app-id)))))
         (count (length windows))
         (stack (unless (eql count (getf state :stacked))
                  (loop for window in windows collect (raise-window (getf window :id))))))
    (values (list :stacked count)
            (append
             stack
             (list (settings :animations nil
                             :border (list :width {line} :radius 14)
                             :shadow (list :range {shadow} :power 3))
                   (shell-surface :overdraw-wallpaper (ui :stack)
                                  :anchors '(:top :right :bottom :left) :layer :background
                                  :background (list :shader "{wallpaper}" :fps 60)))
             (cond
               ((equal "{layout}" "cover")
                (loop for window in windows for index from 0
                      collect (if (zerop index)
                                  (place (getf window :id) 0 0 width height)
                                  (place (getf window :id) (+ 40 (* 16 index))
                                         (+ 30 (* 16 index)) (- width 200) (- height 160)))))
               ((equal "{layout}" "stack")
                (loop for window in windows for index from 0
                      collect (place (getf window :id) (+ 40 (* 16 index))
                                     (+ 30 (* 16 index)) (- width 200) (- height 160))))
               (t
                (loop for window in windows for index from 0
                      collect (apply #'place (getf window :id)
                                     (overdraw-cell index count width height)))))
             (if windows (list (focus (getf (car windows) :id))) nil))
            nil)))
"""


def policy(layout, decorated, path):
    machine.succeed(f"cat > {path} <<'OVERDRAWPOLICY'\n"
                    + POLICY.format(wallpaper=WALLPAPER, layout=layout,
                                    line=3 if decorated else 0,
                                    shadow=10 if decorated else 0)
                    + "OVERDRAWPOLICY")
    return path


def start_binary(binary, unit, socket, options=()):
    machine.wait_for_unit("multi-user.target")
    machine.execute(
        "systemd-run --unit=check-bus --property=Type=exec "
        f"dbus-daemon --session --nofork --address={BUS}")
    machine.wait_for_file("/run/check-bus")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={BUS} "
        "--setenv=HOME=/root --setenv=PATH=/run/current-system/sw/bin "
        f"--setenv=MESA_LOADER_DRIVER_OVERRIDE=zink --setenv={THREADS} "
        "--setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        + f"{binary} --socket {socket} --backend headless --no-watch --test-timings "
        + " ".join(options))
    for _ in range(600):
        if machine.execute(ENV + f"{binary} --socket {socket} inspect")[0] == 0:
            return
        assert machine.execute(f"systemctl show -P SubState {unit} | grep -qx running")[0] == 0, (
            "tomoe stopped while starting:\n"
            + machine.succeed(f"journalctl -u {unit} -o cat | tail -30"))
        time.sleep(0.1)
    raise AssertionError("tomoe did not answer inspect within 60 s")


def stop_binary(binary, unit, socket):
    machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} {binary} --socket {socket} quit")
    machine.wait_until_succeeds(
        f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'", timeout=60)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", (
        f"{unit} exited {status}: " + machine.succeed(f"journalctl -u {unit} -o cat | tail -40"))


def command(binary, socket, arguments):
    return machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} {binary} --socket {socket} "
                           + arguments)


def mount(binary, socket, layout, decorated, path):
    command(binary, socket, f"mount {policy(layout, decorated, path)}")
    reply = read(command(binary, socket, "inspect"))
    assert reply[1] == ":OK", show(reply)
    error = plist(reply[2])[":LAST-ERROR"]
    assert error == "NIL", f"the overdraw policy failed: {error}"


def frames(binary, socket):
    return sum(int(value) for value in re.findall(
        r":FRAME (\d+)", command(binary, socket, "frames"), re.IGNORECASE))


def counters(binary, socket):
    text = command(binary, socket, "frames")
    found = {}
    for name in ("gl-calls", "quads", "runs", "fragments"):
        match = re.search(r":" + name + r" (\d+)", text, re.IGNORECASE)
        if match:
            found[name] = int(match.group(1))
    return found


def channel(binary, socket, name):
    reply = read(command(binary, socket, "timings"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(plist(reply[2])[":CHANNELS"]):
        fields = plist(entry)
        if text(fields[":NAME"]) == name:
            return {key: int(fields.get(":" + key.upper(), 0)) for key in
                    ("count", "mean", "p50", "p90", "p99", "max")}
    return {}


def presented_shot(socket, limit=12):
    previous = None
    for _ in range(limit):
        digest = machine.succeed(
            ENV + f"WAYLAND_DISPLAY={socket} grim -c -t ppm /run/overdraw.ppm"
            " && sha256sum /run/overdraw.ppm").split()[0][:16]
        if digest == previous:
            return digest
        previous = digest
        time.sleep(0.4)
    return previous


def spawn(socket, first, count, repaint, unit):
    for index in range(first, count):
        assert machine.execute(f"systemctl show -P SubState {unit} | grep -qx running")[0] == 0, (
            f"{unit} is not running:\n"
            + machine.succeed(f"journalctl -u {unit} -o cat | tail -20"))
        machine.execute(
            f"systemd-run --unit={socket}-{index} --property=Type=exec "
            f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY={socket} "
            "tomoe-test-client --seconds 3600 "
            f"--mode xdg --app-id overdraw-{index:02d} --size 320x200 "
            f"--color {COLOURS[index % len(COLOURS)]}"
            + (" --commit-rate 60 --repaint" if index == 0 and repaint else ""))
    if count > first:
        units = " ".join(f"-u {socket}-{index}" for index in range(count))
        machine.wait_until_succeeds(
            f"test \"$(journalctl {units} -o cat | grep -cx mapped)\" -ge {count}",
            timeout=180)


def sample(binary, unit, socket, seconds=2):
    command(binary, socket, "timings reset")
    time.sleep(0.3)
    before_frames, before = frames(binary, socket), counters(binary, socket)
    machine.sleep(seconds)
    drawn = max(frames(binary, socket) - before_frames, 1)
    after = counters(binary, socket)
    taken = {"frames": drawn}
    for name in before:
        taken[name] = (after[name] - before[name]) / drawn
    for name in ("frame-draw", "frame-raster", "frame-total"):
        block = channel(binary, socket, name)
        taken[name] = block.get("p50", 0) / 1e6
        taken[name + "-p99"] = block.get("p99", 0) / 1e6
    return taken


def measure(binary, unit, socket, runs=3):
    taken = [sample(binary, unit, socket) for _ in range(runs)]
    cost = {}
    for name in taken[0]:
        values = sorted(one[name] for one in taken)
        cost[name] = values[len(values) // 2]
        cost[name + "-noise"] = f"{values[0]:.2f}..{values[-1]:.2f}"
    return cost


def describe(cost):
    return (f"{cost['frames']} frames, {cost.get('fragments', 0) / 1e3:.1f}k fragments, "
            f"{cost['quads']:.1f} quads, {cost['runs']:.1f} runs and "
            f"{cost['gl-calls']:.1f} gl calls/frame, draw/raster/total "
            f"{cost['frame-draw']:.2f}/{cost['frame-raster']:.2f}/{cost['frame-total']:.2f} ms "
            f"p50 in {cost['frame-draw-noise']}/{cost['frame-raster-noise']}/"
            f"{cost['frame-total-noise']}, draw {cost['frame-draw-p99']:.2f} and raster "
            f"{cost['frame-raster-p99']:.2f} ms p99")


def session(binary, tag, layout, phase, options, decorated=True):
    name = f"{tag}{layout}{phase}"
    unit, socket = f"tomoe-{tag}-{layout}-{phase}", name
    start_binary(binary, unit, socket, options=options)
    mount(binary, socket, layout, decorated, f"/run/overdraw-{layout}.lisp")
    return unit, socket


def rotate(socket, degrees):
    machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} wlr-randr --output HEADLESS-1 "
                          f"--transform {degrees}")
    listing = machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} wlr-randr")
    assert f"Transform: {degrees}" in listing, listing


def run(binary, tag):
    digests, costs = {}, {}
    unit, socket = session(binary, tag, "grid", "pix", [], decorated=False)
    seen = 0
    for count in COUNTS:
        spawn(socket, seen, count, True, unit)
        seen = count
        time.sleep(1.5)
        digests[("grid", count)] = presented_shot(socket)
        print(f"{tag} grid {count} partial: presented {digests[('grid', count)]}", flush=True)
    stop_binary(binary, unit, socket)
    for rotation in ("90", "270"):
        unit, socket = session(binary, tag, "grid", f"rot{rotation}", [], decorated=False)
        rotate(socket, rotation)
        spawn(socket, 0, COUNTS[1], True, unit)
        time.sleep(1.5)
        key = (f"rot{rotation}", COUNTS[1])
        digests[key] = presented_shot(socket)
        print(f"{tag} rotated {rotation} {COUNTS[1]}: presented {digests[key]}", flush=True)
        stop_binary(binary, unit, socket)
    for layout in ("grid", "stack", "cover"):
        unit, socket = session(binary, tag, layout, "cost", ["--test-debug-damage"])
        seen = 0
        for count in COUNTS:
            spawn(socket, seen, count, True, unit)
            seen = count
            time.sleep(2.5)
            costs[(layout, count)] = measure(binary, unit, socket)
            print(f"{tag} {layout} {count} repaint: " + describe(costs[(layout, count)]),
                  flush=True)
        stop_binary(binary, unit, socket)
    return digests, costs


digests, costs = run("tomoe", "branch")
for key, limit in LIMITS.items():
    assert costs[key]["fragments"] <= limit, (
        f"{key[0]} {key[1]} drew more pixels than the bound: " + describe(costs[key]))
if GOLDEN:
    assert [digests[key] for key in sorted(digests)] == GOLDEN, (
        f"the presented bytes changed: {digests} != {GOLDEN}")

if BASELINE:
    base_digests, base_costs = run(BASELINE, "base")
    assert base_digests == digests, (
        f"the presented bytes changed against the baseline: {base_digests} != {digests}")
    for key in sorted(costs):
        before, after = base_costs[key], costs[key]
        reported = "fragments" in before
        print(f"{key[0]} {key[1]}: fragments {before.get('fragments', 0) / 1e3:.1f}k -> "
              f"{after['fragments'] / 1e3:.1f}k, quads {before['quads']:.1f} -> "
              f"{after['quads']:.1f}, runs {before['runs']:.1f} -> {after['runs']:.1f}, "
              f"gl calls {before['gl-calls']:.1f} -> {after['gl-calls']:.1f}, draw "
              f"{before['frame-draw']:.2f} -> {after['frame-draw']:.2f} ms, raster "
              f"{before['frame-raster']:.2f} -> {after['frame-raster']:.2f} ms, total "
              f"{before['frame-total']:.2f} -> {after['frame-total']:.2f} ms", flush=True)
        if reported:
            assert after["fragments"] < before["fragments"], (
                f"{key[0]} {key[1]} drew as many fragments as the baseline: "
                + describe(after))
