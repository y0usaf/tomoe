import re
import time

BAR_WIDTH = 1280
BAR_HEIGHT = 24
BAR_PIXELS = BAR_WIDTH * BAR_HEIGHT
PANEL_PIXELS = 1272 * 648
DAMAGE_BUDGET = (BAR_PIXELS + PANEL_PIXELS) // 25
MIN_FRAMES = 12


def start_binary(binary, unit, socket):
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
        f"{binary} --socket {socket} --backend headless --no-watch --bare")
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


def mount(binary, socket):
    command(binary, socket, f"mount {DAMAGE_POLICY}")
    reply = read(command(binary, socket, "inspect"))
    assert reply[1] == ":OK", show(reply)
    error = plist(reply[2])[":LAST-ERROR"]
    assert error == "NIL", f"the ui-damage policy failed: {error}"


def frames(binary, socket):
    return sum(int(value) for value in re.findall(
        r":FRAME (\d+)", command(binary, socket, "frames"), re.IGNORECASE))


def drawn_pixels(binary, socket):
    match = re.search(r":DRAWN-PIXELS (-?\d+)", command(binary, socket, "frames"),
                      re.IGNORECASE)
    return int(match.group(1)) if match else 0


def channel(binary, socket, name):
    reply = read(command(binary, socket, "timings"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(plist(reply[2])[":CHANNELS"]):
        fields = plist(entry)
        if text(fields[":NAME"]) == name:
            return int(fields.get(":P50", 0)) / 1e3
    return 0.0


def measure(binary, socket):
    command(binary, socket, "timings reset")
    time.sleep(0.3)
    before = frames(binary, socket)
    samples = []
    for _ in range(30):
        samples.append(drawn_pixels(binary, socket))
        time.sleep(0.05)
    drawn = max(frames(binary, socket) - before, 1)
    assert drawn >= MIN_FRAMES, f"{drawn} frames in 1.5 s is too few to sample"
    cost = {"frames": drawn, "damage": max(samples)}
    for name in ("transaction", "frame-draw", "frame-raster", "frame-total"):
        cost[name] = channel(binary, socket, name)
    return cost


def describe(cost):
    return (f"{cost['frames']} frames, up to {cost['damage']} px damaged per tick, "
            f"transaction {cost['transaction']:.1f} us and draw/raster/total "
            f"{cost['frame-draw']:.1f}/{cost['frame-raster']:.1f}/{cost['frame-total']:.1f} us "
            "p50")


def run(binary, tag):
    unit, socket = f"tomoe-{tag}-damage", f"uidamage-{tag}"
    start_binary(binary, unit, socket)
    mount(binary, socket)
    cost = measure(binary, socket)
    print(f"{tag}: " + describe(cost), flush=True)
    stop_binary(binary, unit, socket)
    return cost


cost = run("tomoe", "branch")
assert cost["damage"] <= DAMAGE_BUDGET, (
    f"one tick damaged {cost['damage']} px of shell surfaces, over the {DAMAGE_BUDGET} px "
    f"of a {BAR_PIXELS + PANEL_PIXELS} px bar and panel a partial repaint may take: "
    + describe(cost))
print(f"branch: {cost['damage']} px damaged per tick", flush=True)

if BASELINE:
    base_cost = run(BASELINE, "base")
    assert base_cost["damage"] >= 2 * cost["damage"], (
        f"one tick damaged {cost['damage']} px against the baseline's "
        f"{base_cost['damage']} px, not the 2x a retained canvas must save: "
        + describe(base_cost))
    for name in ("frame-raster", "frame-total"):
        ratio = base_cost[name] / cost[name]
        print(f"ui update {name} {base_cost[name]:.1f} -> {cost[name]:.1f} us p50, "
              f"{ratio:.2f}x", flush=True)
        assert ratio >= 2.0, (
            f"a ui update costs {ratio:.2f}x less {name} time than the baseline, not 2x: "
            + describe(base_cost) + " -> " + describe(cost))

quit()
