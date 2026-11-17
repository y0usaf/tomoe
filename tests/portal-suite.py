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

import re


def node_of(unit):
    line = machine.succeed(f"journalctl -u {unit} -o cat | grep '^start 0'")
    match = re.search(r"\[\((\d+)", line)
    assert match, line
    return int(match.group(1))


def consume(node, mode, milliseconds, probe=None):
    out = machine.execute(
        f"XDG_RUNTIME_DIR={RUNTIME} {CONSUMER} {node} {mode} {milliseconds}"
        + (f" {probe}" if probe else ""), check_return=False)[1]
    print("consumer " + mode + ": " + out.replace("\n", " | "), flush=True)
    return out


def portal_says():
    text = machine.execute(
        "journalctl --no-pager -o cat | grep '^portal: ' | tail -20", check_return=False)[1]
    print("portal log: " + text.replace("\n", " | "), flush=True)


def resident(pattern):
    pid = machine.succeed(f"pgrep -o -f '{pattern}'").strip()
    status = machine.succeed(f"cat /proc/{pid}/status")
    return int(re.search(r"VmRSS:\s+(\d+)", status).group(1)) // 1024


def usage(pattern):
    pid = machine.succeed(f"pgrep -o -f '{pattern}'").strip()
    stat = machine.succeed(f"cat /proc/{pid}/stat").split()
    ticks = (int(stat[13]) + int(stat[14])) * 1000 // 100
    slices = sum(int(line.split()[2]) for line
                 in machine.succeed(f"cat /proc/{pid}/task/*/schedstat").splitlines() if line)
    return ticks, slices


def portal_cast(unit, types=1, close=False, cursor_mode=None):
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={CAST_BUS} "
        + ("--setenv=CLOSE=1 " if close else "")
        + (f"--setenv=CURSOR_MODE={cursor_mode} " if cursor_mode else "")
        + f"{CAST_PYTHON} {CAST} {types}"
    )
    try:
        machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -q '^start 0'", timeout=60)
    except Exception:
        print("share never started; app said: "
              + machine.execute(f"journalctl -u {unit} -o cat")[1].replace("\n", " | "), flush=True)
        portal_says()
        raise


portal_cast("viewer-resize")
node = node_of("viewer-resize")
machine.succeed(
    f"systemd-run --unit=consumer-resize --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} {CONSUMER} {node} dmabuf 8000")


def consumer_text():
    return machine.execute("journalctl -u consumer-resize -o cat")[1]


def consumer_says(pattern, what, timeout=30):
    for _ in range(timeout * 10):
        match = re.search(pattern, consumer_text(), re.M)
        if match:
            return match
        time.sleep(0.1)
    raise AssertionError(what + ": " + consumer_text().replace("\n", " | "))


consumer_says(r"^format \d+ modifier (?:0x[0-9a-f]+|none) 1280x720 frames \d+$",
              "the consumer never received the output at 1280x720")
machine.succeed(ENV + "WAYLAND_DISPLAY=check wlr-randr --output HEADLESS-1 "
                      "--custom-mode 960x540@60Hz")
changed = consumer_says(r"^format \d+ modifier (?:0x[0-9a-f]+|none) 960x540 frames (\d+)$",
                        "the share never renegotiated the stream to 960x540")
done = consumer_says(r"^frames (\d+) ", "the consumer never finished counting frames", timeout=40)
assert int(done.group(1)) > int(changed.group(1)), (done.group(0), changed.group(0))
print(f"renegotiation: frames before={changed.group(1)} after={done.group(1)}", flush=True)
machine.succeed("systemctl stop viewer-resize")
until(gone, "the resized share kept its node")
machine.succeed("systemctl stop consumer-resize || true")

machine.succeed(
    f"systemd-run --unit=portal-cursor --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check tomoe-cursor-client")
machine.wait_until_succeeds("journalctl -u portal-cursor -o cat | grep -qx cursor-ready")

portal_cast("viewer-metadata", cursor_mode=4)
node = node_of("viewer-metadata")
out = consume(node, "dmabuf", 2500)
portal_says()
assert "alloc dmabuf" in out, out
assert re.search(r"format \d+ modifier 0x[0-9a-f]+", out), out
assert "cursor visible=1" in out, out
assert re.search(r"^frames [1-9]", out, re.M), out
assert int(re.search(r"^frames (\d+)", out, re.M).group(1)) < 200, out

machine.succeed(
    f"systemd-run --unit=pointer-portal --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{POINTER} --path '300,300;620,320;620,520' --steps 4 --interval-ms 16 "
    f"--hold-ms 200 --delay-ms 100 --extent 1280x720")
