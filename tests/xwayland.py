import json
import re

start()
machine.wait_for_unit("multi-user.target")
assert machine.execute("pgrep -x Xwayland")[0] != 0, "Xwayland started before any X client"
sockets = machine.succeed("ls /tmp/.X11-unix/X*").split()
display = ":" + sockets[0].rsplit("X", 1)[1]


def env(wayland=False):
    return ENV + ("WAYLAND_DISPLAY=check " if wayland else "") + f"DISPLAY={display} "


def unit(name, command):
    machine.succeed(
        f"systemd-run --unit={name} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DISPLAY={display} "
        "--setenv=PATH=/run/current-system/sw/bin "
        + command
    )


def windows():
    return json.loads(cli("msg windows"))


def find(title):
    return next((w for w in windows() if w["title"] == title), None)


def find_app(app_id):
    return next((w for w in windows() if w["app_id"] == app_id), None)


def wait_app(app_id):
    for _ in range(300):
        found = find_app(app_id)
        if found:
            return found
        time.sleep(0.1)
    raise AssertionError(f"application {app_id} did not map: {windows()}")


def wait_new_focused(before):
    for _ in range(300):
        found = [
            w for w in windows()
            if w["id"] not in before and w["focused"]
        ]
        if found:
            return found[0]
        time.sleep(0.1)
    raise AssertionError(f"no new focused window: {windows()}")


def wait_window(title, focused=False):
    for _ in range(300):
        found = [
            w for w in windows()
            if w["title"] == title and (not focused or w["focused"])
        ]
        if found:
            return found[0]
        time.sleep(0.1)
    raise AssertionError(f"window {title} did not map: {windows()}")


def pixel(x, y):
    values = machine.succeed(
        env(True) + f"grim -t ppm -g '{x},{y} 1x1' - | od -An -v -tu1"
    ).split()
    return tuple(int(v) for v in values[-3:])


machine.succeed(f"DISPLAY={display} xdpyinfo > /dev/null")
machine.wait_until_succeeds("pgrep -x Xwayland")
info = machine.succeed(f"DISPLAY={display} xdpyinfo")
assert "vendor string:    The X.Org Foundation" in info, info
machine.succeed(
    ENV + "WAYLAND_DISPLAY=check setsid tomoe-pointer-script --extent 64x64 "
    "'signal /tmp/keyboard-ready' 'wait /tmp/keyboard-done' > /tmp/keyboard.log 2>&1 & echo $! > /tmp/keyboard.pid"
)
machine.wait_until_succeeds("test -e /tmp/keyboard-ready || ! kill -0 $(cat /tmp/keyboard.pid)", timeout=60)
assert machine.execute("test -e /tmp/keyboard-ready")[0] == 0, machine.execute("cat /tmp/keyboard.log")[1]
machine.wait_until_succeeds(
    f"DISPLAY={display} xkbcomp -xkb {display} - | grep -c 'xkb_symbols \"pc105\"'", timeout=60
)
core = machine.succeed(f"DISPLAY={display} xmodmap -pk")
assert re.search(r"^\s*8\s*$", core, re.M), core
assert re.search(r"^\s*9\s+0xff1b \(Escape\)", core, re.M), core
assert re.search(r"^\s*38\s+0x0061 \(a\)", core, re.M), core
assert re.search(r"^\s*50\s+0xffe1 \(Shift_L\)", core, re.M), core
modmap = machine.succeed(f"DISPLAY={display} xmodmap -pm")
assert re.search(r"^shift\s+Shift_L \(0x32\)", modmap, re.M), modmap
assert re.search(r"^control\s+Control_L \(0x25\)", modmap, re.M), modmap
machine.succeed("touch /tmp/keyboard-done")
machine.wait_until_fails("kill -0 $(cat /tmp/keyboard.pid)", timeout=60)
machine.succeed(f"DISPLAY={display} xset +fp {X11_FONTS} && DISPLAY={display} xset fp rehash")

unit("xterm", "xterm -bg black -fg white -e sleep 3600")
term = wait_app("XTerm")
assert term["app_id"] == "XTerm", term
assert term["focused"], term
machine.succeed("test -x /run/current-system/sw/bin/zenity")
unit("zenity", "env GDK_BACKEND=x11 zenity --error --text hello --title CheckGTK")
gtk = wait_window("CheckGTK")
assert gtk["app_id"] == "zenity", gtk

supported = machine.succeed(f"DISPLAY={display} xprop -root _NET_SUPPORTED")
for name in ["_NET_SUPPORTED", "_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST", "_NET_WM_STATE"]:
    assert name in supported, supported

