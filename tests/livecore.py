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


def note(message):
    print("livecore: " + message, flush=True)


class Windows:
    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()
        self.processes = []
        self.killed = []

    def start(self, name, directory, environment, *options):
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
            with self.lock:
                self.lines.append(name + ": killed")
            return True
        return False

    def output(self):
        with self.lock:
            return "\n".join(self.lines[-40:])


with tempfile.TemporaryDirectory(prefix="tomoe-livecore-", dir=os.environ.get("TMPDIR")) as root:
    home = pathlib.Path(root) / "home"
    runtime = pathlib.Path(root) / "runtime"
    home.mkdir()
    runtime.mkdir(mode=0o700)
    environment = {key: os.environ[key] for key in
                   ("PATH", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_DRIVERS_PATH",
                    "__EGL_VENDOR_LIBRARY_FILENAMES", "LD_LIBRARY_PATH") if key in os.environ}
    environment.update(HOME=str(home), XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=SOCKET,
                       LIBGL_ALWAYS_SOFTWARE="1")
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

    def frames():
        reply = cli("frames").stdout
        counted = re.findall(r':frame (\d+)', reply, re.IGNORECASE)
        return [int(value) for value in counted] or [0]

    def stamp():
        reply = cli("command", "core-report").stdout
        found = re.search(r'"stamp"\s*:\s*"([^"]*)"', reply)
        if not found:
            found = re.search(r':stamp\s+"([^"]*)"', reply, re.IGNORECASE)
        if not found:
            print("livecore: core-report replied:\n" + reply, flush=True)
        return found.group(1) if found else None

    def offset():
        return min(window["geometry"]["y"] for window in windows())

    def gone(name, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if name not in identities().values():
                return True
            time.sleep(0.05)
        return False

    def swapped(wanted, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if stamp() == wanted:
                return True
            time.sleep(0.05)
        return False

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

    def step():
        before = offset()
        subprocess.run([SCRIPT, "move 100 100", "sync", "sleep 40", "key 30 down", "key 30 up",
                        "sync", "sleep 200"], env=environment, check=True, timeout=60)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if offset() > before:
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
            assert wait_for("ready"), "the pointer never reached a client: " + repr(lines[-6:])
            pointer.stdin.write("shape 4\n")
            pointer.stdin.flush()
            assert wait_for("ok"), "a client's pointer request was not answered: " + repr(lines[-6:])
        finally:
            pointer.kill()
            pointer.wait()

    clients = Windows()
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

        for index in range(3):
            name = "core-" + str(index + 1)
            clients.start(name, root, environment, "--mode", "xdg", "--commit-rate", "60",
                          "--repaint", "--app-id", name, "--title", name,
                          "--size", "320x200")
            assert clients.wait(name + r": mapped", 1, 60), "client " + name + " did not map"
        stamp_a = stamp()
        assert stamp_a, "the running core does not report its build"
        mapped = identities()
        assert sorted(mapped.values()) == ["core-1", "core-2", "core-3"], mapped
        assert drawing(), "the windows were not drawing before the swap"
        assert step(), "a key did not reach the running core before the swap"
        assert drawing(), "the windows stopped drawing after a key before the swap"
        pointer_round()
        note("three windows mapped, drawing and taking keys and the pointer on " + str(stamp_a))

        cli("command", "swap-core", "file=" + CORE_B)
        assert swapped(STAMP_B), "the swapped core reports " + str(stamp())
        assert not clients.exited(), "a client exited during the swap: " + str(clients.exited())
        after = windows()
        assert {window["id"]: window["app_id"] for window in after} == mapped, after
        assert all(window["mapped"] for window in after), after
        assert drawing(), "the windows stopped drawing after the swap"
        assert step(), "a key did not reach the swapped core"
        clients.start("core-4", root, environment, "--mode", "xdg", "--commit-rate", "60",
                      "--repaint", "--app-id", "core-4", "--title", "core-4",
                      "--size", "320x200")
        assert clients.wait(r"core-4: mapped", 1, 60), "a client could not connect to the swapped core"
        assert "core-4" in identities().values(), "a client that connected after the swap is missing"
        mapped = identities()
        assert drawing(), "the windows stopped drawing after a key on the swapped core"
        pointer_round()
        assert clients.kill("core-1"), "the first client could not be killed"
        assert gone("core-1"), "a window whose client died after the swap is still listed"
        mapped = identities()
        assert drawing(), "the session stopped drawing after a client died post-swap"
        assert step(), "a key stopped reaching the core after a client died post-swap"
        assert len(cli("msg", "version").stdout) > 0, "the session stopped answering"
        note("the same windows stay mapped, keep drawing and take keys and the pointer after the swap; "
             "a client that dies afterwards leaves the session drawing and answering")

        bad = cli("command", "swap-core", "file=" + CORE_BAD, check=False)
        assert bad.returncode != 0, "an incompatible core was accepted:\n" + bad.stdout
        assert "layout" in (bad.stdout + bad.stderr), bad.stdout + bad.stderr
        assert stamp() == STAMP_B, "a refused core changed the running core"
        assert identities() == mapped, "a refused core lost the windows"
        assert drawing(), "the windows stopped drawing after a refused swap"
        assert step(), "a key stopped reaching the core after a refused swap"
        pointer_round()
        note("an incompatible core is refused and the session keeps running")

        cli("command", "swap-core", "file=" + CORE_A)
        assert swapped(stamp_a), "the second swap reports " + str(stamp())
        assert identities() == mapped, "the second swap lost the windows"
        assert drawing(), "the windows stopped drawing after the second swap"
        assert step(), "a key stopped reaching the core after a second swap"
        assert clients.kill("core-2"), "the second client could not be killed"
        assert gone("core-2"), "a window whose client died after two swaps is still listed"
        mapped = identities()
        assert drawing(), "the session stopped drawing after a client died after two swaps"
        assert step(), "a key stopped reaching the core after a client died after two swaps"
        assert not clients.exited(), "a client exited during a swap"
        note("a second swap reaches the same core again with the same windows")
    except Exception:
        log.seek(0)
        print("livecore: compositor log:\n" + log.read(), flush=True)
        session = home / ".local" / "state" / "tomoe" / (SOCKET + ".log")
        if session.exists():
            print("livecore: session log:\n" + session.read_text()[-4000:], flush=True)
        print("livecore: client output:\n" + clients.output(), flush=True)
        raise
    finally:
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
