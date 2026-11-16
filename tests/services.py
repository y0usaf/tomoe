import time

DIRECTORY = "/run/services"
RUNTIME = "/run/tomoe"
CONTROL = f"XDG_RUNTIME_DIR={RUNTIME} DBUS_SESSION_BUS_ADDRESS=disabled: tomoe"

machine.succeed(f"rm -rf {DIRECTORY}")
machine.succeed(f"install -d -m 700 {RUNTIME} {DIRECTORY}")
machine.succeed(f"cp {SERVICE_PROBE} {DIRECTORY}/probe.py")


def policy(name, socket_path, pid_path, child_path, fork):
    source = f"""(in-package #:tomoe-user)
(define-extension "probe" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (service :probe (list "{SERVICE_PYTHON}" "{DIRECTORY}/probe.py"
                                          "{socket_path}" "{pid_path}" "{fork}"
                                          "{child_path}"))) nil))
"""
    machine.succeed(f"cat > {DIRECTORY}/{name}.lisp <<'LISP'\n{source}LISP")


def launch(unit, socket, policy_path):
    machine.execute(f"systemctl stop {unit} 2>/dev/null; "
                    f"systemctl reset-failed {unit} 2>/dev/null", check_return=False)
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --property=KillMode=process "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS=disabled: "
        "--setenv=HOME=/root --setenv=PATH=/run/current-system/sw/bin "
        "--setenv=MESA_LOADER_DRIVER_OVERRIDE=zink --setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        f"tomoe --socket {socket} --backend headless --no-watch --config {policy_path}")
    for _ in range(600):
        if machine.execute(CONTROL + f" --socket {socket} inspect")[0] == 0:
            return
        if machine.execute(f"systemctl show -P SubState {unit} | grep -qx running")[0] != 0:
            raise AssertionError("tomoe stopped while starting:\n"
                                 + machine.succeed(f"journalctl -u {unit} -o cat | tail -30"))
        time.sleep(0.1)
    raise AssertionError("tomoe did not answer inspect within 60 s")


def running(pid):
    return machine.execute(
        f"grep -qE '^State:[[:space:]]+[^Z]' /proc/{pid}/status")[0] == 0


def wait_running(path, seconds=20):
    for _ in range(seconds * 20):
        if machine.execute(f"test -s {path}")[0] == 0:
            pid = int(machine.succeed(f"cat {path}"))
            if running(pid):
                return pid
        time.sleep(0.05)
    raise AssertionError(f"{path} never named a running process")


def wait_gone(pid, seconds=10):
    for _ in range(seconds * 20):
        if not running(pid):
            return True
        time.sleep(0.05)
    return False


def kill_compositor(unit):
    pid = int(machine.succeed(f"systemctl show -P MainPID {unit}").strip())
    assert pid > 1, f"{unit} has no main process"
    machine.succeed(f"kill -9 {pid}")
    return pid


def service_sockets():
    listing = machine.execute(
        f"find {RUNTIME}/tomoe-services -name probe.sock -print 2>/dev/null",
        check_return=False)[1]
    return [line for line in listing.split("\n") if line]


def exists(path):
    return machine.execute(f"test -e {path}")[0] == 0


policy("direct", f"{DIRECTORY}/direct.sock", f"{DIRECTORY}/direct.pid",
       f"{DIRECTORY}/direct.child", 0)
launch("tomoe-direct", "direct", f"{DIRECTORY}/direct.lisp")
direct = wait_running(f"{DIRECTORY}/direct.pid")
kill_compositor("tomoe-direct")
assert wait_gone(direct), f"a service outlived a SIGKILLed compositor: {direct}"

policy("leftover", "-", f"{DIRECTORY}/leftover.pid", f"{DIRECTORY}/leftover.child", 1)
launch("tomoe-leftover", "leftover", f"{DIRECTORY}/leftover.lisp")
leftover = wait_running(f"{DIRECTORY}/leftover.pid")
child = wait_running(f"{DIRECTORY}/leftover.child")
sockets = service_sockets()
assert len(sockets) == 1, f"the service's socket is not in its own directory: {sockets}"
dead_socket = sockets[0]
kill_compositor("tomoe-leftover")
assert wait_gone(leftover), f"a service outlived a SIGKILLed compositor: {leftover}"
assert running(child), f"the service's forked child did not survive: {child}"
assert exists(dead_socket), f"the leftover child is not holding {dead_socket}"

policy("live", f"{DIRECTORY}/live.sock", f"{DIRECTORY}/live.pid",
       f"{DIRECTORY}/live.child", 0)
launch("tomoe-live", "live", f"{DIRECTORY}/live.lisp")
live = wait_running(f"{DIRECTORY}/live.pid")
machine.succeed("systemd-run --unit=decoy --property=Type=exec sleep 300")
for _ in range(200):
    decoy = machine.succeed("systemctl show -P MainPID decoy").strip()
    if decoy not in ("", "0"):
        break
    time.sleep(0.05)
decoy = int(decoy)
assert running(decoy), "the decoy process never started"

policy("next", "-", f"{DIRECTORY}/next.pid", f"{DIRECTORY}/next.child", 0)
launch("tomoe-next", "next", f"{DIRECTORY}/next.lisp")
next_pid = wait_running(f"{DIRECTORY}/next.pid")
assert wait_gone(child), f"a process a dead session left survived the next start: {child}"
assert not exists(dead_socket), f"a dead session's socket survived the next start: {dead_socket}"
assert running(live), "clearing a dead session killed a live session's service"
assert running(decoy), "clearing a dead session killed an unrelated process"
assert running(next_pid)

cli("quit", "next")
machine.wait_until_succeeds("systemctl show -P SubState tomoe-next | grep -qx 'exited\\|failed'",
                            timeout=60)
assert wait_gone(next_pid), f"a service outlived its compositor's quit: {next_pid}"
assert not service_sockets(), f"a finished session left its service's socket: {service_sockets()}"
assert running(live)

cli("quit", "live")
machine.wait_until_succeeds("systemctl show -P SubState tomoe-live | grep -qx 'exited\\|failed'",
                            timeout=60)
assert wait_gone(live), f"a service outlived its compositor's quit: {live}"
machine.execute("systemctl stop decoy 2>/dev/null", check_return=False)
