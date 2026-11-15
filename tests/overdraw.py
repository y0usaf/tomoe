COUNTS = [int(value) for value in OVERDRAW_COUNTS.split(",")]
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

(define-extension "overdraw" (:reads (:windows :outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let* ((output (first (context snapshot :outputs)))
         (width (or (and output (getf output :width)) 1280))
         (height (or (and output (getf output :height)) 720))
         (windows (sort (copy-list (context snapshot :windows))
                        (lambda (first second) (< (getf first :id) (getf second :id)))))
         (count (length windows)))
    (values nil
            (append
             (list (settings :animations nil
                             :border (list :width 3 :radius 14)
                             :shadow (list :range 10 :power 3))
                   (shell-surface :overdraw-wallpaper (ui :stack)
                                  :anchors '(:top :right :bottom :left) :layer :background
                                  :background (list :shader "{wallpaper}" :fps 60)))
             (if (equal "{layout}" "stack")
                 (loop for window in windows for index from 0
                       collect (place (getf window :id) (+ 40 (* 16 index))
                                      (+ 30 (* 16 index)) (- width 200) (- height 160)))
                 (loop for window in windows for index from 0
                       collect (apply #'place (getf window :id)
                                      (overdraw-cell index count width height))))
             (if windows (list (focus (getf (car windows) :id))) nil))
            nil)))
"""


def policy(layout, path):
    machine.succeed(f"cat > {path} <<'OVERDRAWPOLICY'\n"
                    + POLICY.format(wallpaper=WALLPAPER, layout=layout)
                    + "OVERDRAWPOLICY")
    return path


def start_binary(binary, unit, socket, env=()):
    machine.wait_for_unit("multi-user.target")
    machine.execute(
        "systemd-run --unit=check-bus --property=Type=exec "
        f"dbus-daemon --session --nofork --address={BUS}")
    machine.wait_for_file("/run/check-bus")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={BUS} "
        "--setenv=TOMOE_TIMINGS=1 --setenv=HOME=/root --setenv=PATH=/run/current-system/sw/bin "
        f"--setenv=MESA_LOADER_DRIVER_OVERRIDE=zink --setenv={THREADS} "
        "--setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        + "".join(f"--setenv={name}={value} " for name, value in env)
        + f"{binary} --socket {socket} --backend headless --no-watch")
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


def mount(binary, socket, layout, path):
    command(binary, socket, f"mount {policy(layout, path)}")
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
        client(f"{socket}-{index}", f"--mode xdg --app-id overdraw-{index} --size 320x200 "
               f"--color {COLOURS[index % len(COLOURS)]}"
               + (" --commit-rate 60 --repaint" if index == 0 and repaint else ""),
               socket=socket)


def measure(binary, unit, socket, seconds=3):
    command(binary, socket, "timings reset")
    time.sleep(0.3)
    before_frames, before = frames(binary, socket), counters(binary, socket)
    machine.sleep(seconds)
    drawn = max(frames(binary, socket) - before_frames, 1)
    after = counters(binary, socket)
    cost = {"frames": drawn}
    for name in before:
        cost[name] = (after[name] - before[name]) / drawn
    for name in ("frame-draw", "frame-raster", "frame-total"):
        block = channel(binary, socket, name)
        cost[name] = block.get("p50", 0) / 1000.0
        cost[name + "-p99"] = block.get("p99", 0) / 1000.0
    return cost


def describe(cost):
    return (f"{cost['frames']} frames, {cost['fragments'] / 1e3:.1f}k fragments, "
            f"{cost['quads']:.1f} quads, {cost['runs']:.1f} runs and "
            f"{cost['gl-calls']:.1f} gl calls/frame, draw/raster/total "
            f"{cost['frame-draw']:.2f}/{cost['frame-raster']:.2f}/{cost['frame-total']:.2f} ms "
            f"p50, draw {cost['frame-draw-p99']:.2f} and raster "
            f"{cost['frame-raster-p99']:.2f} ms p99")


def session(binary, tag, layout, env):
    unit, socket = f"tomoe-{tag}-{layout}", f"{tag}{layout}"
    start_binary(binary, unit, socket, env=env)
    mount(binary, socket, layout, f"/run/overdraw-{layout}.lisp")
    return unit, socket


def run(binary, tag):
    digests, costs = {}, {}
    for layout in ("grid", "stack"):
        unit, socket = session(binary, tag, layout, [])
        seen = 0
        for count in COUNTS:
            spawn(socket, seen, count, True, unit)
            seen = count
            time.sleep(1.5)
            digests[(layout, count)] = presented_shot(socket)
            print(f"{tag} {layout} {count} partial: presented {digests[(layout, count)]}",
                  flush=True)
        stop_binary(binary, unit, socket)
    unit, socket = session(binary, tag, "stack", [("TOMOE_DEBUG_DAMAGE", "1")])
    spawn(socket, 0, COUNTS[-1], True, unit)
    time.sleep(2)
    digests[("repaint", COUNTS[-1])] = presented_shot(socket)
    print(f"{tag} repaint {COUNTS[-1]}: presented {digests[('repaint', COUNTS[-1])]}",
          flush=True)
    stop_binary(binary, unit, socket)
    for layout in ("grid", "stack"):
        unit, socket = session(binary, tag, layout, [("TOMOE_DEBUG_DAMAGE", "1")])
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
