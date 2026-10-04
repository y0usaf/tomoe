import json

CAST_BUS = "unix:path=/run/tomoe/tomoe.check.bus"
start(
    "--bare",
    bus=False,
    env={
        "XDG_DATA_DIRS": "/run/current-system/sw/share",
        "NIX_XDG_DESKTOP_PORTAL_DIR": "/run/current-system/sw/share/xdg-desktop-portal/portals",
    },
)
machine.succeed(
    "systemd-run --unit=pipewire-check --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=PIPEWIRE_CONFIG_DIR={PIPEWIRE_CONFIG} pipewire"
)
machine.wait_for_file(f"{RUNTIME}/pipewire-0")


def until(check, what, timeout=30):
    for _ in range(timeout * 10):
        if check():
            return
        time.sleep(0.1)
    raise AssertionError(what)


def nodes():
    dump = json.loads(machine.succeed(f"XDG_RUNTIME_DIR={RUNTIME} pw-dump"))
    return [
        o["id"]
        for o in dump
        if o.get("type") == "PipeWire:Interface:Node"
        and o["info"]["props"].get("node.name", "").startswith("tomoe-portal")
    ]


def stream_threads():
    pids = machine.execute("pgrep -f '[x]dg-desktop-portal-tomoe'")[1].split()
    return [
        name
        for pid in pids
        for name in machine.execute(f"cat /proc/{pid}/task/*/comm")[1].split()
        if name.startswith("portal-")
    ]


def gone():
    return not nodes() and not stream_threads()


def cast(unit, types=1, close=False):
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={CAST_BUS} "
        + ("--setenv=CLOSE=1 " if close else "")
        + f"{CAST_PYTHON} {CAST} {types}"
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -q '^start 0'", timeout=60)


def said(unit, line):
    return machine.execute(f"journalctl -u {unit} -o cat | grep -qx '{line}'")[0] == 0


cast("viewer-killed")
assert len(nodes()) == 1, nodes()
machine.succeed("systemctl kill -s KILL viewer-killed")
until(gone, "a share whose app was killed kept its node or stream thread")

cast("viewer-closes", close=True)
until(gone, "a share the app closed kept its node or stream thread")

client("window", "--mode xdg --app-id check-window --title check --size 320x200")
cast("viewer-window", types=2)
assert len(nodes()) == 1, nodes()
machine.succeed("systemctl stop window")
until(lambda: said("viewer-window", "CLOSED"), "the app was not told its cast window closed")
until(gone, "a share whose window closed kept its node or stream thread")

cast("viewer-backend")
machine.succeed("pkill -KILL -f '[x]dg-desktop-portal-tomoe'")
until(lambda: not nodes(), "a killed portal left its nodes")
cast("viewer-after-backend")
assert len(nodes()) == 1, nodes()
machine.succeed("systemctl stop viewer-after-backend")
until(gone, "a share whose app stopped kept its node or stream thread")

cast("viewer-frontend")
machine.succeed("pkill -KILL -f '[l]ibexec/xdg-desktop-portal$'")
until(gone, "shares outlived xdg-desktop-portal")

machine.succeed("systemctl stop viewer-killed viewer-closes viewer-window viewer-backend viewer-frontend || true")
quit()
machine.succeed("systemctl stop pipewire-check")
