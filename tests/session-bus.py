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