before = len(windows())
unit("override", f"{X11_CLIENT} override")
wait_window("X11 Parent")
time.sleep(1)
assert len(windows()) == before + 1, windows()
for _ in range(100):
    if pixel(310, 210) == (0, 255, 0):
        break
    time.sleep(0.1)
assert pixel(310, 210) == (0, 255, 0), pixel(310, 210)
assert find("X11 Override") is None, windows()

unit("transient", f"{X11_CLIENT} transient")
wait_window("X11 Transient")
state = inspect()
transient = next(
    plist(entry) for entry in items(state[":WINDOWS"]) if text(plist(entry)[":TITLE"]) == "X11 Transient"
)
parents = [
    plist(entry) for entry in items(state[":WINDOWS"])
    if text(plist(entry)[":TITLE"]) == "X11 Parent"
]
assert transient[":DIALOG"] == "T", transient
assert any(text(parent[":ID"]) == text(transient[":PARENT"]) for parent in parents), (
    transient, parents
)

xterm_id = machine.succeed(f"DISPLAY={display} xdotool search --class XTerm").split()[0]
machine.succeed(f"DISPLAY={display} wmctrl -i -r {xterm_id} -b add,fullscreen")
fresh = None
for _ in range(100):
    state = inspect()
    fresh = next(
        plist(entry) for entry in items(state[":WINDOWS"])
        if text(plist(entry)[":APP-ID"]) == "XTerm"
    )
    if fresh[":FULLSCREEN-REQUESTED"] == "T":
        break
    time.sleep(0.1)
assert fresh[":FULLSCREEN-REQUESTED"] == "T", fresh

unit("offscreen", f"{X11_CLIENT} offscreen")
offscreen = wait_window("X11 Offscreen")
for _ in range(100):
    if offscreen.get("geometry"):
        break
    time.sleep(0.1)
    offscreen = find("X11 Offscreen") or offscreen
assert offscreen.get("geometry"), offscreen
offscreen_id = machine.succeed(
    f"DISPLAY={display} xdotool search --name 'X11 Offscreen'"
).split()[0]


def x_geometry_of(window_id):
    fields = dict(
        line.split("=", 1)
        for line in machine.succeed(
            f"DISPLAY={display} xdotool getwindowgeometry --shell {window_id}"
        ).splitlines()
        if "=" in line
    )
    return tuple(int(fields[key]) for key in ("X", "Y", "WIDTH", "HEIGHT"))


def x_geometry():
    return x_geometry_of(offscreen_id)


offscreen_box = offscreen["geometry"]
want_geometry = (
    offscreen_box["x"],
    offscreen_box["y"],
    offscreen_box["w"],
    offscreen_box["h"],
)
for _ in range(100):
    if x_geometry() == want_geometry:
        break
    time.sleep(0.1)
assert x_geometry() == want_geometry, (x_geometry(), want_geometry)

