import re
import time

CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
DEVICE = f"/dev/dri/{CARD}"
CONNECTOR = f"/sys/class/drm/{CARD}-Virtual-1"
STATE = "/run/tomoe-session-state"
FRAME = re.compile(r"(?<!-):FRAME (\d+)", re.IGNORECASE)
DRM = {
    "LIBSEAT_BACKEND": "noop",
    "TOMOE_TEST_SESSION": STATE,
}


def session(state):
    machine.succeed(f"echo {state} > {STATE}")


def frames():
    return sum(int(value) for value in FRAME.findall(cli("frames")))


def primary():
    for line in machine.succeed(f"tomoe-planes-probe dump {DEVICE}").splitlines():
        if not line.startswith("plane "):
            continue
        entry = {}
        for word in line.split()[2:]:
            name, _, value = word.partition("=")
            entry[name] = value
        if entry.get("type") == "PRIMARY":
            return entry
    raise AssertionError("the output has no primary plane")


def output():
    reply = read(cli("frames"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(reply[2]):
        fields = plist(entry)
        if text(fields[":NAME"]) == "Virtual-1":
            return fields
    raise AssertionError("the output is missing from frames: " + show(reply))


def wait_for(condition, what, timeout=20):
    deadline = time.monotonic() + timeout
    seen = None
    while time.monotonic() < deadline:
        seen = condition()
        if seen:
            return
        time.sleep(0.1)
    raise AssertionError(f"{what} did not happen; last seen {seen}")


def settle(limit=60):
    previous = frames()
    for _ in range(limit):
        time.sleep(0.2)
        current = frames()
        if current == previous:
            return current
        previous = current
    raise AssertionError("frames never settled")


machine.succeed(f"echo active > {STATE}")
start("--bare", f"--drm_device {DEVICE}", backend="drm", env=DRM)
cli(f"mount {FRAMECLOCK_POLICY}")
settle()
machine.wait_until_succeeds(f"grep -qx enabled {CONNECTOR}/enabled", timeout=30)

base = settle()
client("session-flood", "--app-id", "session-probe", "--commit-rate", "120")
wait_for(lambda: frames() > base + 10, "an active session drew the first frames")
machine.succeed("systemctl stop session-flood")
settle()
before = primary()["fb"]
base = frames()

session("inactive")
time.sleep(0.5)
client("session-flood2", "--app-id", "session-probe", "--commit-rate", "120")
time.sleep(1.0)
assert frames() == base, f"Tomoe drew frames for an inactive session: {base} -> {frames()}"
wait_for(lambda: output()[":SUSPENDED"] == "T", "the seat reported its suspension", timeout=5)
assert output()[":PENDING"] == "NIL", output()
assert primary()["fb"] == before, "the display changed while the session was inactive"
status = machine.succeed(f"cat {CONNECTOR}/enabled").strip()
assert status == "enabled", f"the output left the display while inactive: {status}"

session("active")
wait_for(lambda: frames() > base + 5, "an active session drew frames again", timeout=10)
wait_for(lambda: primary()["fb"] != before, "the resumed frame reached KMS", timeout=10)
assert output()[":SUSPENDED"] == "NIL", output()
assert machine.succeed(f"cat {CONNECTOR}/enabled").strip() == "enabled"
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
log = machine.succeed(
    "journalctl -o cat _SYSTEMD_INVOCATION_ID=$(systemctl show -P InvocationID tomoe)")
assert "commit failed" not in log, log
assert "flip event missing" not in log, log
assert "output Virtual-1 added: connected" in log, log
assert "the seat is inactive; outputs stop drawing" in log, log
assert "the seat is active; outputs draw" in log, log
quit()
