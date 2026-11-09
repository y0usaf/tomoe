import json

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
cli("event '(:type :key :owner \"commands\" :command \"close\" :state :pressed)'")
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
quit()
