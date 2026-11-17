import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time

TOMOE, CORE_A, CORE_B, CORE_BAD, CLIENT, SCRIPT, POINTER, POLICY, STAMP_B = sys.argv[1:10]
SOCKET = "livecore"
PHASES = (
    ("pre", 183, ("core-1", "core-2", "core-3")),
    ("swapped", 184, ("core-1", "core-2", "core-3")),
    ("closed", 187, ("core-2", "core-3")),
    ("refused", 185, ("core-2", "core-3")),
    ("second", 186, ("core-2", "core-3")),
    ("closed2", 188, ("core-3",)),
)


def note(message):
    print("livecore: " + message, flush=True)


class Windows:
    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()
        self.processes = []
        self.killed = []

    def start(self, name, environment, *options):
        process = subprocess.Popen([CLIENT, "--seconds", "3600", *options], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.processes.append((name, process))

        def read():
            for line in process.stdout:
                with self.lock:
                    self.lines.append(name + ": " + line.strip())

        threading.Thread(target=read, daemon=True).start()
        return process

    def count(self, pattern):
        expression = re.compile(pattern)
        with self.lock:
            return sum(1 for line in self.lines if expression.search(line))

    def wait(self, pattern, count=1, seconds=60):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.count(pattern) >= count:
                return True
            time.sleep(0.05)
        return False

    def exited(self):
        return [name for name, process in self.processes
                if name not in self.killed and process.poll() is not None]

    def kill(self, name):
        for entry, process in self.processes:
            if entry != name:
                continue
            process.kill()
            process.wait(timeout=20)
            self.killed.append(name)
            return True
        return False

    def output(self):
        with self.lock:
            return "\n".join(self.lines[-40:])


class Driver:
    def __init__(self, directory, environment):
        self.directory = pathlib.Path(directory)
        commands = ["sync", "signal " + str(self.directory / "driver-ready")]
        for name, code, _ in PHASES:
            commands += [
                "wait " + str(self.directory / ("go-" + name)),
                "move 20 20", "sync", "move 40 40", "sync", "move 70 70", "sync",
                "move 232 182", "sync", "move 256 206", "sync", "sleep 80",
                "key 30 down", "sync", "sleep 60", "key 30 up", "sync", "sleep 100",
                "key " + str(code) + " down", "sync", "sleep 60",
                "key " + str(code) + " up", "sync", "sleep 150",
                "signal " + str(self.directory / ("done-" + name)),
            ]
        commands += ["wait " + str(self.directory / "driver-stop")]
        self.process = subprocess.Popen([SCRIPT, *commands], env=environment, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.output = []

        def read():
            for line in self.process.stdout:
                self.output.append(line.strip())

        threading.Thread(target=read, daemon=True).start()

    def ready(self, seconds=30):
        deadline = time.monotonic() + seconds
        ready = self.directory / "driver-ready"
        while time.monotonic() < deadline:
            if ready.exists():
                return True
            if self.process.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def run(self, name, seconds=60):
        (self.directory / ("go-" + name)).touch()
        done = self.directory / ("done-" + name)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if done.exists():
                return True
            if self.process.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def stop(self):
        (self.directory / "driver-stop").touch()
        self.process.kill()
        self.process.wait(timeout=20)


with tempfile.TemporaryDirectory(prefix="tomoe-livecore-", dir=os.environ.get("TMPDIR")) as root:
    home = pathlib.Path(root) / "home"
    runtime = pathlib.Path(root) / "runtime"
    home.mkdir()
    runtime.mkdir(mode=0o700)
    environment = {key: os.environ[key] for key in
                   ("PATH", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_DRIVERS_PATH",
                    "__EGL_VENDOR_LIBRARY_FILENAMES", "LD_LIBRARY_PATH") if key in os.environ}
    environment.update(HOME=str(home), XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=SOCKET,
                       LIBGL_ALWAYS_SOFTWARE="1", TMPDIR=root)
    log_path = pathlib.Path(root) / "compositor.log"
    log = open(log_path, "w+")
    compositor = subprocess.Popen([TOMOE, "--backend", "headless", "--socket", SOCKET,
                                   "--config", POLICY, "--no-watch"],
                                  env=environment, stdout=log, stderr=log)

    def cli(*arguments, check=True):
        result = subprocess.run([TOMOE, "--socket", SOCKET, *arguments], env=environment,
                                text=True, capture_output=True)
        if check and result.returncode != 0:
            raise AssertionError("tomoe " + " ".join(arguments) + " exited "
                                 + str(result.returncode) + ":\n" + result.stdout + result.stderr)
        return result

    def windows():
        return json.loads(cli("msg", "windows").stdout)

    def identities():
        return {window["id"]: window["app_id"] for window in windows()}

    def focused_app():
        for window in windows():
            if window.get("focused"):
                return window["app_id"]
        return None

    def frames():
        counted = re.findall(r":frame (\d+)", cli("frames").stdout, re.IGNORECASE)
        return [int(value) for value in counted] or [0]

    def stamp():
        reply = cli("command", "core-report").stdout
        found = re.search(r'"stamp"\s*:\s*"([^"]*)"', reply)
        if not found:
            found = re.search(r':stamp\s+"([^"]*)"', reply, re.IGNORECASE)
        return found.group(1) if found else None

    def steps():
        found = re.search(r":LIVECORE-STEPS\s+(\d+)", cli("inspect").stdout)
        return int(found.group(1)) if found else None

    def drawing(seconds=5):
        before = frames()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            time.sleep(0.25)
            after = frames()
            if before and all(new > old for new, old in zip(after, before)):
                return True
            before = after
        return False

    def swapped(wanted, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if stamp() == wanted:
                return True
            time.sleep(0.05)
        return False

    def gone(name, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if name not in identities().values():
                return True
            time.sleep(0.05)
        return False

    def pointer_round():
        lines = []
        pointer = subprocess.Popen([POINTER], env=environment, text=True, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        def read():
            for line in pointer.stdout:
                lines.append(line.strip())

        threading.Thread(target=read, daemon=True).start()

        def wait_for(word, seconds=30):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if word in lines:
                    return True
                if pointer.poll() is not None:
                    return False
                time.sleep(0.05)
            return word in lines

        try:
            assert wait_for("ready"), "the pointer never reached a new client: " + repr(lines[-6:])
            pointer.stdin.write("shape 4\n")
            pointer.stdin.flush()
            assert wait_for("ok"), "a new client's pointer request was not answered: " + repr(lines[-6:])
        finally:
            pointer.kill()
            pointer.wait()

    def input_round(driver, clients, phase, code, motions):
        target = focused_app()
        assert target, "no window is focused in " + phase
        before_steps = steps()
        key_pattern = target + r": .* key " + str(code) + " [01]"
        before_keys = clients.count(key_pattern)
        before_motion = {name: clients.count(name + r": .* pointer (motion|enter) ")
                         for name in motions}
        assert driver.run(phase), "the input driver for " + phase + " stopped: " \
            + "\n".join(driver.output[-6:])
        assert clients.wait(key_pattern, before_keys + 1, 20), \
            "the window focused in " + phase + " received no key event: " + clients.output()
        for name in motions:
            assert clients.wait(name + r": .* pointer (motion|enter) ", before_motion[name] + 1, 20), \
                name + " received no pointer event in " + phase + ": " + clients.output()
        if before_steps is not None:
            assert steps() == before_steps + 1, \
                "the session's own key binding did not fire in " + phase

    clients = Windows()
    driver = None
    try:
        for _ in range(600):
            if compositor.poll() is not None:
                log.seek(0)
                raise AssertionError("the compositor stopped while starting:\n" + log.read())
            if (runtime / SOCKET).exists() and cli("msg", "version", check=False).returncode == 0:
                break
            time.sleep(0.1)
        else:
            raise AssertionError("the compositor did not answer")

        driver = Driver(root, environment)
        assert driver.ready(), "the input driver never took the seat: " + "\n".join(driver.output)

        for index in range(3):
            name = "core-" + str(index + 1)
            clients.start(name, environment, "--mode", "xdg", "--commit-rate", "60",
                          "--repaint", "--keys", "--pointer", "--app-id", name, "--title", name,
                          "--size", "320x200")
            assert clients.wait(name + r": mapped", 1, 60), "client " + name + " did not map"
        stamp_a = stamp()
        assert stamp_a, "the running core does not report its build"
        mapped = identities()
        assert sorted(mapped.values()) == ["core-1", "core-2", "core-3"], mapped
        assert drawing(), "the windows were not drawing before the swap"
        input_round(driver, clients, "pre", 183, ("core-1", "core-2", "core-3"))
        assert drawing(), "the windows stopped drawing after the first input round"
        pointer_round()
        note("three windows mapped, drawing, and taking a bound key, a key event and the pointer on "
             + str(stamp_a))

        cli("command", "swap-core", "file=" + CORE_B)
        assert swapped(STAMP_B), "the swapped core reports " + str(stamp())
        assert not clients.exited(), "a client exited during the swap: " + str(clients.exited())
        assert {window["id"]: window["app_id"] for window in windows()} == mapped, windows()
        assert all(window["mapped"] for window in windows()), windows()
        assert drawing(), "the windows stopped drawing after the swap"
        input_round(driver, clients, "swapped", 184, ("core-1", "core-2", "core-3"))
        assert drawing(), "the windows stopped drawing after input on the swapped core"
        assert not clients.exited(), "a client exited while the swapped core took input"
        note("the same three windows stay mapped, keep drawing, and take a bound key, a key event "
             "and the pointer after the swap")

        assert clients.kill("core-1"), "the first client could not be killed"
        assert gone("core-1"), "a window whose client died right after the swap is still listed"
        mapped = identities()
        assert sorted(mapped.values()) == ["core-2", "core-3"], mapped
        assert all(window["mapped"] for window in windows()), windows()
        assert drawing(), "the session stopped drawing after a client died on the swapped core"
        assert len(cli("msg", "version").stdout) > 0, "the session stopped answering"
        input_round(driver, clients, "closed", 187, ("core-2", "core-3"))
        assert drawing(), "the session stopped drawing after input following a close"
        assert not clients.exited(), "a client exited after the close on the swapped core"
        note("a client that dies while the swapped core runs leaves the other windows mapped, "
             "drawing and taking input")

        clients.start("core-4", environment, "--mode", "xdg", "--commit-rate", "60", "--repaint",
                      "--keys", "--pointer", "--app-id", "core-4", "--title", "core-4",
                      "--size", "320x200")
        assert clients.wait(r"core-4: mapped", 1, 60), "a client could not connect to the swapped core"
        assert "core-4" in identities().values(), "a client that connected after the swap is missing"
        mapped = identities()
        pointer_round()
        note("a client that connects after the swap maps and takes the pointer")

        bad = cli("command", "swap-core", "file=" + CORE_BAD, check=False)
        assert bad.returncode != 0, "an incompatible core was accepted:\n" + bad.stdout
        assert "layout" in (bad.stdout + bad.stderr), bad.stdout + bad.stderr
        assert stamp() == STAMP_B, "a refused core changed the running core"
        assert identities() == mapped, "a refused core lost the windows"
        assert drawing(), "the windows stopped drawing after a refused swap"
        input_round(driver, clients, "refused", 185, ("core-2", "core-3"))
        note("an incompatible core is refused and the session keeps drawing and taking input")

        cli("command", "swap-core", "file=" + CORE_A)
        assert swapped(stamp_a), "the second swap reports " + str(stamp())
        assert identities() == mapped, "the second swap lost the windows"
        assert drawing(), "the windows stopped drawing after the second swap"
        input_round(driver, clients, "second", 186, ("core-2", "core-3"))
        assert clients.kill("core-2"), "the second client could not be killed"
        assert gone("core-2"), "a window whose client died after two swaps is still listed"
        mapped = identities()
        assert "core-3" in mapped.values(), "the last pre-swap client left with the second close"
        assert drawing(), "the session stopped drawing after a client died after two swaps"
        input_round(driver, clients, "closed2", 188, ("core-3",))
        assert not clients.exited(), "a client exited during a swap"
        note("a second swap returns to the first core, and the pre-swap windows that stay open "
             "keep drawing and taking input after each close")
    except Exception:
        log.seek(0)
        print("livecore: compositor log:\n" + log.read(), flush=True)
        session = home / ".local" / "state" / "tomoe" / (SOCKET + ".log")
        if session.exists():
            print("livecore: session log:\n" + session.read_text()[-4000:], flush=True)
        print("livecore: client output:\n" + clients.output(), flush=True)
        if driver:
            print("livecore: driver output:\n" + "\n".join(driver.output[-10:]), flush=True)
        raise
    finally:
        if driver:
            driver.stop()
        for name, process in clients.processes:
            process.kill()
        compositor.terminate()
        try:
            compositor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            compositor.kill()
            compositor.wait()
        if compositor.returncode != 0:
            log.seek(0)
            print("livecore: compositor log:\n" + log.read(), flush=True)
            session = home / ".local" / "state" / "tomoe" / (SOCKET + ".log")
            if session.exists():
                print("livecore: session log:\n" + session.read_text()[-4000:], flush=True)
        assert compositor.returncode == 0, "the compositor exited " + str(compositor.returncode)
