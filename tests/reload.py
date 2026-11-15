import time

COLOUR = "336699"
VERSION_A = "A"
VERSION_B = "B"
REPORT = "/run/reload/report"


def call(arguments, timeout=60):
    return machine.succeed(ENV + "tomoe --socket check " + arguments, timeout=timeout)


def state():
    reply = read(call("inspect"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(plist(reply[2])[":EXTENSIONS"]):
        entry = plist(entry)
        if text(entry.get(":NAME", '""')) == "reload-probe":
            return plist(entry[":STATE"])
    return {}


def install(version):
    source = POLICY_A if version == VERSION_A else POLICY_B
    machine.succeed("mkdir -p /run/reload")
    machine.succeed(f"cp {source} /run/reload/init.lisp")


def capture_colour():
    machine.succeed("mkdir -p /run/reload")
    machine.execute("systemctl stop reload-capture", check_return=False)
    machine.succeed(
        "systemd-run --unit=reload-capture --property=Type=exec "
        f"--property=StandardOutput=file:{REPORT} "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{CAPTURE_CLIENT} --windows 1 --frames 3 --seconds 20")
    machine.wait_until_succeeds(
        "systemctl show -P SubState reload-capture | grep -qx 'exited\\|failed'", timeout=90)
    status = machine.succeed("systemctl show -P ExecMainStatus reload-capture").strip()
    assert status == "0", "reload-capture exited " + status + ": " + machine.succeed(
        "journalctl -u reload-capture -o cat | tail -20")
    ready = []
    for line in machine.succeed(f"cat {REPORT}").splitlines():
        parts = line.split()
        if parts[0] == "frame" and parts[3] == "ready":
            ready.append(int(parts[5], 16) & 0xFFFFFF)
    assert ready, "no captured frame: " + machine.succeed(f"cat {REPORT}")
    assert all(pixel == int(COLOUR, 16) for pixel in ready), ready
    return ready


start("--bare")
install(VERSION_A)
cli("mount /run/reload/init.lisp")
assert state().get(":VERSION") == f'"{VERSION_A}"', state()
machine.succeed(
    f"systemd-run --unit=reload-win --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"tomoe-test-client --seconds 3600 --app-id reload-target --title reload "
    f"--size 400x300 --color {COLOUR}")
machine.wait_until_succeeds("journalctl -u reload-win -o cat | grep -qx mapped", timeout=60)
time.sleep(0.5)
assert capture_colour(), "the window's colour did not reach the output before the reload"

before = state()
assert before.get(":HITS") == "0", before
call('event \'(:type :key :owner "reload-probe" :command "hit" :state :pressed)\'')
time.sleep(0.5)
after = state()
assert after.get(":HITS") == "1", after

install(VERSION_B)
call("reload")
for _ in range(100):
    if state().get(":VERSION") == f'"{VERSION_B}"':
        break
    time.sleep(0.1)
assert state().get(":VERSION") == f'"{VERSION_B}"', state()
assert state().get(":HITS") == "1", state()

assert capture_colour(), "the window's colour stopped reaching the output after the reload"
call('event \'(:type :key :owner "reload-probe" :command "hit" :state :pressed)\'')
time.sleep(0.5)
assert state().get(":HITS") == "2", state()
assert inspect()[':LAST-ERROR'] == "NIL", inspect()[':LAST-ERROR']
quit()
print("reload: the changed policy loaded, the window kept drawing and the key still counted")