moved = consume(node, "dmabuf", 2000)
positions = set(re.findall(r"position=(-?\d+),(-?\d+)", moved))
assert len(positions) >= 3, moved
assert re.search(r"bitmap=\d+x\d+@\d+", moved), moved
print("metadata positions:", sorted(positions), flush=True)
machine.execute("systemctl stop pointer-portal", check_return=False)

machine.succeed("systemctl stop viewer-metadata")
until(gone, "the metadata share kept its node")
machine.succeed(
    f"systemd-run --unit=pointer-hold --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{POINTER} --path '300,300;300,300' --steps 1 --interval-ms 16 --hold-ms 30000 "
    f"--delay-ms 100 --extent 960x540")
machine.wait_until_succeeds("journalctl -u pointer-hold -o cat | grep -qx pressed", timeout=30)
portal_cast("viewer-embed", cursor_mode=2)
node = node_of("viewer-embed")
machine.succeed(
    f"systemd-run --unit=consumer-embed --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} {CONSUMER} {node} shm 9000 300 300 600 300")


def consumer_text(unit):
    return machine.execute(f"journalctl -u {unit} -o cat")[1]


def probes(unit):
    return [line.split()[1:3] for line in consumer_text(unit).splitlines()
            if line.startswith("probe ")]


until(lambda: len(probes("consumer-embed")) >= 1, "the embedded share delivered no frame")
machine.succeed(
    f"systemd-run --unit=pointer-move --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{POINTER} --path '600,300;600,300' --steps 1 --interval-ms 16 --hold-ms 3000 "
    f"--delay-ms 100 --extent 960x540")
machine.wait_until_succeeds("journalctl -u pointer-move -o cat | grep -qx released", timeout=30)
until(lambda: len(probes("consumer-embed")) >= 2,
      "the embedded share delivered no frame after the pointer moved")
seen = probes("consumer-embed")
first, last = seen[0], seen[-1]
assert first[0] != last[0], consumer_text("consumer-embed")
assert first[1] != last[1], consumer_text("consumer-embed")
assert "alloc memfd" in consumer_text("consumer-embed"), consumer_text("consumer-embed")
print(f"embedded cursor block {first[0]} then {last[0]}, control {first[1]} then {last[1]}",
      flush=True)
machine.succeed("systemctl stop pointer-hold pointer-move viewer-embed consumer-embed || true")
until(gone, "the embedded-cursor share kept its node")
portal_cast("viewer-idle", cursor_mode=4)
node = node_of("viewer-idle")

portal_before = usage("[x]dg-desktop-portal-tomoe")
tomoe_before = usage("tomoe --socket check")
idle = consume(node, "dmabuf", 4000)
portal_after = usage("[x]dg-desktop-portal-tomoe")
tomoe_after = usage("tomoe --socket check")
frames = int(re.search(r"^frames (\d+)", idle, re.M).group(1))
portal_cpu = portal_after[0] - portal_before[0]
tomoe_cpu = tomoe_after[0] - tomoe_before[0]
portal_wakes = portal_after[1] - portal_before[1]
tomoe_wakes = tomoe_after[1] - tomoe_before[1]
print(f"idle share: frames={frames} portal_cpu_ms={portal_cpu} tomoe_cpu_ms={tomoe_cpu} "
      f"portal_wakeups={portal_wakes} tomoe_wakeups={tomoe_wakes} "
      f"portal_rss_mb={resident('[x]dg-desktop-portal-tomoe')} "
      f"tomoe_rss_mb={resident('tomoe --socket check')}", flush=True)
assert frames <= 4, "an idle share kept the compositor drawing: " + str(frames)
assert portal_cpu < 300, "the idle portal burned " + str(portal_cpu) + " ms"
machine.succeed("systemctl stop viewer-idle")
until(gone, "the idle share kept its node")

portal_cast("viewer-survive", cursor_mode=4)
node = node_of("viewer-survive")
machine.succeed(
    f"systemd-run --unit=consumer-survive --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} {CONSUMER} {node} dmabuf 4000")
until(lambda: "link" in machine.execute("journalctl -u consumer-survive -o cat")[1],
      "the share never produced a consumer link")
machine.succeed("pkill -KILL -f '[x]dg-desktop-portal-tomoe'")
until(gone, "a killed portal left its nodes")
machine.succeed(ENV + "tomoe --socket check inspect")
machine.succeed(ENV + "tomoe --socket check msg version")
until(lambda: machine.execute("systemctl is-active consumer-survive")[0] != 0,
      "the consumer outlived the killed portal")
portal_cast("viewer-after-kill")
assert len(nodes()) == 1, nodes()
quit()
machine.succeed("systemctl stop pipewire-check")
