import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import time

MAX_OBJECTS = 65536
FLOOD_SYNCS = 35000


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    return s


def sync_msg(new_id):
    return struct.pack("<III", 1, (12 << 16) | 0, new_id)


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.done = set()
        self.error = None
        self.closed = False

    def pump(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return False
        try:
            data = self.sock.recv(1 << 16)
        except (ConnectionResetError, BrokenPipeError):
            self.closed = True
            return True
        if not data:
            self.closed = True
            return True
        self.buf += data
        while len(self.buf) >= 8:
            obj, word = struct.unpack_from("<II", self.buf)
            size = word >> 16
            opcode = word & 0xFFFF
            if size < 8 or len(self.buf) < size:
                break
            if obj == 1 and opcode == 0:
                self.error = self.buf[:size]
            elif obj != 1 and opcode == 0:
                self.done.add(obj)
            self.buf = self.buf[size:]
        return True


def check_max_objects(path):
    sock = connect(path)
    sock.setblocking(True)
    rd = Reader(sock)
    next_id = 2
    while next_id < MAX_OBJECTS:
        end = min(next_id + 512, MAX_OBJECTS)
        sock.sendall(b"".join(sync_msg(i) for i in range(next_id, end)))
        next_id = end
        while rd.pump(0):
            if rd.closed or rd.error:
                raise SystemExit(f"max-objects: rejected early before id {next_id}")
    deadline = time.monotonic() + 10
    while len(rd.done) < MAX_OBJECTS - 2 and time.monotonic() < deadline:
        rd.pump(0.1)
        if rd.closed or rd.error:
            raise SystemExit("max-objects: rejected a legal id")
    if len(rd.done) < MAX_OBJECTS - 2:
        raise SystemExit(f"max-objects: only {len(rd.done)} of {MAX_OBJECTS - 2} legal syncs answered")
    try:
        sock.sendall(sync_msg(MAX_OBJECTS) + sync_msg(MAX_OBJECTS + 1))
    except (BrokenPipeError, ConnectionResetError):
        pass
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not rd.closed and not rd.error:
        rd.pump(0.1)
        if MAX_OBJECTS in rd.done or MAX_OBJECTS + 1 in rd.done:
            raise SystemExit(f"max-objects: FAIL id {MAX_OBJECTS} was accepted")
    if not (rd.closed or rd.error):
        raise SystemExit("max-objects: FAIL no error and no disconnect")
    sock.close()
    return f"max_obj=rejected@{MAX_OBJECTS} legal={MAX_OBJECTS - 2}"


def check_flood(path):
    sock = connect(path)
    sock.settimeout(5)
    payload = b"".join(sync_msg(i) for i in range(2, 2 + FLOOD_SYNCS))
    sent_all = True
    try:
        sock.sendall(payload)
    except (BrokenPipeError, ConnectionResetError):
        sent_all = False
    time.sleep(1.0)
    rd = Reader(sock)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not rd.closed:
        if not rd.pump(0.5):
            break
    sock.close()
    if not rd.closed:
        raise SystemExit(f"flood: FAIL stalled client with {FLOOD_SYNCS * 24} queued reply bytes was not disconnected")
    return f"flood=disconnected sent_all={sent_all}"


def xwm_pids():
    out = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            with open(f"/proc/{e}/stat") as f:
                st = f.read()
        except OSError:
            continue
        if st[st.find("(") + 1:st.rfind(")")] == "tomoe-xwm":
            out.append(int(e))
    return out


def check_xwm_kill(display, client_bin):
    pids = []
    for _ in range(60):
        pids = xwm_pids()
        if pids:
            break
        time.sleep(0.05)
    if len(pids) != 1:
        raise SystemExit(f"xwm: FAIL expected one tomoe-xwm, found {pids}")
    old = pids[0]
    env = dict(os.environ, WAYLAND_DISPLAY=display)
    honest = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "1", "--windows", "1"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.2)
    os.kill(old, signal.SIGKILL)
    new = None
    for _ in range(100):
        cur = [p for p in xwm_pids() if p != old]
        if cur:
            new = cur[0]
            break
        time.sleep(0.05)
    out, _ = honest.communicate(timeout=20)
    if new is None:
        raise SystemExit("xwm: FAIL tomoe-xwm did not respawn")
    if honest.returncode != 0 or not re.search(r"frames [1-9][0-9]*", out) or not re.search(r"lost 0\b", out):
        raise SystemExit(f"xwm: FAIL client disturbed by sidecar kill: {out}")
    return f"xwm {old}->{new}"


FLUSH_CONFIG = """(in-package #:tomoe-user)
(load-policy "desktop")
(define-extension "e2e-flush" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (bind-key '(:mod) "Tab" "focus-next-window")) nil))
"""
FLUSH_SWITCHES = 6
FLUSH_BOUND_MS = 2.0


