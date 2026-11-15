import json
import socket
import time

COLOURS = ["336699"]
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


class Events:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(path)
        self.socket.sendall(json.dumps({"id": 1, "method": "subscribe",
                                        "params": {"events": sorted(INPUT)}}).encode() + b"\n")
        self.buffer = b""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for frame in self.read(0.2):
                if frame.get("id") == 1:
                    return
        raise AssertionError("the subscribe reply never arrived")

    def read(self, timeout):
        frames = []
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                frames.append(json.loads(line))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return frames
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(65536)
            except socket.timeout:
                return frames
            if not chunk:
                return frames
            self.buffer += chunk

    def collect(self, timeout):
        return [frame for frame in self.read(timeout) if "event" in frame]

    def drain(self, timeout=0.3):
        return self.collect(timeout)


def script(*commands):
    machine.succeed(ENV + "WAYLAND_DISPLAY=check " + SCRIPT + " " + " ".join(
        f"'{command}'" for command in commands))


machine.succeed("mkdir -p /run/lock /run/capture")
with open("/run/lock/policy.lisp", "w") as handle:
    handle.write(POLICY)

start("--bare")
cli("mount /run/lock/policy.lisp")
machine.succeed(
    "systemd-run --unit=lock-window --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    "tomoe-test-client --seconds 3600 --app-id capture-target --title target "
    "--size 320x240 --color 336699")
machine.wait_until_succeeds("journalctl -u lock-window -o cat | grep -qx mapped", timeout=60)

events = Events(f"{RUNTIME}/tomoe.check.sock")
time.sleep(0.3)
events.drain()

script("move 300 200", "key 30 down", "key 30 up")
before = events.collect(2.0)
keys = [frame for frame in before if frame["event"] == "key"]
assert len(keys) >= 2, f"no key events before the lock: {before}"

machine.succeed(
    "systemd-run --unit=lock-screen --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{LOCK_CLIENT} --seconds 6 --locked-file /run/lock/locked "
    "--unlocked-file /run/lock/unlocked")
machine.wait_for_file("/run/lock/locked", timeout=60)
events.drain()
script("move 120 120", "press 272", "release 272", "key 30 down", "key 30 up")
locked = events.collect(2.0)
assert not locked, f"input leaked while the session was locked: {locked}"

machine.wait_for_file("/run/lock/unlocked", timeout=60)
time.sleep(0.5)
events.drain()
script("key 30 down", "key 30 up")
after = events.collect(3.0)
assert [frame for frame in after if frame["event"] == "key"], f"no key events after unlocking: {after}"
quit()
