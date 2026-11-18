import json
import time

POLICY = """
(in-package #:tomoe-user)
(define-extension "lock-check" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((windows (context snapshot :windows)))
    (values nil
            (append (loop for window in windows collect (place (getf window :id) 0 0 320 240))
                    (list (focus (getf (car (last windows)) :id))))
            nil)))
"""
INPUT = {"pointer_motion", "pointer_button", "key"}


def frames():
    return [frame for frame in
            (json.loads(line) for line in
             machine.succeed("cat /run/lock/frames.jsonl 2>/dev/null || true").splitlines()
             if line.strip())
            if frame.get("event") in INPUT]


def collect(seen, timeout):
    time.sleep(timeout)
    return frames()[seen:]


def script(*commands):
    machine.succeed(ENV + "WAYLAND_DISPLAY=check " + SCRIPT + " " + " ".join(
        f"'{command}'" for command in commands))


machine.succeed("mkdir -p /run/lock /run/capture")
machine.succeed("cat > /run/lock/policy.lisp <<'LISP'\n" + POLICY + "\nLISP\n")

start("--bare")
cli("mount /run/lock/policy.lisp")
machine.succeed(
    "systemd-run --unit=lock-window --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    "tomoe-test-client --seconds 3600 --app-id capture-target --title target "
    "--size 320x240 --color 336699")
machine.wait_until_succeeds("journalctl -u lock-window -o cat | grep -qx mapped", timeout=60)

machine.succeed(
    "systemd-run --unit=lock-reader --property=Type=exec "
    "--setenv=PATH=/run/current-system/sw/bin --setenv=HOME=/root "
    f"{EVENT_READER} "
    f"{RUNTIME}/tomoe.check.sock /run/lock/frames.jsonl /run/lock/subscribed /run/lock/stop")
machine.wait_for_file("/run/lock/subscribed", timeout=30)
time.sleep(0.3)

seen = len(frames())
script("move 300 200", "key 30 down", "key 30 up")
before = collect(seen, 2.0)
keys = [frame for frame in before if frame["event"] == "key"]
assert len(keys) >= 2, f"no key events before the lock: {before}"

machine.succeed(
    "systemd-run --unit=lock-screen --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{LOCK_CLIENT} --seconds 6 --locked-file /run/lock/locked "
    "--unlocked-file /run/lock/unlocked")
machine.wait_for_file("/run/lock/locked", timeout=60)
time.sleep(0.3)
seen = len(frames())
script("move 120 120", "press 272", "release 272", "key 30 down", "key 30 up")
locked = collect(seen, 2.0)
assert not locked, f"input leaked while the session was locked: {locked}"

machine.wait_for_file("/run/lock/unlocked", timeout=60)
time.sleep(0.5)
seen = len(frames())
script("key 30 down", "key 30 up")
after = collect(seen, 3.0)
assert [frame for frame in after if frame["event"] == "key"], f"no key events after unlocking: {after}"
machine.execute("touch /run/lock/stop")
quit()
