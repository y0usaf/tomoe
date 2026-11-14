import re
import time

RUNTIME = "/run/tomoe"
BUS = "unix:path=/run/check-bus"
THREADS = "LP_NUM_THREADS=4"
ENV = f"XDG_RUNTIME_DIR={RUNTIME} DBUS_SESSION_BUS_ADDRESS={BUS} {THREADS} "
TOKEN = re.compile(r'\s*(?:(\()|(\))|("(?:[^"\\]|\\.)*")|([^\s()"]+))')


def read(text):
    stack = [[]]
    for match in TOKEN.finditer(text):
        if match.group(1):
            stack.append([])
        elif match.group(2):
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(match.group(3) or match.group(4))
    return stack[0][0]


def show(value):
    return "(" + " ".join(map(show, value)) + ")" if isinstance(value, list) else value


def plist(value):
    return dict(zip(value[::2], value[1::2])) if isinstance(value, list) else {}


def items(value):
    return value if isinstance(value, list) else []


def text(value):
    return re.sub(r'\\(.)', r'\1', value[1:-1]) if value.startswith('"') else value


def cli(arguments, socket="check"):
    return machine.succeed(ENV + f"tomoe --socket {socket} " + arguments)


def inspect(socket="check"):
    reply = read(cli("inspect", socket))
    assert reply[1] == ":OK", show(reply)
    return plist(reply[2])


def start(*options, bus=True, env=None, backend="headless", unit="tomoe", socket="check"):
    machine.wait_for_unit("multi-user.target")
    if bus:
        machine.execute(
            "systemd-run --unit=check-bus --property=Type=exec "
            f"dbus-daemon --session --nofork --address={BUS}"
        )
        machine.wait_for_file("/run/check-bus")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        "--property=LimitCORE=infinity "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} "
        + (f"--setenv=DBUS_SESSION_BUS_ADDRESS={BUS} " if bus else "")
        + "".join(f"--setenv={key}={value} " for key, value in (env or {}).items())
        + "--setenv=HOME=/root --setenv=PATH=/run/current-system/sw/bin --setenv=MESA_LOADER_DRIVER_OVERRIDE=zink "
        + f"--setenv={THREADS} --setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        f"tomoe --socket {socket} --backend {backend} --no-watch " + " ".join(options)
    )
    for _ in range(600):
        if machine.execute(ENV + f"tomoe --socket {socket} inspect")[0] == 0:
            return
        if machine.execute(f"systemctl show -P SubState {unit} | grep -qx running")[0] != 0:
            raise AssertionError("tomoe stopped while starting:\n" + machine.succeed(f"journalctl -u {unit} -o cat | tail -30"))
        time.sleep(0.1)
    raise AssertionError("tomoe did not answer inspect within 60 s")


def client(unit, *options, socket="check"):
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY={socket} "
        "tomoe-test-client --seconds 3600 " + " ".join(options)
    )
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx mapped", timeout=60)


def processes(unit="tomoe"):
    pids = machine.succeed(f"cat /sys/fs/cgroup/system.slice/{unit}.service/cgroup.procs 2>/dev/null || true").split()
    return {pid: machine.execute(f"tr '\\0' ' ' < /proc/{pid}/cmdline")[1].strip() for pid in pids}


def quit(unit="tomoe", socket="check"):
    cli("quit", socket)
    machine.wait_until_succeeds(f"systemctl show -P SubState {unit} | grep -qx 'exited\\|failed'", timeout=60)
    status = machine.succeed(f"systemctl show -P ExecMainStatus {unit}").strip()
    assert status == "0", f"tomoe exited {status}: " + machine.succeed(f"journalctl -u {unit} -o cat | tail -40")
    left = processes(unit)
    assert not left, f"processes outlived quit: {left}"
