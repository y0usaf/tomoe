start("--bare", bus=False)
OWN = "unix:path=/run/tomoe/tomoe.check.bus"
state = inspect()
assert text(state[":SESSION-BUS"]) == OWN, state[":SESSION-BUS"]
machine.succeed(f"DBUS_SESSION_BUS_ADDRESS={OWN} notify-send check-summary")
cli(f"mount {BUS_PROBE}")
machine.wait_until_succeeds("test -s /run/bus-probe", timeout=30)
assert machine.succeed("cat /run/bus-probe") == OWN, machine.succeed("cat /run/bus-probe")
cli("unmount bus-probe")
quit()
machine.fail("test -e /run/tomoe/tomoe.check.bus")
machine.fail("test -e /run/tomoe/dbus-1")

COMPOSITOR = (f"XDG_RUNTIME_DIR={RUNTIME} HOME=/root PATH=/run/current-system/sw/bin "
              f"MESA_LOADER_DRIVER_OVERRIDE=zink {THREADS} LIBGL_ALWAYS_SOFTWARE=1 ")
machine.succeed("install -d -m 0700 " + RUNTIME)
machine.succeed(COMPOSITOR + "tomoe --socket check --backend headless --bare --no-watch "
                "</dev/null >/run/killed.log 2>&1 & echo $! >/run/killed.pid")
for _ in range(600):
    if machine.execute(ENV + "tomoe --socket check inspect")[0] == 0:
        break
    time.sleep(0.1)
else:
    raise AssertionError("tomoe did not start: " + machine.execute("cat /run/killed.log")[1])
for _ in range(600):
    status, listing = machine.execute("pgrep -f '^dbus-daemon .*address=unix:path=/run/tomoe/tomoe.check.bus'")
    bus = listing.split()
    if status == 0 and len(bus) == 1 and machine.execute("test -S /run/tomoe/tomoe.check.bus")[0] == 0:
        break
    time.sleep(0.1)
else:
    raise AssertionError("the private session bus never started")
machine.succeed("kill -9 $(cat /run/killed.pid)")
machine.wait_until_succeeds("test ! -e /run/tomoe/tomoe.check.bus", timeout=5)
machine.wait_until_succeeds(f"test ! -e /proc/{bus[0]}", timeout=5)
machine.fail("pgrep -f '^dbus-daemon .*address=unix:path=/run/tomoe/tomoe.check.bus'")
