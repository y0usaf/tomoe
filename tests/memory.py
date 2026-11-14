import os

SHOT = "/run/memory"
machine.succeed(f"rm -rf {SHOT} && cp -r {SOURCE} {SHOT}")
machine.succeed("mkdir -p /run/user/1001/bolo && printf idle > /run/user/1001/bolo/state")


def tomoe_process():
    pids = machine.succeed(
        "cat /sys/fs/cgroup/system.slice/tomoe.service/cgroup.procs 2>/dev/null || true"
    ).split()
    for pid in pids:
        if "tomoe --socket check" in machine.succeed(f"tr '\\0' ' ' < /proc/{pid}/cmdline"):
            return pid
    raise AssertionError("no tomoe process")


def measure(label):
    print(f"--- {label}")
    print(machine.succeed(f"python3 {ATTRIBUTE} {tomoe_process()} {TOMOE_BIN}").strip())
    print("ui:", state_ui())
    print("lisp:", cli("memory"))


def state_ui():
    return inspect()[":NATIVE-UI"]


def shot(name):
    machine.succeed(f"{ENV}WAYLAND_DISPLAY=check grim -t ppm {SHOT}/{name}.ppm")
    return machine.succeed(f"sha256sum {SHOT}/{name}.ppm").split()[0]


def fresh(*options, bus=True):
    machine.execute("systemctl stop check-bus tomoe; systemctl reset-failed check-bus tomoe")
    start(*options, bus=bus)


fresh("--bare")
measure("bare")
quit()

fresh("--config", CONFIG)
time.sleep(3)
state = inspect()
print("config error:", state[":LAST-ERROR"])
print("output errors:", state[":OUTPUT-ERRORS"])
print("extensions:", state[":EXTENSIONS"])
measure("config")
client("a", "--mode xdg --app-id check-a --size 1200x800")
client("b", "--mode xdg --app-id check-b --size 900x700")
time.sleep(3)
measure("windows")
for step in range(1, 5):
    time.sleep(15)
    measure(f"soak {step * 15}s")
for name in ("bar", "shader-wallpaper", "cursor-trail", "glass", "deck"):
    cli(f"unmount {name}")
    time.sleep(1)
    measure(f"without {name}")
for unit in ("a", "b"):
    machine.succeed(f"systemctl stop {unit}")
quit()

fresh("--bare", bus=False)
cli(f"mount {os.path.dirname(TOMOE_BIN)}/../share/tomoe/examples/shell.lisp")
client("c", "--mode xdg --app-id check-c --size 640x480")
time.sleep(1)
one = shot("one")
two = shot("two")
print("scene one:", one)
print("scene two:", two)
assert one == two, "two identical frames rendered differently"
measure("scene")
quit()
