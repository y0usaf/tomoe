import json

COMPARED = [":EXTENSIONS", ":WINDOW-GEOMETRY", ":LAYOUT", ":BINDINGS", ":TIMERS", ":WATCHES",
            ":EXECUTIONS", ":MANAGED-PROCESSES", ":PATCHES", ":KEYBOARD"]
BOX = "100,60 320x200"


def fail(arguments):
    status, output = machine.execute(ENV + "tomoe --socket check " + arguments)
    assert status != 0, f"expected a failure from {arguments}\n{output}"
    return output.strip()


def quoted(text):
    return "'" + text + "'"


def image(box=BOX):
    return machine.succeed(ENV + f"WAYLAND_DISPLAY=check grim -t ppm -g '{box}' - | sha256sum").split()[0]


def state():
    reply = inspect()
    return {key: show(reply.get(key)) for key in COMPARED}


def entries(key):
    return list(map(plist, items(inspect()[key])))


def names():
    return sorted(text(entry.get(":NAME", '""')) for entry in entries(":EXTENSIONS"))


def layout():
    return text(plist(inspect()[":KEYBOARD"])[":LAYOUT"])


def sleeps():
    found = []
    for pid in machine.succeed(
            "cat /sys/fs/cgroup/system.slice/tomoe.service/cgroup.procs 2>/dev/null || true").split():
        command = machine.execute(f"tr '\\0' ' ' < /proc/{pid}/cmdline")[1]
        if "sleep 3600" in command:
            found.append(command.strip())
    return found


def await_state(predicate, what, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise AssertionError(f"{what} did not happen: " + show(inspect()))


start("--bare")
client("window", "--mode xdg --app-id check-livepatch --title check --size 320x200")
await_state(lambda: items(inspect()[":WINDOW-GEOMETRY"]), "the window to be placed")

def stable(box=BOX, seconds=30):
    deadline = time.monotonic() + seconds
    previous = image(box)
    while time.monotonic() < deadline:
        time.sleep(0.1)
        current = image(box)
        if current == previous:
            return current
        previous = current
    raise AssertionError("the empty region never stopped changing")

before = stable()
before_state = state()
assert image() == before, "the empty region is not stable"
assert entries(":WINDOW-GEOMETRY")[0][":X"] == "0", before_state[":WINDOW-GEOMETRY"]
applied = json.loads(cli(f"command patch file={GAP_PATCH}"))
assert applied["name"] == "patch", applied
assert applied["result"]["extensions"] == ["livepatch-gap"], applied
print("applied", applied["result"])

patched_state = state()
box = entries(":WINDOW-GEOMETRY")[0]
assert (box[":X"], box[":Y"]) == ("100", "60"), box
assert image() != before, "the patch did not change the screen"
assert [text(timer.get(":NAME", '""')) for timer in entries(":TIMERS")] == [":LIVEPATCH-TICK"]
assert "livepatch-gap" in names(), names()
assert "livepatch-gap" in [text(binding.get(":OWNER", '""')) for binding in entries(":BINDINGS")]
await_state(lambda: sleeps(), "the patch's service to run")
await_state(lambda: entries(":MANAGED-PROCESSES")
            and entries(":MANAGED-PROCESSES")[0][":STATUS"] == ":RUNNING", "the service to report")
assert len(entries(":PATCHES")) == 1, patched_state[":PATCHES"]

keyboard = json.loads(cli("msg command " + quoted(json.dumps(
    {"name": "patch", "arguments": {"forms": open(KEYBOARD_PATCH).read(), "name": "kbd"}}))))
assert keyboard["result"]["definitions"] == ["TOMOE::DEFAULT-KEYBOARD-CONFIG"], keyboard
await_state(lambda: layout() == "de", "the patched keyboard layout")
assert image() != before, "the patched keyboard layout did not reach the screen"

newer = json.loads(cli("msg command " + quoted(json.dumps(
    {"name": "patch", "arguments": {"forms": open(KEYBOARD_PATCH).read()
                                                   .replace(':layout "de"', ':layout "fr"'),
                                    "name": "kbd2"}}))))
assert newer["result"]["definitions"] == ["TOMOE::DEFAULT-KEYBOARD-CONFIG"], newer
await_state(lambda: layout() == "fr", "the second keyboard layout")
assert "older than" in fail("command undo-patch name=kbd"), "a redefinition was undone out of order"
assert layout() == "fr", "a refused undo changed the session"

assert json.loads(cli("command undo-patch name=kbd2"))["result"]["warning"] is None
await_state(lambda: layout() == "de", "the first keyboard layout")
undone = json.loads(cli("command undo-patch name=kbd"))
assert undone["result"]["warning"] is None, undone
await_state(lambda: layout() == "", "the restored keyboard layout")

broken = fail("command patch forms=" + quoted(
    '(defun tomoe::default-keyboard-config () (list :rules "" :model "" :layout "fr" '
    ':variant "" :options nil :repeat-rate 25 :repeat-delay 600)) (no-such-function 1)')
    + " name=broken")
assert "NO-SUCH-FUNCTION" in broken.upper(), broken
assert layout() == "", "a rejected patch kept the definition it made"
assert len(entries(":PATCHES")) == 1, "a rejected patch was recorded"

broken = fail("command patch forms=" + quoted(
    '(in-package #:tomoe-user) (define-extension "livepatch-broken" (:reads () :state nil) '
    '(snapshot state event) (declare (ignore snapshot state event)) '
    '(values nil (list (place "x" 0 0 1 1)) nil))') + " name=broken-ext")
assert "livepatch-broken" not in names(), "a rejected extension stayed mounted"
assert machine.execute(ENV + "tomoe --socket check inspect")[0] == 0, "the compositor did not survive"

undone = json.loads(cli("command undo-patch"))
assert undone["result"]["extensions"] == ["livepatch-gap"], undone
await_state(lambda: not entries(":TIMERS") and not entries(":MANAGED-PROCESSES"), "the effects to retire")
await_state(lambda: image() == before, "the screen to return to its state before the patch")

after_state = state()
differences = [f"{key}: {before_state[key]} -> {after_state[key]}"
               for key in COMPARED if before_state[key] != after_state[key]]
assert not differences, "\n".join(differences)
assert not sleeps(), "the patch's service outlived its undo"
assert not entries(":PATCHES"), inspect()[":PATCHES"]
assert {"patch", "undo-patch"} <= {entry["name"] for entry in json.loads(cli("commands"))}
assert not machine.succeed(f"ls -A {RUNTIME}/tomoe-patches 2>/dev/null || true").split(), \
    "generated patch sources remain"

quit()