def check_idle_flush(tomoe_bin, client_bin):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    config = os.path.join(runtime, "e2e-flush.lisp")
    with open(config, "w") as f:
        f.write(FLUSH_CONFIG)
    sock = "e2e-flush"
    tomoe = subprocess.Popen([tomoe_bin, "--socket", sock, "--backend", "headless", "--config", config,
        "--no-watch"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            if subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.05)
        env = dict(os.environ, WAYLAND_DISPLAY=sock, WAYLAND_DEBUG="client")
        client = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "idle", "--seconds",
            str(16 + FLUSH_SWITCHES * 0.3), "--windows", "2"], env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        lines = []
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            ready, _, _ = select.select([client.stderr], [], [], 0.2)
            if not ready:
                continue
            line = client.stderr.readline()
            if not line:
                break
            lines.append(line)
            if re.search(r"wl_keyboard#\d+\.enter", line):
                break
        if not any(re.search(r"wl_keyboard#\d+\.enter", l) for l in lines):
            client.kill()
            raise SystemExit("idle-flush: FAIL no window took keyboard focus within 15s")
        time.sleep(0.2)
        for _ in range(FLUSH_SWITCHES):
            subprocess.run([tomoe_bin, "--socket", sock, "event",
                '(:type :key :owner "e2e-flush" :command "focus-next-window")'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            time.sleep(0.3)
        client.kill()
        _, rest = client.communicate(timeout=30)
        trace = "".join(lines) + rest
    finally:
        tomoe.terminate()
        tomoe.wait(timeout=10)
    events = []
    for line in trace.splitlines():
        m = re.match(r"\[\s*(\d+\.\d+)\].*?(wl_keyboard#\d+\.enter|xdg_toplevel#\d+\.configure\()", line)
        if m:
            events.append((float(m.group(1)), "enter" if "enter" in m.group(2) else "configure"))
    gaps = []
    for i, (t, kind) in enumerate(events):
        if kind != "enter":
            continue
        after = [t2 for t2, k2 in events[i + 1:i + 6] if k2 == "configure"]
        gaps.append(round(after[0] - t, 3) if after else None)
    switched = gaps[-FLUSH_SWITCHES:]
    if len(switched) < FLUSH_SWITCHES or None in switched:
        raise SystemExit(f"idle-flush: FAIL focus switches without a configure: {gaps} "
            f"(trace lines {len(trace.splitlines())}, enters {trace.count('.enter(')}, "
            f"configures {trace.count('.configure(')}, head {trace.splitlines()[:2]})")
    if max(switched) >= FLUSH_BOUND_MS:
        raise SystemExit(f"idle-flush: FAIL configure stranded after keyboard enter (ms): {switched}")
    return f"idle_flush=max{max(switched)}ms"


X11_TIMEOUT = 30


def x_display():
    for _ in range(100):
        try:
            names = sorted(n for n in os.listdir("/tmp/.X11-unix") if n.startswith("X"))
        except OSError:
            names = []
        if names:
            return ":" + names[0][1:]
        time.sleep(0.1)
    raise SystemExit("x11: FAIL tomoe-xwm never created an X socket")


def run_timeout(cmd, env, data=None, timeout=X11_TIMEOUT):
    return subprocess.run(cmd, env=env, input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=timeout)


def clipboard_x_to_wayland(env, size):
    data = os.urandom(size // 4 * 3).hex().encode()[:size]
    owner = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline:
            got = run_timeout(["wl-paste", "--no-newline"], env).stdout
            if got == data:
                return
            time.sleep(0.2)
        raise SystemExit(f"x11: FAIL X->Wayland clipboard of {size} bytes: got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()


def clipboard_wayland_to_x(env, size):
    data = os.urandom(size // 4 * 3).hex().encode()[:size]
    owner = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"],
        env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        got = b""
        while time.monotonic() < deadline:
            got = run_timeout(["xclip", "-selection", "clipboard", "-o",
                "-target", "UTF8_STRING"], env).stdout
            if got == data:
                return
            time.sleep(0.2)
        raise SystemExit(f"x11: FAIL Wayland->X clipboard of {size} bytes: got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()


def check_x11(path, tomoe_bin):
    sock = os.path.basename(path)
    env = dict(os.environ, WAYLAND_DISPLAY=sock, DISPLAY=x_display())
    logo = subprocess.Popen(["xlogo", "-geometry", "240x120"], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        mapped = False
        while time.monotonic() < deadline and not mapped:
            out = subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True).stdout
            mapped = "xlogo" in out.lower()
            if not mapped:
                time.sleep(0.2)
        if not mapped:
            raise SystemExit("x11: FAIL xlogo never appeared in tomoe inspect")
        clipboard_x_to_wayland(env, 4096)
        clipboard_x_to_wayland(env, 24 << 20)
        clipboard_wayland_to_x(env, 4096)
        clipboard_wayland_to_x(env, 2 << 20)
    finally:
        logo.kill()
        logo.wait()
    return "x11=mapped,clip-x2w(4K,24M),clip-w2x(4K,2M)"


def main():
    args = sys.argv[1:]
    checks = args[0].split(",")
    path = args[1]
    display = os.path.basename(path)
    client_bin = args[2] if len(args) > 2 else None
    results = []
    for check in checks:
        if check == "max-objects":
            results.append(check_max_objects(path))
        elif check == "flood":
            results.append(check_flood(path))
        elif check == "xwm":
            results.append(check_xwm_kill(display, client_bin))
        elif check == "x11":
            results.append(check_x11(path, args[3]))
        elif check == "idle-flush":
            results.append(check_idle_flush(args[3], client_bin))
        else:
            raise SystemExit(f"unknown check {check}")
    print("e2e-fault-isolation: " + " ".join(results))


main()