offscreen_color = (0x33, 0x66, 0xCC)
sample = (offscreen_box["x"] + offscreen_box["w"] - 12,
          offscreen_box["y"] + offscreen_box["h"] // 2)
for _ in range(100):
    if pixel(*sample) == offscreen_color:
        break
    time.sleep(0.1)
assert pixel(*sample) == offscreen_color, (pixel(*sample), offscreen_color)


def wait_gone(title):
    for _ in range(100):
        if find(title) is None:
            return
        time.sleep(0.1)
    raise AssertionError(f"withdrawn {title} kept a window: {windows()}")


def wait_drawn(title):
    for _ in range(100):
        found = find(title)
        box = found and found.get("geometry")
        if box and box["w"] > 1 and box["h"] > 1:
            return found
        time.sleep(0.1)
    raise AssertionError(f"{title} did not come back with a size: {windows()}")


machine.succeed(f"DISPLAY={display} xdotool windowunmap {offscreen_id}")
wait_gone("X11 Offscreen")
machine.succeed(f"DISPLAY={display} xdotool windowmap {offscreen_id}")
offscreen_box = wait_drawn("X11 Offscreen")["geometry"]
want_geometry = (offscreen_box["x"], offscreen_box["y"], offscreen_box["w"], offscreen_box["h"])
sample = (offscreen_box["x"] + offscreen_box["w"] - 12,
          offscreen_box["y"] + offscreen_box["h"] // 2)
for _ in range(100):
    if pixel(*sample) == offscreen_color and x_geometry() == want_geometry:
        break
    time.sleep(0.1)
assert pixel(*sample) == offscreen_color, (pixel(*sample), offscreen_color)
assert x_geometry() == want_geometry, (x_geometry(), want_geometry)

remap_titles = ["X11 Remap A", "X11 Remap B"]
remap_ids = {}
for index, title in enumerate(remap_titles):
    unit(f"remap-{index}", f"{X11_CLIENT} normal '{title}'")
    wait_window(title)
    remap_ids[title] = machine.succeed(
        f"DISPLAY={display} xdotool search --name '^{title}$'"
    ).split()[0]


for remap_order in [remap_titles, remap_titles[::-1]] * 2:
    for title in remap_titles:
        machine.succeed(f"DISPLAY={display} xdotool windowunmap {remap_ids[title]}")
        wait_gone(title)
    for title in remap_order:
        machine.succeed(f"DISPLAY={display} xdotool windowmap {remap_ids[title]}")
        wait_drawn(title)
machine.succeed("systemctl stop remap-0 remap-1")


def x_side():
    found = []
    for window_id in machine.succeed(
        f"DISPLAY={display} xdotool search --onlyvisible --name '^X11 '"
    ).split():
        title = machine.succeed(f"DISPLAY={display} xdotool getwindowname {window_id}").strip()
        if title != "X11 Override":
            found.append((title, x_geometry_of(window_id)))
    return sorted(found)


def reported_side():
    found = []
    for window in windows():
        box = window.get("geometry")
        if box and window["title"].startswith("X11 "):
            found.append((window["title"], (box["x"], box["y"], box["w"], box["h"])))
    return sorted(found)


missed = []
for index in range(8):
    unit(f"fresh-{index}", f"{X11_CLIENT} normal 'X11 Fresh {index}'")
    wait_drawn(f"X11 Fresh {index}")
    for _ in range(100):
        if x_side() == reported_side():
            break
        time.sleep(0.1)
    if x_side() != reported_side():
        missed.append((index, x_side(), reported_side()))
    machine.succeed(f"systemctl stop fresh-{index}")
    wait_gone(f"X11 Fresh {index}")
assert not missed, missed

machine.succeed(env(True) + "sh -c 'printf wayland-data | timeout 10 wl-copy' >/dev/null 2>&1")
time.sleep(0.5)
assert machine.succeed(f"DISPLAY={display} timeout 10 xclip -selection clipboard -o") == "wayland-data"
machine.succeed(f"printf x11-data | DISPLAY={display} timeout 10 xclip -selection clipboard -i >/dev/null 2>&1")
time.sleep(0.5)
assert machine.succeed(env(True) + "timeout 10 wl-paste --no-newline") == "x11-data"

machine.succeed("head -c 1048576 /dev/zero | tr '\\0' a > /run/big")
machine.succeed(f"DISPLAY={display} timeout 10 xclip -selection clipboard -i < /run/big >/dev/null 2>&1")
time.sleep(0.5)
machine.succeed(env(True) + "timeout 10 wl-paste --no-newline > /run/big-w")

machine.succeed(env(True) + "timeout 10 wl-copy < /run/big >/dev/null 2>&1")
time.sleep(0.5)
machine.succeed(f"DISPLAY={display} timeout 10 xclip -selection clipboard -o > /run/big-x")
want = machine.succeed("sha256sum /run/big | cut -d' ' -f1").strip()
for path in ["/run/big-w", "/run/big-x"]:
    got = machine.succeed(f"sha256sum {path} | cut -d' ' -f1").strip()
    assert got == want, (path, got, want)


machine.succeed("rm -f /run/x11-closed")
before_ids = {w["id"] for w in windows()}
unit("closeable", f"env X11_CLIENT_MARK=/run/x11-closed {X11_CLIENT} closeable")
closed = wait_new_focused(before_ids)
closed_id = closed["id"]
time.sleep(1)
cli("command close-window")
for _ in range(100):
    if machine.execute("test -e /run/x11-closed")[0] == 0:
        break
    time.sleep(0.1)
assert machine.execute("test -e /run/x11-closed")[0] == 0, "WM_DELETE_WINDOW was not delivered"
for _ in range(100):
    if not any(w["id"] == closed_id for w in windows()):
        break
    time.sleep(0.1)
assert not any(w["id"] == closed_id for w in windows()), windows()
unit("silent", f"{X11_CLIENT} silent")
time.sleep(1)
unit("traffic", f"sh -c 'while :; do xdotool windowsize {xterm_id} 400 300; xdotool windowsize {xterm_id} 500 350; done'")
machine.succeed("rm -f /run/silent-status /run/silent-paste")
unit("paste", "env WAYLAND_DISPLAY=check sh -c 'timeout 12 wl-paste --no-newline >/run/silent-paste 2>/dev/null; echo $? >/run/silent-status'")
time.sleep(0.5)
started = time.monotonic()
windows()
assert time.monotonic() - started < 8, "silent selection owner blocked the control socket"
machine.wait_until_succeeds("test -s /run/silent-status")
status = int(machine.succeed("cat /run/silent-status").strip())
assert status != 124, status
machine.succeed("systemctl stop traffic")
machine.succeed("systemctl stop silent")

tomoe_pid = machine.succeed("systemctl show -P MainPID tomoe").strip()


def stall(server, resume):
    machine.succeed("rm -f /run/xwm-stalled")
    machine.succeed(f"kill -STOP {server}")
    wake = f"kill -CONT {server}; " if resume else ""
    unit(
        "stall-watch",
        "sh -c 'until ! timeout 1 tomoe --socket check msg windows >/dev/null; "
        f"do sleep 0.2; done; {wake}touch /run/xwm-stalled'",
    )
    machine.execute(
        ENV + "sh -c 'for i in $(seq 600); do test -e /run/xwm-stalled && break; "
        "timeout 20 tomoe --socket check command focus-next-window >/dev/null; done'",
        timeout=600,
    )
    stalled = machine.execute("test -e /run/xwm-stalled")[0] == 0
    machine.execute("systemctl stop stall-watch; systemctl reset-failed stall-watch")
    return stalled


old_server = machine.succeed("pgrep -x Xwayland").strip()
stalled = stall(old_server, True)
machine.execute(f"kill -CONT {old_server}")
unit("after-stall", f"{X11_CLIENT} normal 'X11 After Stall'")
wait_drawn("X11 After Stall")
assert stalled, "Tomoe never waited on a full window manager socket"
assert machine.succeed("pgrep -x Xwayland").strip() == old_server
assert machine.execute(
    "journalctl -u tomoe -o cat | grep -q 'Xwayland window manager failed'"
)[0] != 0, machine.succeed("journalctl -u tomoe -o cat | tail -20")
machine.succeed("systemctl stop after-stall")

assert stall(old_server, False), "Tomoe never waited on a stopped Xwayland"
machine.wait_until_succeeds(
    "journalctl -u tomoe -o cat | grep -q 'Xwayland window manager failed.*send timed out'",
    timeout=30,
)
machine.wait_until_succeeds(
    "journalctl -u tomoe -o cat | grep -q 'Xwayland .* ignored SIGTERM'", timeout=30
)
machine.wait_until_succeeds(f"! kill -0 {old_server}", timeout=30)
unit("after-timeout", f"{X11_CLIENT} normal 'X11 After Timeout'")
wait_drawn("X11 After Timeout")
timeout_server = machine.succeed("pgrep -x Xwayland").strip()
assert timeout_server != old_server, (old_server, timeout_server)
machine.succeed("systemctl stop after-timeout")
wait_gone("X11 After Timeout")


def wm_fds():
    links = machine.succeed(
        f"for f in /proc/{tomoe_pid}/fd/*; do readlink $f; done"
    ).split()
    return sorted(
        re.sub(r"\[\d+\]", "", link) for link in links
        if not link.startswith(("/dmabuf:", "/memfd:", "anon_inode:sync_file"))
    )


fds_before = wm_fds()
wm_window = machine.succeed(
    f"DISPLAY={display} xprop -root -notype _NET_SUPPORTING_WM_CHECK"
).split()[-1]
machine.execute(f"DISPLAY={display} timeout 10 xdotool windowkill {int(wm_window, 16)}")
machine.wait_until_succeeds(
    "journalctl -u tomoe -o cat | grep -q 'Xwayland window manager failed.*connection closed'",
    timeout=30,
)
machine.wait_until_succeeds(f"! kill -0 {timeout_server}", timeout=30)
unit("after-wm", f"{X11_CLIENT} normal 'X11 After WM'")
wait_drawn("X11 After WM")
new_server = machine.succeed("pgrep -x Xwayland").strip()
assert new_server != timeout_server, (timeout_server, new_server)
machine.succeed("systemctl stop after-wm")
wait_gone("X11 After WM")
for _ in range(100):
    fds_after = wm_fds()
    if fds_after == fds_before:
        break
    time.sleep(0.1)
assert fds_after == fds_before, (fds_before, fds_after)
quit()
